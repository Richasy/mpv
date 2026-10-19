#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Compare request creation during paused playback on curl and lavf."""

import contextlib
import ctypes
import http.server
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time

from libmpv_curl_settings import options, wait_event
from libmpv_network_cursors import (
    EndFile, EVENT_END_FILE, EVENT_LOG_MESSAGE, LogMessage,
    create_client, get_property, load_libmpv, run_command,
)

MEDIA = bytes(range(256)) * 1000
CHUNK = 8192


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self, mode):
        super().__init__(("127.0.0.1", 0), Handler)
        self.mode = mode
        self.changed = threading.Condition()
        self.requests = []
        self.disconnect = threading.Event()
        self.disconnected = threading.Event()
        self.release = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        match = re.fullmatch(r"bytes=(\d+)-(\d*)",
                             self.headers.get("Range", ""))
        if not match:
            raise RuntimeError("Expected a byte range")
        start = int(match[1])
        with self.server.changed:
            self.server.requests.append(start)
            self.server.changed.notify_all()
        self.close_connection = True
        if (self.server.mode == "disconnect-rate-limit" and start > 0
                and not self.server.release.is_set()):
            self.send_response(429)
            self.send_header("Content-Length", "0")
            self.send_header("Retry-After", "0")
            self.end_headers()
            return
        if start >= len(MEDIA):
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(MEDIA)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        end = min(start + CHUNK, len(MEDIA)) - 1
        partial = (self.server.mode.startswith("disconnect")
                   or self.server.mode == "cancel") and start == 0
        if partial:
            end = len(MEDIA) - 1
        self.send_response(206)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Range", f"bytes {start}-{end}/{len(MEDIA)}")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            size = 144 * 1024 if partial else CHUNK
            self.wfile.write(MEDIA[start:start + size])
            self.wfile.flush()
            if partial:
                self.server.disconnect.wait(10)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        finally:
            if partial:
                self.server.disconnected.set()


@contextlib.contextmanager
def serve(mode):
    server = Server(mode)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        yield server
    finally:
        server.disconnect.set()
        server.release.set()
        server.shutdown()
        worker.join(3)
        server.server_close()
        if worker.is_alive():
            raise RuntimeError("Fixture did not retire")


def run_case(library, backend, mode, verify):
    with serve(mode) as server, contextlib.ExitStack() as stack, \
            tempfile.TemporaryDirectory(prefix="mpv-paused-requests-") as directory:
        output = Path(directory, "audio.pcm")
        mpv = load_libmpv(stack, library)
        with contextlib.ExitStack() as client_scope:
            client = create_client(client_scope, mpv, options(**{
                "ao": "pcm", "ao-pcm-file": str(output),
                "ao-pcm-waveheader": "no", "audio-format": "s16",
                "audio-samplerate": "8000", "audio-channels": "mono",
                "pause": "yes", "keep-open": "no",
                "stream-buffer-size": "4KiB", "curl-buffer-size": "32KiB",
                "demuxer": "rawaudio", "demuxer-rawaudio-rate": "8000",
                "demuxer-rawaudio-channels": "mono",
                "demuxer-rawaudio-format": "s16le",
                "demuxer-readahead-secs": "0", "network-timeout": "3",
                "curl-enabled": "yes" if backend == "curl" else "no",
                "curl-retry-http-429": "yes", "curl-max-retries": "1",
                "stream-lavf-o": "reconnect=1,reconnect_on_network_error=1",
            }))
            lines = []

            def pump(seconds):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    event = mpv.mpv_wait_event(client, 0.02).contents
                    if event.event_id == EVENT_END_FILE:
                        end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
                        raise RuntimeError(f"Paused source ended: {end.reason}/{end.error}")
                    if event.event_id == EVENT_LOG_MESSAGE:
                        log = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
                        text = log.text.decode(errors="replace").strip()
                        if "curl_request " in text or "retrying" in text:
                            lines.append(text)

            run_command(mpv, client, "loadfile",
                        f"http://127.0.0.1:{server.server_port}/media")
            wait_event(mpv, client, 8, 3)
            deadline = time.monotonic() + 3
            while (get_property(mpv, client, "demuxer-cache-idle") != "yes"
                   or get_property(mpv, client, "audio-out-params/samplerate") is None):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Paused demuxer did not settle")
                pump(0.02)
            if get_property(mpv, client, "pause") != "yes":
                raise RuntimeError("Source did not remain paused")
            with server.changed:
                before = list(server.requests)
            server.disconnect.set()
            if mode != "short" and not server.disconnected.wait(2):
                raise RuntimeError("Held connection was not disconnected")
            pump(1)
            with server.changed:
                paused = list(server.requests)
            if verify and backend == "curl":
                if mode != "short" and not any(
                        line.startswith("curl_request end id=1 ")
                        and "curl_result=18 " in line for line in lines):
                    raise RuntimeError("Curl did not observe the forced disconnect")
                if mode != "short" and paused != before:
                    raise RuntimeError(f"Paused source reopened: {before} -> {paused}")
            server.release.set()
            if mode == "cancel":
                run_command(mpv, client, "stop")
            else:
                run_command(mpv, client, "set", "pause", "no")
            end = wait_event(mpv, client, "end", 8)
            expected_reason = 2 if mode == "cancel" else 0
            failed = end["reason"] != expected_reason or end["error"] < 0
            if failed and (verify or mode != "disconnect-rate-limit"):
                raise RuntimeError(f"Resumed source failed: {end}")
        pcm = output.read_bytes()
        if pcm != MEDIA and not failed and mode != "cancel":
            raise RuntimeError(f"Resumed PCM differs ({len(pcm)}/{len(MEDIA)})")
        if mode == "cancel" and server.requests != paused:
            raise RuntimeError("Cancellation started a deferred request")
        print(json.dumps({
            "backend": backend, "mode": mode,
            "before_disconnect": before, "while_paused": paused,
            "total_requests": len(server.requests), "resumed_pcm_exact": pcm == MEDIA,
            "resumed_end": end,
        }), flush=True)


if __name__ == "__main__":
    if len(sys.argv) in (5, 6) and sys.argv[1] == "--child":
        run_case(sys.argv[2], sys.argv[3], sys.argv[4],
                 len(sys.argv) == 6 and sys.argv[5] == "--verify")
    elif len(sys.argv) in (2, 3):
        reports = {}
        for backend in ("lavf", "curl"):
            for mode in ("short", "disconnect", "disconnect-rate-limit", "cancel"):
                result = subprocess.run(
                    [sys.executable, "-B", __file__, "--child",
                     sys.argv[1], backend, mode, *sys.argv[2:]],
                    capture_output=True, text=True, timeout=20)
                if result.returncode:
                    raise RuntimeError(result.stdout + result.stderr)
                report = json.loads(result.stdout.strip().splitlines()[-1])
                reports[backend, mode] = report
                print(result.stdout.strip(), flush=True)
        if len(sys.argv) == 3:
            if (len(reports["curl", "short"]["while_paused"]) >
                    len(reports["lavf", "short"]["while_paused"])):
                raise RuntimeError("Curl short ranges run ahead of the lavf control")
    else:
        raise SystemExit(
            "usage: libmpv_curl_paused_requests.py <libmpv library> [--verify]")
