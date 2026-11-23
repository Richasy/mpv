#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Real loopback TLS negotiation failures; unchanged trust policy on retry."""
import contextlib
import ctypes
import importlib.util
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import sys
import threading
import time

spec = importlib.util.spec_from_file_location(
    "dash_fixture", Path(__file__).with_name("libmpv_dash_source.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

EVENT = re.compile(
    r"^DASH recovery: stream=(\d+) track=(\d+) phase=(\d+) "
    r"action=(\d+) reason=(\d+) curl=(-?\d+) attempt=(\d+) limit=(\d+) "
    r"delay_ms=(\d+) remaining_ms=(-?\d+)\n$")


def run_case(mpv, tracks, certificates, mode):
    with fixture.serve(tracks) as server:
        server.secure = True
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(*map(str, certificates["server"]))
        original_accept = server.get_request
        failures = 1 if mode == "recover" else 0 if mode == "certificate" else 100

        def accept():
            nonlocal failures
            connection, address = original_accept()
            connection.settimeout(2)
            if failures:
                failures -= 1
                # Consume the actual ClientHello, then abort before ServerHello.
                connection.recv(4096)
                connection.shutdown(socket.SHUT_RDWR)
                connection.close()
                raise OSError("synthetic negotiation abort")
            return context.wrap_socket(connection, server_side=True), address

        server.get_request = accept
        options = {
            "pause": "no", "speed": "100", "keep-open": "no",
            "tls-verify": "yes", "network-timeout": "1.2" if mode == "deadline" else "6",
            "curl-max-retries": "0" if mode == "disabled" else "100" if mode == "deadline" else "2",
            "demuxer-lavf-o": "use_mfra_for=0",
        }
        if mode != "certificate":
            options["tls-ca-file"] = str(certificates["server"][0])
        events = []
        with fixture.client(mpv, options) as handle:
            source = fixture.source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            started = time.monotonic()
            while time.monotonic() - started < 15:
                event = mpv.mpv_wait_event(handle, 0.02).contents
                if event.event_id == fixture.LOG_MESSAGE:
                    log = ctypes.cast(event.data, ctypes.POINTER(fixture.LogMessage)).contents
                    text = ctypes.string_at(log.text).decode("utf-8", "replace")
                    if text.startswith("DASH recovery:"):
                        match = EVENT.fullmatch(text)
                        assert match, text
                        row = tuple(map(int, match.groups()))
                        events.append(row)
                        if mode == "cancel" and row[3] == 1 and row[6] == 2:
                            fixture.command(mpv, handle, "stop")
                if event.event_id != fixture.END_FILE:
                    continue
                reason = ctypes.cast(event.data, ctypes.POINTER(fixture.EndFile)).contents.reason
                state = fixture.snapshot(mpv, handle)
                scheduled = [row for row in events if row[3] == 1]
                actual = [row for row in events if row[3] == 3]
                assert all(row[5] == 35 for row in scheduled), events
                assert all(any(row[:3] == admitted[:3] and row[4:9] == admitted[4:9]
                               for admitted in scheduled) for row in actual), events
                if mode == "recover":
                    assert reason == 0 and state.failure == 0 and scheduled and actual, events
                elif mode == "cancel":
                    assert reason == 2 and state.failure == 0, (reason, state.failure, events)
                    assert len(actual) < len(scheduled), events
                else:
                    assert reason == fixture.END_ERROR and state.failure == fixture.TRANSPORT
                    if mode == "certificate":
                        assert not scheduled and any(r[4:6] == (7, 60) for r in events), events
                    elif mode == "disabled":
                        assert not scheduled and any(r[4] == 2 for r in events), events
                    elif mode == "exhaust":
                        assert any(r[4] == 3 for r in events), events
                    elif mode == "deadline":
                        assert time.monotonic() - started < 2.5
                fixture.assert_private(mpv, handle)
                print(f"TLS_RECOVERY mode={mode} scheduled={len(scheduled)} actual={len(actual)} PASS", flush=True)
                return
            raise AssertionError(("TLS case did not settle", mode, events))


def main():
    runtime = Path(sys.argv[1]).resolve()
    directory = runtime / f"tls-recovery-generated-{os.getpid()}"
    directory.mkdir()
    try:
        certificates = fixture.generate_tls_certificates(directory)
        tracks = fixture.generate_tracks(directory)
        with contextlib.ExitStack() as stack:
            stack.enter_context(os.add_dll_directory(str(runtime)))
            mpv = ctypes.CDLL(str(runtime / "libmpv-2.dll"))
            fixture.configure_library(mpv)
            for mode in ("recover", "certificate", "disabled", "exhaust", "deadline", "cancel"):
                run_case(mpv, tracks, certificates, mode)
        print("DASH_TLS_RECOVERY_PASS", flush=True)
    finally:
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
