#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import http.server
import re
import struct
import subprocess
import sys
import threading
import urllib.parse


def element(tag, payload):
    width = 1
    while len(payload) >= (1 << (7 * width)) - 1:
        width += 1
    size = ((1 << (7 * width)) | len(payload)).to_bytes(width, "big")
    return bytes.fromhex(tag) + size + payload


def integer(tag, value, width=1):
    return element(tag, value.to_bytes(width, "big"))


def matroska_fixture():
    header = element(
        "1a45dfa3",
        integer("4286", 1)
        + integer("42f7", 1)
        + integer("42f2", 4)
        + integer("42f3", 8)
        + element("4282", b"matroska")
        + integer("4287", 4)
        + integer("4285", 2),
    )
    info = element(
        "1549a966",
        integer("2ad7b1", 1000000, 3)
        + element("4489", struct.pack(">d", 10000)),
    )
    track = element(
        "ae",
        integer("d7", 1)
        + integer("73c5", 1)
        + integer("83", 2)
        + element("86", b"A_PCM/INT/LIT")
        + element(
            "e1",
            element("b5", struct.pack(">d", 8000))
            + integer("9f", 1)
            + integer("6264", 16),
        ),
    )
    tracks = element("1654ae6b", track)
    cluster = element(
        "1f43b675",
        integer("e7", 0) + element("a3", b"\x81\x00\x00\x80" + bytes(160000)),
    )
    cues = element("1c53bb6b", b"")
    tags = element("1254c367", b"")

    def seek_head(cue_pos, tag_pos):
        entries = b""
        for tag, pos in (("1c53bb6b", cue_pos), ("1254c367", tag_pos)):
            entries += element(
                "4dbb",
                element("53ab", bytes.fromhex(tag)) + integer("53ac", pos, 8),
            )
        return element("114d9b74", entries)

    head = seek_head(0, 0)
    cue_pos = len(head + info + tracks + cluster)
    head = seek_head(cue_pos, cue_pos + len(cues))
    body = head + info + tracks + cluster + cues + tags
    segment = element("18538067", body)
    tail_offset = len(header) + len(segment) - len(body) + cue_pos
    return header + segment, tail_offset


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self):
        super().__init__(("127.0.0.1", 0), FixtureHandler)
        self.media, self.tail_offset = matroska_fixture()
        self.release_stall = threading.Event()
        self.release_cancel = threading.Event()
        self.cancel_entered = threading.Event()
        self.expired = threading.Event()


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5)

    def log_message(self, *args):
        return

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/denied":
            self.send_response(403)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            return

        media = self.server.media
        start, end = 0, len(media) - 1
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.send_error(416)
                return
            start = int(match[1])
            if match[2]:
                end = min(end, int(match[2]))
            if start > end:
                self.send_error(416)
                return
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", "video/x-matroska")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Connection", "close")
        if requested:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
        self.end_headers()
        self.wfile.flush()

        # Headers succeed; only the remote tail body is held until the native
        # logger proves a pending operation or the cancellation case finishes.
        if start >= self.server.tail_offset and path in ("/stall", "/cancel"):
            gate = (
                self.server.release_stall
                if path == "/stall"
                else self.server.release_cancel
            )
            if path == "/cancel":
                self.server.cancel_entered.set()
            if not gate.wait(45):
                self.server.expired.set()
                self.close_connection = True
                return
        try:
            self.wfile.write(media[start : end + 1])
        except (BrokenPipeError, ConnectionResetError):
            # Stopping a native client intentionally abandons its response.
            pass
        self.close_connection = True


def main():
    server = FixtureServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    process = None
    reader = None
    output = []
    reader_errors = []
    try:
        process = subprocess.Popen(
            [
                sys.argv[1],
                f"http://127.0.0.1:{server.server_port}",
                str(server.tail_offset),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )

        def read_output():
            for line in process.stdout:
                if line.strip() == "RELEASE_TAIL":
                    server.release_stall.set()
                elif line.strip() == "CANCEL_REQUESTED":
                    if (
                        not server.cancel_entered.is_set()
                        or server.release_cancel.is_set()
                    ):
                        reader_errors.append(
                            "Cancellation did not start while the tail body was held"
                        )
                else:
                    output.append(line)
                    if line.startswith("PASS cancel:") and (
                        not server.cancel_entered.is_set()
                        or server.release_cancel.is_set()
                    ):
                        reader_errors.append(
                            "The native client did not exit before tail release"
                        )

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        result = process.wait(timeout=55)
        reader.join(timeout=3)
        if reader.is_alive():
            raise RuntimeError("Native diagnostic output did not drain")
        if reader_errors:
            raise RuntimeError("; ".join(reader_errors))
        sys.stdout.write("".join(output))
        if server.expired.is_set():
            raise RuntimeError("A fixture gate expired instead of being released")
        if result:
            raise RuntimeError(f"Native diagnostic regression exited with {result}")
    finally:
        server.release_stall.set()
        server.release_cancel.set()
        if process is not None:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=5)
            if reader is not None:
                reader.join(timeout=3)
            process.stdout.close()
        server.shutdown()
        worker.join(timeout=3)
        server.server_close()
        if worker.is_alive():
            raise RuntimeError("Loopback fixture did not stop")


if __name__ == "__main__":
    main()
