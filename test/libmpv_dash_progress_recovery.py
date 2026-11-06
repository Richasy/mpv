#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Loopback-only repeated body interruptions against the production runtime."""
import contextlib
import ctypes
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import time

spec = importlib.util.spec_from_file_location(
    "dash_fixture", Path(__file__).with_name("libmpv_dash_source.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def run_case(mpv, tracks, role, mode):
    with fixture.serve(tracks) as server:
        server.disconnect_role = role
        server.disconnect_attempts = 7 if mode == "progress" else 100
        server.disconnect_bytes = 1048576 if mode == "progress" else 1
        if mode == "progress":
            server.disconnect_min_payload = 1048577
        retries = 0 if mode == "disabled" else 5
        with fixture.client(mpv, {
            "pause": "no", "speed": "100", "keep-open": "no",
            "network-timeout": "6", "curl-max-retries": str(retries),
            "demuxer-lavf-o": "use_mfra_for=0",
        }) as handle:
            source = fixture.source_for(server)
            source.duration_seconds = 450
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            generation = fixture.snapshot(mpv, handle).generation
            maximum = 0.0
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.02).contents
                position = fixture.prop(mpv, handle, "time-pos")
                if position:
                    maximum = max(maximum, float(position))
                if event.event_id != fixture.END_FILE:
                    continue
                reason = ctypes.cast(
                    event.data, ctypes.POINTER(fixture.EndFile)).contents.reason
                state = fixture.snapshot(mpv, handle)
                requests = server.requests_for(role)
                offsets = [request[1] for request in requests]
                assert state.generation == generation, "Recovery recreated the source"
                if mode == "progress":
                    assert reason == 0 and state.failure == 0 and maximum > 440, (
                        "Progressing transfers exhausted lifetime retries",
                        role, reason, state.failure, maximum, offsets,
                        fixture.prop(mpv, handle, "rodel-dash-transfer"))
                    assert len(server.disconnections) == 7, server.disconnections
                    continuations = [start + received for start, received
                                     in server.disconnections if start > 0]
                    assert len(continuations) >= 6, server.disconnections
                    for offset in continuations:
                        assert offset in offsets, (offset, offsets)
                else:
                    assert reason == fixture.END_ERROR
                    assert state.failure == fixture.TRANSPORT
                    assert offsets == [i * server.disconnect_bytes
                                       for i in range(retries + 1)], offsets
                settled = len(server.requests)
                for _ in range(5):
                    mpv.mpv_wait_event(handle, 0.02)
                assert len(server.requests) == settled, "Settled source kept requesting"
                fixture.assert_private(mpv, handle)
                print(f"BODY_PROGRESS role={role} mode={mode} "
                      f"requests={len(offsets)} same_generation=1 PASS", flush=True)
                return
            raise AssertionError(("Progress case did not settle", role, mode))


def main():
    runtime = Path(sys.argv[1]).resolve()
    directory = runtime / f"progress-generated-{os.getpid()}"
    directory.mkdir()
    try:
        tracks = fixture.generate_tracks(directory, duration=450)
        assert all(len(data) > 8 * 1048576 for data in tracks.values())
        with contextlib.ExitStack() as stack:
            stack.enter_context(os.add_dll_directory(str(runtime)))
            mpv = ctypes.CDLL(str(runtime / "libmpv-2.dll"))
            fixture.configure_library(mpv)
            assert not hasattr(mpv, "mpv_dash_test_control")
            for role in ("video", "audio"):
                for mode in ("progress", "tiny", "disabled"):
                    run_case(mpv, tracks, role, mode)
            if "--positive-only" not in sys.argv:
                short = fixture.generate_tracks(directory)
                for role in ("video", "audio"):
                    for rejection in ("budget", "zero-progress", "unseekable",
                                      "wrong-range", "wrong-total", 200, 401, 403, 412):
                        with fixture.serve(short) as server:
                            fixture.test_interrupted_transfer(
                                mpv, server, role, rejection=rejection)
                        print(f"BODY_NEGATIVE role={role} "
                              f"case={rejection} PASS", flush=True)
        print("DASH_PROGRESS_RECOVERY_PASS", flush=True)
    finally:
        assert directory.parent == runtime
        assert directory.name.startswith("progress-generated-")
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
