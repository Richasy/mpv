#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Loopback regressions for data-wait budgets and latency-transparent caching."""

import contextlib
import ctypes
import http.server
import json
import re
import subprocess
import sys
import threading
import time

sys.dont_write_bytecode = True

from libmpv_network_cursors import (
    EndFile, EVENT_END_FILE, EVENT_LOG_MESSAGE, EVENT_PLAYBACK_RESTART, LogMessage,
    create_client, load_libmpv, run_command,
)
from libmpv_network_diagnostics import matroska_fixture

FILE_LOADED = 8
EOF = 0
STOP = 2
ERROR = 4
WAIT_BUDGET = 1.5
CASES = ("min-fetch", "tail-prefetch", "timeout", "retry", "mkv-error",
         "zero-timeout", "progress", "progress-seek", "healthy")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self, mode):
        super().__init__(("127.0.0.1", 0), Handler)
        self.mode = mode
        self.media, _ = matroska_fixture(
            include_seek_head=False, seconds=100, chunk_seconds=1)
        if mode == "progress-seek":
            self.media = bytes(1600000)
        self.release = threading.Event()
        self.held = threading.Event()
        self.lock = threading.Lock()
        self.requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        server = self.server
        with server.lock:
            server.requests += 1
            request = server.requests
        if server.mode in ("retry", "mkv-error") and request > 1:
            self.send_response(502 if server.mode == "retry" else 403)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        media = server.media
        start = 0
        end = len(media) - 1
        range_header = self.headers.get("Range")
        if range_header:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", range_header)
            if not match:
                self.send_error(416)
                return
            start = int(match[1])
            if match[2]:
                end = min(end, int(match[2]))
        if start > end:
            self.send_error(416)
            return
        self.send_response(206 if range_header else 200)
        self.send_header("Content-Type", "video/x-matroska")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Connection", "close")
        if range_header:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
        self.end_headers()
        self.close_connection = True
        try:
            if server.mode in ("min-fetch", "tail-prefetch", "progress-seek"):
                prefix = 4096
            else:
                prefix = 65536
            offset = min(end + 1, start + prefix)
            self.wfile.write(media[start:offset])
            self.wfile.flush()
            server.held.set()

            if server.mode in ("retry", "mkv-error"):
                return
            if server.mode not in ("healthy", "progress", "progress-seek"):
                if not server.release.wait(12):
                    return
            while offset <= end:
                chunk = 1024 if server.mode == "progress-seek" else 16384
                next_offset = min(end + 1, offset + chunk)
                self.wfile.write(media[offset:next_offset])
                self.wfile.flush()
                offset = next_offset
                if server.mode == "progress" and server.release.wait(0.03):
                    break
                if server.mode == "progress-seek" and server.release.wait(0.04):
                    break
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


@contextlib.contextmanager
def serve(mode):
    server = Server(mode)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield server
    finally:
        server.release.set()
        server.shutdown()
        worker.join(timeout=3)
        server.server_close()
        if worker.is_alive():
            raise RuntimeError("Loopback fixture did not stop")


def run_case(library, backend, mode):
    with serve(mode) as server, contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        options = {
            "ao": "null",
            "ao-null-untimed": "yes",
            "keep-open": "no",
            "sid": "no",
            "network-timeout": str(WAIT_BUDGET),
            "curl-enabled": "yes" if backend == "curl" else "no",
            "stream-lru-cache-cursors": "1",
            "stream-lru-cache-tail-prefetch": "0",
            "stream-lavf-o": (
                "reconnect=1,reconnect_on_network_error=1,"
                "reconnect_on_http_error=502,reconnect_delay_max=30"),
        }
        if mode == "tail-prefetch":
            options.update({
                "stream-lru-cache-min-fetch": "0",
                "stream-lru-cache-tail-prefetch": "4MiB",
                "stream-lru-cache-tail-threshold": "64MiB",
            })
        if mode == "zero-timeout":
            options["network-timeout"] = "0"
        if mode == "progress-seek":
            options.update({
                "pause": "yes",
                "cache": "no",
                "stream-lru-cache": "0",
                "stream-buffer-size": "4KiB",
                "network-timeout": "0.25",
                "demuxer-readahead-secs": "0",
                "demuxer": "rawaudio",
                "demuxer-rawaudio-rate": "8000",
                "demuxer-rawaudio-channels": "mono",
                "demuxer-rawaudio-format": "s16le",
            })
        if mode == "mkv-error":
            options["stream-lavf-o"] = "reconnect=0,reconnect_on_network_error=0"
        client = create_client(stack, mpv, options)
        if mpv.mpv_request_log_messages(client, b"debug") < 0:
            raise RuntimeError("Native diagnostic subscription failed")
        lines = []

        def pump(seconds, until):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(client, 0.02).contents
                if event.event_id == EVENT_LOG_MESSAGE:
                    message = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
                    text = message.text.decode("utf-8", errors="replace").rstrip()
                    lines.append(text)
                    if isinstance(until, str) and until in text:
                        return None
                if event.event_id == EVENT_END_FILE:
                    end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
                    result = (end.reason, end.error)
                    if until != EVENT_END_FILE:
                        raise RuntimeError(f"Unexpected end-file {result}\n"
                                           + "\n".join(lines[-15:]))
                    return result
                if event.event_id == until:
                    return None
            raise TimeoutError(f"Did not receive event {until}\n"
                               + "\n".join(lines[-15:]))

        url = f"http://127.0.0.1:{server.server_port}/media"
        run_command(mpv, client, "loadfile", url)
        try:
            if mode in ("min-fetch", "tail-prefetch"):
                marker = ("lru_cache: fetch_at" if mode == "min-fetch"
                          else "lru_cache: tail prefetch interrupted")
                pump(1.0, marker)
                if not server.held.is_set() or server.release.is_set():
                    raise RuntimeError("File loaded without exercising the held body")
                server.release.set()
                result = pump(8, EVENT_END_FILE)
                if result[0] != EOF:
                    raise RuntimeError(f"Resumed fixture did not finish normally: {result}")
            elif mode == "zero-timeout":
                pump(2, FILE_LOADED)
                started = time.monotonic()
                while time.monotonic() - started < WAIT_BUDGET + 0.5:
                    event = mpv.mpv_wait_event(client, 0.02).contents
                    if event.event_id == EVENT_END_FILE:
                        raise RuntimeError("network-timeout=0 expired the data wait")
                run_command(mpv, client, "stop")
                result = pump(2, EVENT_END_FILE)
                if result[0] != STOP or server.release.is_set():
                    raise RuntimeError(f"Cancellation was not preserved: {result}")
            elif mode == "progress-seek":
                pump(4, FILE_LOADED)
                run_command(mpv, client, "seek", "2", "absolute+exact")
                pump(4, EVENT_PLAYBACK_RESTART)
                if backend == "lavf":
                    seeks = [
                        float(match[1])
                        for line in lines
                        if (match := re.search(
                            r"network_io end .*operation=seek .*elapsed_ms=([\d.]+)",
                            line))
                    ]
                    if not any(elapsed > 250 for elapsed in seeks):
                        raise RuntimeError("Fixture did not exercise a long internal seek\n"
                                           + "\n".join(lines[-20:]))
            else:
                started = time.monotonic()
                result = pump(8 if mode in ("healthy", "progress") else 4,
                              EVENT_END_FILE)
                elapsed = time.monotonic() - started
                expected = EOF if mode in ("healthy", "progress") else ERROR
                if result[0] != expected or (expected == ERROR and result[1] >= 0):
                    raise RuntimeError(f"Expected reason {expected}, got {result}\n"
                                       + "\n".join(lines[-15:]))
                if mode == "progress" and elapsed <= WAIT_BUDGET:
                    raise RuntimeError("Progress fixture did not outlast the wait budget")
        finally:
            run_command(mpv, client, "stop")
            server.release.set()
        print(json.dumps({"case": mode, "backend": backend, "result": "PASS",
                          "requests": server.requests}), flush=True)


def main():
    if len(sys.argv) > 2 and sys.argv[2] == "--child":
        run_case(sys.argv[1], sys.argv[3], sys.argv[4])
        return
    args = sys.argv[2:]
    backends = ("lavf", "curl")
    if "--lavf-only" in args:
        args.remove("--lavf-only")
        backends = ("lavf",)
        print("SKIP curl: backend not included in this build", flush=True)
    selected = args or list(CASES)
    if any(mode not in CASES for mode in selected):
        raise ValueError("Unknown regression case")
    for backend in backends:
        for mode in selected:
            result = subprocess.run(
                [sys.executable, __file__, sys.argv[1], "--child", backend, mode],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
                timeout=18,
            )
            if result.returncode:
                raise RuntimeError(f"{backend}/{mode} failed:\n"
                                   + result.stdout + result.stderr)
            sys.stdout.write(result.stdout)


if __name__ == "__main__":
    main()
