#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import http.server
import json
import subprocess
import sys
import threading
import urllib.parse
from pathlib import Path


class TranslationServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), TranslationHandler)
        self.lock = threading.Lock()
        self.requests = []


class TranslationHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        return

    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path != "/v1/chat/completions":
            self.send_error(404)
            return
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size))
        messages = request.get("messages", [])
        text = messages[-1].get("content", "") if messages else ""
        with self.server.lock:
            self.server.requests.append(text)
        payload = json.dumps(
            {"choices": [{"message": {"content": f"translated:{text}"}}]}
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)


def main():
    if len(sys.argv) != 8:
        raise SystemExit(
            "expected executable, embedded, video, external, bitmap, dual, "
            "and ASS fixtures"
        )
    server = TranslationServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    dense_path = Path(sys.argv[3]).with_name("sub-translation-dense.srt")
    dense_path.write_text(
        "".join(
            f"{index + 1}\n"
            "00:00:05,000 --> 00:00:20,000\n"
            f"dense-{index:03d}\n\n"
            for index in range(160)
        ),
        encoding="utf-8",
    )
    try:
        process = subprocess.run(
            [
                sys.argv[1],
                f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                *sys.argv[2:],
                str(dense_path),
            ],
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=45,
        )
        sys.stdout.buffer.write(process.stdout.encode("utf-8"))
        if process.returncode:
            with server.lock:
                sys.stdout.write(f"fixture requests: {server.requests!r}\n")
            raise RuntimeError(
                f"native subtitle translation test exited with "
                f"{process.returncode}"
            )
        with server.lock:
            requests = list(server.requests)
        expected = {"foo", "bar", "Hello\nworld", "I", "Repeat"}
        expected.update(f"dense-{index:03d}" for index in range(160))
        if not expected.issubset(requests) or any(
            text not in expected for text in requests
        ):
            raise RuntimeError(
                f"unexpected translated request set: {requests!r}"
            )
        if any("{\\i" in text for text in requests):
            raise RuntimeError("ASS override tags reached the translator")
        if any("http://" in text or "fixture-secret" in text
               for text in requests):
            raise RuntimeError("private configuration leaked into translation text")
    finally:
        server.shutdown()
        worker.join(timeout=3)
        server.server_close()
        dense_path.unlink(missing_ok=True)
        if worker.is_alive():
            raise RuntimeError("translation fixture server did not stop")


if __name__ == "__main__":
    main()
