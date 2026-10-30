#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Local typed-DASH continuation header stalls; no remote service or real media.

The old pinned DLL must fail the recovery assertion. Run against the complete
native runtime directory produced by the local build gate.
"""
import contextlib
import ctypes
import importlib.util
import os
from pathlib import Path
import re
import shutil
import sys
import threading
import time
import urllib.parse

spec = importlib.util.spec_from_file_location(
    "dash_fixture", Path(__file__).with_name("libmpv_dash_source.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def run_case(mpv, tracks, role, mode, ordinary_other=False):
    original = fixture.Handler.do_GET
    release = threading.Event()
    blocked = threading.Event()
    lock = threading.Lock()
    attempts = []
    held_offset = None
    stalled_at = None

    def delayed(handler):
        nonlocal held_offset, stalled_at
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", handler.headers.get("Range", ""))
        current_role = urllib.parse.urlsplit(handler.path).path.removeprefix("/")
        start = int(match[1]) if match else -1
        with lock:
            attempts.append((current_role, start))
            threshold = (190000 if role == "video" else 130000) if mode == "paused-prefetch" else 131072
            first = current_role == role and threshold <= start < len(tracks[role]) // 2 and not blocked.is_set() and (mode != "paused-prefetch" or start % 16384 != 0)
            if first:
                held_offset = start
                stalled_at = time.monotonic()
                blocked.set()
            repeated = current_role == role and start == held_offset and not first
            hold = first or (repeated and mode == "permanent")
            if repeated and not hold:
                if mode.startswith("http-"):
                    handler.server.deny = role
                    handler.server.deny_code = int(mode.removeprefix("http-"))
                elif mode == "wrong-range":
                    handler.server.bad_range = role
                elif mode == "wrong-total":
                    handler.server.wrong_total_role = role
                elif mode == "range-200":
                    handler.server.arm_failure = role
                    handler.server.arm_code = 200
                release.set()
        if hold:
            if mode == "empty-reply":
                handler.close_connection = True
                return
            release.wait(30 if mode == "paused-prefetch" else 15)
            if mode == "paused-prefetch":
                original(handler)
                return
            handler.close_connection = True
            return
        original(handler)

    fixture.Handler.do_GET = delayed
    try:
        with fixture.serve(tracks) as server:
            if ordinary_other:
                server.full_body_roles.add("audio" if role == "video" else "video")
            try:
                with fixture.client(mpv, {
                    "pause": "yes" if mode == "paused-prefetch" else "no", "speed": "4", "keep-open": "no",
                    "network-timeout": "4", "curl-max-retries": "0" if mode == "budget-zero" else "2",
                    "curl-max-request-size": "16KiB" if mode == "paused-prefetch" else "64KiB", "demuxer-lavf-o": "use_mfra_for=0",
                    "demuxer-readahead-secs": "0.1" if mode == "paused-prefetch" else "1",
                }) as handle:
                    source = fixture.source_for(server)
                    assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
                    generation = fixture.snapshot(mpv, handle).generation
                    loaded = False
                    maximum = 0.0
                    stopped = False
                    prefetch_proved = False
                    safe_diagnostics = []
                    deadline = time.monotonic() + (35 if mode == "paused-prefetch" else 15)
                    while time.monotonic() < deadline:
                        event = mpv.mpv_wait_event(handle, 0.02).contents
                        if mode == "paused-prefetch" and event.event_id == fixture.LOG_MESSAGE:
                            message = ctypes.cast(event.data, ctypes.POINTER(fixture.LogMessage)).contents
                            text = ctypes.string_at(message.text).decode("utf-8", "replace")
                            if "curl_request" in text:
                                safe_diagnostics.append(dict(re.findall(r"(offset|buffered|read_waiting|headers|body)=(\d+)", text)))
                            if "curl_request pending" in text and f"offset={held_offset} " in text:
                                values = dict(re.findall(r"(buffered|read_waiting|headers|body)=(\d+)", text))
                                if int(values.get("buffered", "0")) > 0 and values.get("read_waiting") == "0" and values.get("headers") == "0" and values.get("body") == "0":
                                    prefetch_proved = True
                                    assert len([start for current, start in attempts if current == role and start == held_offset]) == 1
                                    assert fixture.snapshot(mpv, handle).failure == 0
                                    release.set()
                                    fixture.command(mpv, handle, "set", "pause", "no")
                        if event.event_id == fixture.FILE_LOADED:
                            loaded = True
                        if mode == "stop" and blocked.is_set() and not stopped:
                            fixture.command(mpv, handle, "stop")
                            stopped = True
                        if event.event_id == fixture.END_FILE:
                            state = fixture.snapshot(mpv, handle)
                            reason = ctypes.cast(event.data, ctypes.POINTER(fixture.EndFile)).contents.reason
                            assert blocked.is_set(), "Fixture did not reach a continuation"
                            assert state.generation == generation, "Recovery created a new source generation"
                            repeated = [start for current, start in attempts if current == role and start == held_offset]
                            if mode in ("recover", "paused-prefetch"):
                                assert reason == 0 and state.failure == 0 and loaded and maximum >= 12, (
                                    "Header stall terminated playback", reason, state.failure, maximum,
                                    fixture.prop(mpv, handle, "rodel-dash-transfer"))
                                assert len(repeated) == (1 if mode == "paused-prefetch" else 2), "Unexpected continuation request count"
                                if mode == "paused-prefetch":
                                    assert prefetch_proved, "Paused nonempty-buffer/no-reader premise was not observed"
                            elif mode == "stop":
                                assert stopped and reason == 2 and state.failure == 0
                                assert len(repeated) == 1, "A canceled continuation was retried"
                            else:
                                expected = fixture.AUTH if mode in ("http-401", "http-403") else fixture.RISK if mode == "http-412" else fixture.HTTP_RANGE if mode in ("wrong-range", "wrong-total", "range-200") else fixture.TRANSPORT
                                assert reason == fixture.END_ERROR and state.failure == expected, (reason, state.failure, expected)
                                assert state.failed_track == (fixture.VIDEO if role == "video" else fixture.AUDIO)
                                if mode in ("permanent", "budget-zero"):
                                    assert time.monotonic() - stalled_at < 6, "Recovery extended the consumer deadline"
                                    assert len(repeated) <= (1 if mode == "budget-zero" else 2)
                                elif mode == "empty-reply":
                                    assert len(repeated) == 1, "An immediate non-timeout failure was retried"
                                else:
                                    assert len(repeated) == 2
                            settled = len(attempts)
                            for _ in range(5):
                                mpv.mpv_wait_event(handle, 0.05)
                            assert len(attempts) == settled, "A settled source submitted more requests"
                            fixture.assert_private(mpv, handle)
                            print(f"HEADER_STALL role={role} mode={mode} ordinary_other={int(ordinary_other)} loaded={int(loaded)} repeated_offset={held_offset} attempts={len(repeated)} same_generation=1 PASS", flush=True)
                            return
                        position = fixture.prop(mpv, handle, "time-pos")
                        if position:
                            maximum = max(maximum, float(position))
                    raise AssertionError(("Header stall did not settle", role, mode, held_offset, len(attempts), safe_diagnostics[-15:]))
            finally:
                release.set()
    finally:
        fixture.Handler.do_GET = original


def main():
    runtime = Path(sys.argv[1]).resolve()
    directory = runtime / f"header-stall-generated-{os.getpid()}"
    directory.mkdir()
    try:
        tracks = fixture.generate_tracks(directory)
        with contextlib.ExitStack() as stack:
            stack.enter_context(os.add_dll_directory(str(runtime)))
            mpv = ctypes.CDLL(str(runtime / "libmpv-2.dll"))
            fixture.configure_library(mpv)
            assert not hasattr(mpv, "mpv_dash_test_control"), "Use the production DLL"
            modes = ("recover",) if len(sys.argv) > 2 and sys.argv[2] == "--positive" else (
                "recover", "permanent", "budget-zero", "stop", "empty-reply", "range-200", "wrong-range", "wrong-total", "http-401", "http-403", "http-412", "paused-prefetch")
            for role in ("video", "audio"):
                for mode in modes:
                    run_case(mpv, tracks, role, mode)
            if len(sys.argv) == 2:
                run_case(mpv, tracks, "video", "recover", ordinary_other=True)
        print("DASH_HEADER_STALL_RECOVERY_PASS", flush=True)
    finally:
        # Only the unique directory created by this invocation contains generated media.
        assert directory.parent == runtime and directory.name.startswith("header-stall-generated-")
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
