#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Check address-free curl request diagnostics with deterministic loopback IO."""

import contextlib
import ctypes
import http.server
import json
import re
import subprocess
import sys
import threading
import time
import urllib.parse

from libmpv_curl_settings import RANGE_WAV
from libmpv_network_cursors import (
    EndFile, EVENT_END_FILE, EVENT_LOG_MESSAGE, LogMessage,
    create_client, load_libmpv, run_command,
)
import libmpv_network_cursors as cursors

SECRET = "curl-diagnostic-private-marker"
CASES = ("range", "headers", "body", "partial", "status", "wrong-range",
         "retry-cancel", "cursors", "status-held", "wrong-range-held",
         "retry-held")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, mode):
        super().__init__(("127.0.0.1", 0), Handler)
        self.mode = mode
        self.release = threading.Event()
        self.requests = []
        self.rate_limited = False


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError):
            # Cancellation can close a keep-alive socket between requests.
            pass

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/origin":
            self.send_response(302)
            self.send_header("Location", f"/{SECRET}?token={SECRET}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        mode = self.server.mode
        if mode == "headers":
            self.server.release.wait(35)
        if mode in ("status", "status-held"):
            self.send_response(502)
            self.send_header("Content-Length", "100" if mode.endswith("-held") else "0")
            self.end_headers()
            if mode.endswith("-held"):
                self.wfile.flush()
                self.server.release.wait(15)
            return
        if mode == "retry-held" and not self.server.rate_limited:
            self.server.rate_limited = True
            self.send_response(429)
            self.send_header("Retry-After", "1")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.flush()
            self.server.release.wait(15)
            return
        if mode == "retry-cancel":
            self.send_response(429)
            self.send_header("Retry-After", "20")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        requested = self.headers.get("Range", "")
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
        if not match:
            raise RuntimeError(f"Missing numeric Range: {requested!r}")
        start = int(match[1])
        end = min(int(match[2]) if match[2] else len(RANGE_WAV) - 1,
                  len(RANGE_WAV) - 1)
        self.server.requests.append(start)
        if start > end:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(RANGE_WAV)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        wrong = mode in ("wrong-range", "wrong-range-held") and start > 0
        if wrong:
            start, end = 0, len(RANGE_WAV) - 1
        self.send_response(200 if wrong else 206)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("X-Private-Diagnostic", SECRET)
        if not wrong:
            self.send_header("Content-Range",
                             f"bytes {start}-{end}/{len(RANGE_WAV)}")
        self.end_headers()
        if wrong and mode == "wrong-range-held":
            self.wfile.flush()
            self.server.release.wait(15)
            return
        if mode == "body":
            self.wfile.flush()
            self.server.release.wait(130)
        body = RANGE_WAV[start:end + 1]
        if mode == "partial" and start == 0:
            body = body[:1024]
            self.close_connection = True
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            self.close_connection = True


@contextlib.contextmanager
def serve(mode):
    server = Server(mode)
    worker = threading.Thread(target=server.serve_forever)
    worker.start()
    try:
        yield server
    finally:
        server.release.set()
        server.shutdown()
        worker.join(3)
        server.server_close()
        if worker.is_alive():
            raise RuntimeError("Diagnostic fixture did not stop")


def records(lines, event=None):
    result = []
    for line in lines:
        words = line.split()
        if event is None or words[1] == event:
            result.append(dict(word.split("=", 1) for word in words[2:]))
    return result


def run_case(library, mode):
    if mode == "cursors":
        media = cursors.Media()
        with cursors.serve(media.data) as server:
            lines, _, _, _ = cursors.run_case(
                library, server, media, mode, {"curl-enabled": "yes"})
        begins = records([line for line in lines
                          if line.startswith("curl_request ")], "begin")
        ids = [row["id"] for row in begins]
        if len(ids) < 3 or len(ids) != len(set(ids)):
            raise RuntimeError(f"Cursor request identities collided: {ids}")
        print(json.dumps({"case": mode, "result": "PASS",
                          "requests": len(ids)}), flush=True)
        return
    with serve(mode) as server, contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        options = {
            "ao": "null", "ao-null-untimed": "yes", "sid": "no",
            "keep-open": "no", "curl-enabled": "yes",
            "curl-max-request-size": "8192", "curl-max-retries": "1",
            "network-timeout": "30", "user-agent": SECRET,
            "http-header-fields": f"Authorization: Bearer {SECRET}",
        }
        if mode == "partial":
            options["curl-max-request-size"] = "0"
        if mode == "body":
            options["network-timeout"] = "0"
        if mode == "retry-cancel":
            options["curl-retry-http-429"] = "yes"
        if mode.endswith("-held"):
            options["network-timeout"] = "5"
        if mode == "retry-held":
            options["curl-retry-http-429"] = "yes"
        client = create_client(stack, mpv, options)
        if mpv.mpv_request_log_messages(client, b"info") < 0:
            raise RuntimeError("Could not subscribe to info-level diagnostics")
        lines = []
        ends = []
        request_times = []

        def pump(timeout, predicate, allow_timeout=False):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(client, 0.05).contents
                if event.event_id == EVENT_LOG_MESSAGE:
                    message = ctypes.cast(
                        event.data, ctypes.POINTER(LogMessage)).contents
                    text = message.text.decode("utf-8", errors="strict").strip()
                    if text.startswith("curl_request "):
                        lines.append(text)
                        if text.startswith("curl_request begin "):
                            request_times.append(time.monotonic())
                elif event.event_id == EVENT_END_FILE:
                    end = ctypes.cast(
                        event.data, ctypes.POINTER(EndFile)).contents
                    ends.append((end.reason, end.error))
                if predicate():
                    return True
            if allow_timeout:
                return False
            raise TimeoutError(f"{mode}: incomplete diagnostic evidence\n"
                               + "\n".join(lines))

        url = f"http://127.0.0.1:{server.server_port}/origin?token={SECRET}"
        run_command(mpv, client, "loadfile", url)
        try:
            if mode in ("headers", "body"):
                pump(22, lambda: bool(records(lines, "pending")))
                pending = records(lines, "pending")
                if len(pending) != 1 or pending[0]["report"] != "1/3":
                    raise RuntimeError(f"Unbounded pending diagnostics: {pending}")
                expected = "1" if mode == "body" else "0"
                if pending[0]["headers"] != expected or pending[0]["body"] != "0":
                    raise RuntimeError(f"Wrong pending phase: {pending}")
                if float(pending[0]["idle_ms"]) < 15000:
                    raise RuntimeError("Pending warning preceded its idle limit")
                if mode == "body":
                    pump(67, lambda: len(records(lines, "pending")) == 3)
                    pending = records(lines, "pending")
                    for before, after in zip(pending, pending[1:]):
                        if (float(after["elapsed_ms"]) -
                                float(before["elapsed_ms"]) < 30000):
                            raise RuntimeError("Pending reports were not spaced")
                    if pump(32, lambda: len(records(lines, "pending")) > 3,
                            allow_timeout=True):
                        raise RuntimeError("Pending report cap was exceeded")
                run_command(mpv, client, "stop")
                pump(3, lambda: bool(ends) and any(
                    row["cancelled"] == "1"
                    for row in records(lines)
                    if row.get("curl_result") is not None))
                if ends[-1][0] != 2:
                    raise RuntimeError(f"Cancellation changed: {ends}")
            elif mode == "retry-cancel":
                pump(5, lambda: any(row["status"] == "429"
                                   for row in records(lines, "end")))
                run_command(mpv, client, "stop")
                pump(3, lambda: bool(ends) and any(
                    row["cancelled"] == "1"
                    for row in records(lines, "retry_removed")))
                if ends[-1][0] != 2:
                    raise RuntimeError(f"Retry cancellation changed: {ends}")
            else:
                pump(12, lambda: bool(ends) and bool(records(lines, "end")))
                if mode in ("status", "wrong-range", "status-held",
                            "wrong-range-held") and ends[-1][0] != 4:
                    raise RuntimeError(f"Error became a clean EOF: {ends}")
                if mode in ("range", "partial", "retry-held") and ends[-1][0] != 0:
                    raise RuntimeError(f"Healthy/recovered playback failed: {ends}")
        finally:
            run_command(mpv, client, "stop")
            server.release.set()

        for line in lines:
            if any(secret in line for secret in
                   [SECRET, "127.0.0.1", "http:", "https:", "Authorization"]):
                raise RuntimeError("Private request data leaked into a summary")
            if len(line) > 1000:
                raise RuntimeError("Summary exceeds the consumer's text limit")
        begins = records(lines, "begin")
        ids = [row["id"] for row in begins]
        if not ids or len(ids) != len(set(ids)):
            raise RuntimeError(f"Missing or duplicate request identities: {ids}")
        if any(row["id"] not in ids for row in records(lines)):
            raise RuntimeError("Lifecycle event lost its request identity")
        summaries = [row for row in records(lines) if "status" in row]
        if not summaries or any(not all(key in row for key in
                ["dns_ms", "connect_ms", "tls_ms", "first_byte_ms",
                 "redirect_ms", "total_ms", "new_connections"])
                for row in summaries):
            raise RuntimeError("Missing transport timings")
        if mode == "range":
            headers = records(lines, "headers")
            if not any(int(row["offset"]) > 0 and
                       row["offset"] == row["range_start"] for row in headers):
                raise RuntimeError("Ranged request was not diagnosed")
            if not any(row["redirects"] == "1" for row in headers):
                raise RuntimeError("Redirect chain was not diagnosed")
            if not records(lines, "first_body"):
                raise RuntimeError("Missing first-body delivery")
            if not any(float(row["first_byte_ms"]) > 0 and
                       float(row["connect_ms"]) >= 0 and
                       float(row["tls_ms"]) == 0 for row in headers):
                raise RuntimeError("Invalid loopback HTTP transport timings")
        if mode == "partial":
            if not any(row["retry"] == "1" and int(row["offset"]) > 0
                       for row in begins):
                raise RuntimeError("Partial-transfer retry was not diagnosed")
            if not any(int(row["curl_result"]) > 0
                       for row in records(lines, "end")):
                raise RuntimeError("Missing failed-transfer result")
        if mode in ("status", "status-held") and not any(
                row["status"] == "502" for row in summaries):
            raise RuntimeError("Missing HTTP failure status")
        if mode in ("wrong-range", "wrong-range-held") and not any(
                row["rejected"] == "1" for row in records(lines, "end")):
            raise RuntimeError("Missing rejected-range result")
        if mode.endswith("-held"):
            rejected = [row for row in records(lines, "end")
                        if row["rejected"] == "1"]
            if not rejected or any(float(row["elapsed_ms"]) >= 1500
                                   for row in rejected):
                raise RuntimeError("Rejected headers waited for a held body")
        if mode == "retry-held":
            starts = records(lines, "begin")
            rejected = records(lines, "end")[0]
            if rejected["status"] != "429" or starts[1]["retry"] != "1":
                raise RuntimeError("Held 429 did not preserve its retry")
            if request_times[1] - request_times[0] < 1:
                raise RuntimeError("Held 429 bypassed Retry-After")
        print(json.dumps({"case": mode, "result": "PASS",
                          "requests": len(begins)}), flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--child":
        run_case(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2:
        for case in CASES:
            subprocess.run([sys.executable, "-B", __file__, "--child",
                            sys.argv[1], case], check=True,
                           timeout=125 if case == "body" else 40)
    else:
        raise SystemExit("usage: libmpv_curl_diagnostics.py <libmpv library>")
