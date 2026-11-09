#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Loopback-only pre-header connection recovery against the production DLL."""
import contextlib
import ctypes
import importlib.util
import os
from pathlib import Path
import re
import shutil
import socket
import sys
import threading
import time
import urllib.parse

spec = importlib.util.spec_from_file_location(
    "dash_fixture", Path(__file__).with_name("libmpv_dash_source.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


def run_case(mpv, tracks, role, mode):
    if mode == "reset":
        run_case(mpv, tracks, role, "exhaust")
        run_case(mpv, tracks, role, "recover")
        print(f"INITIAL_RECOVERY role={role} mode=new-core-reset PASS", flush=True)
        return
    original = fixture.Handler.do_GET
    lock = threading.Lock()
    remaining = 1 if mode == "recover" else 100
    arrivals = []
    reservation = None
    recovered_server = None
    recovered_thread = None

    def before_headers(handler):
        nonlocal remaining
        current = urllib.parse.urlsplit(handler.path).path.removeprefix("/")
        if current == role and mode not in ("connect", "http-403", "http-412"):
            with lock:
                arrivals.append(handler.headers.get("Range"))
                if remaining > 0:
                    remaining -= 1
                    handler.close_connection = True
                    return
        original(handler)

    fixture.Handler.do_GET = before_headers
    try:
        with fixture.serve(tracks) as server:
            if mode.startswith("http-"):
                server.deny = role
                server.deny_code = int(mode.removeprefix("http-"))
            retries = 0 if mode == "disabled" else 100 if mode == "deadline" else 2
            timeout = 1.2 if mode == "deadline" else 6
            with fixture.client(mpv, {
                "pause": "no", "speed": "100", "keep-open": "no",
                "network-timeout": str(timeout), "curl-max-retries": str(retries),
                "demuxer-lavf-o": "use_mfra_for=0",
            }) as handle:
                source = fixture.source_for(server)
                if mode == "connect":
                    reservation = socket.socket()
                    reservation.bind(("127.0.0.1", 0))
                    port = reservation.getsockname()[1]
                    getattr(source, role).url = f"http://127.0.0.1:{port}/{role}".encode()
                assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
                generation = fixture.snapshot(mpv, handle).generation
                counts = []
                loaded = False
                stopped = False
                started = time.monotonic()
                deadline = started + 15
                while time.monotonic() < deadline:
                    event = mpv.mpv_wait_event(handle, 0.02).contents
                    if event.event_id == fixture.LOG_MESSAGE:
                        message = ctypes.cast(event.data, ctypes.POINTER(fixture.LogMessage)).contents
                        text = ctypes.string_at(message.text).decode("utf-8", "replace")
                        retry = re.search(
                            r"DASH initial connection failed \(curl (\d+)\), retrying \(#(\d+)\)", text)
                        if retry:
                            counts.append(int(retry[2]))
                            if mode == "connect" and recovered_server is None:
                                assert int(retry[1]) == 7, "The fixture must exercise actual connect refusal"
                                reservation.close()
                                reservation = None
                                recovered_server = fixture.Server(
                                    tracks, address=("127.0.0.1", port))
                                recovered_thread = threading.Thread(
                                    target=recovered_server.serve_forever, daemon=True)
                                recovered_thread.start()
                            if mode == "stop" and counts[-1] == 2:
                                fixture.command(mpv, handle, "stop")
                                stopped = True
                    if event.event_id == fixture.FILE_LOADED:
                        loaded = True
                    if event.event_id != fixture.END_FILE:
                        continue
                    state = fixture.snapshot(mpv, handle)
                    reason = ctypes.cast(
                        event.data, ctypes.POINTER(fixture.EndFile)).contents.reason
                    assert state.generation == generation
                    if mode in ("recover", "connect"):
                        assert reason == 0 and state.failure == 0 and loaded, (
                            "Initial connection did not recover", role, mode, reason,
                            state.failure, counts, fixture.prop(mpv, handle, "rodel-dash-transfer"))
                        assert counts and counts[0] == 1, counts
                        if mode != "connect":
                            assert counts == [1], counts
                    elif mode == "stop":
                        assert stopped and reason == 2 and state.failure == 0
                        settled = len(arrivals)
                        for _ in range(15):
                            mpv.mpv_wait_event(handle, 0.1)
                        assert len(arrivals) == settled, "Stopped initial request was retried"
                    else:
                        expected = fixture.AUTH if mode == "http-403" else fixture.RISK if mode == "http-412" else fixture.TRANSPORT
                        assert reason == fixture.END_ERROR and state.failure == expected, (
                            role, mode, reason, state.failure, counts)
                        assert state.failed_track == (fixture.VIDEO if role == "video" else fixture.AUDIO)
                        if mode in ("disabled", "http-403", "http-412"):
                            assert not counts, counts
                        elif mode == "exhaust":
                            assert counts == [1, 2], counts
                        elif mode == "deadline":
                            assert time.monotonic() - started < timeout + 1
                            assert len(counts) < retries
                    fixture.assert_private(mpv, handle)
                    print(f"INITIAL_RECOVERY role={role} mode={mode} retries={counts} PASS", flush=True)
                    return
                raise AssertionError(("Initial recovery did not settle", role, mode, counts))
    finally:
        fixture.Handler.do_GET = original
        if reservation is not None:
            reservation.close()
        if recovered_server is not None:
            recovered_server.shutdown()
            recovered_server.server_close()
            recovered_thread.join(timeout=3)


def main():
    runtime = Path(sys.argv[1]).resolve()
    directory = runtime / f"initial-generated-{os.getpid()}"
    directory.mkdir()
    try:
        tracks = fixture.generate_tracks(directory)
        with contextlib.ExitStack() as stack:
            stack.enter_context(os.add_dll_directory(str(runtime)))
            mpv = ctypes.CDLL(str(runtime / "libmpv-2.dll"))
            fixture.configure_library(mpv)
            assert not hasattr(mpv, "mpv_dash_test_control")
            modes = ("connect",) if "--positive-only" in sys.argv else (
                "recover", "connect", "exhaust", "disabled", "deadline",
                "stop", "http-403", "http-412", "reset")
            for role in ("video", "audio"):
                for mode in modes:
                    run_case(mpv, tracks, role, mode)
        print("DASH_INITIAL_RECOVERY_PASS", flush=True)
    finally:
        assert directory.parent == runtime and directory.name.startswith("initial-generated-")
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
