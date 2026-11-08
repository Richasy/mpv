#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Bound speculative HTTP work and adopt it without another mpv instance."""

import contextlib
import http.server
import io
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

from libmpv_curl_settings import options, wait_event
from libmpv_network_cursors import (
    create_client, get_property, load_libmpv, run_command,
)

BUDGET = 8 * 1024 * 1024
PCM = bytes(range(256)) * (48000 * 2 * 2 * 120 // 256)
output = io.BytesIO()
with wave.open(output, "wb") as audio:
    audio.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
    audio.writeframes(PCM)
WAV = output.getvalue()

def ebml(identifier, payload):
    width = next(n for n in range(1, 9) if len(payload) < (1 << (7 * n)) - 1)
    return (identifier.to_bytes((identifier.bit_length() + 7) // 8, "big")
            + ((1 << (7 * width)) | len(payload)).to_bytes(width, "big") + payload)


def make_mkv():
    header = ebml(0x1A45DFA3, ebml(0x4286, b"\x01") + ebml(0x42F7, b"\x01")
                  + ebml(0x42F2, b"\x04") + ebml(0x42F3, b"\x08")
                  + ebml(0x4282, b"matroska") + ebml(0x4287, b"\x04")
                  + ebml(0x4285, b"\x02"))
    info = ebml(0x1549A966, ebml(0x2AD7B1, (1000000).to_bytes(3, "big"))
                + ebml(0x4489, struct.pack(">d", 10000))
                + ebml(0x4D80, b"prefetch-test") + ebml(0x5741, b"prefetch-test"))
    audio = ebml(0xE1, ebml(0xB5, struct.pack(">d", 48000))
                 + ebml(0x9F, b"\x02") + ebml(0x6264, b"\x10"))
    tracks = ebml(0x1654AE6B, ebml(0xAE, ebml(0xD7, b"\x01")
                  + ebml(0x73C5, b"\x01") + ebml(0x83, b"\x02")
                  + ebml(0x86, b"A_PCM/INT/LIT") + audio))
    data = header + bytes.fromhex("1853806701ffffffffffffff") + info + tracks
    boundary = None
    for second in range(10):
        data += ebml(0x1F43B675, ebml(0xE7, (second * 1000).to_bytes(2, "big"))
                     + ebml(0xA3, b"\x81\x00\x00\x80"
                            + PCM[second * 192000:(second + 1) * 192000]))
        if boundary is None:
            boundary = len(data)
    return data, boundary


MKV, MKV_CLUSTER_END = make_mkv()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.connection.settimeout(3)
        self.close_connection = True
        requested = self.headers.get("Range", "")
        with self.server.lock:
            self.server.requests.append((self.path, requested))
        if self.path in ("/redirect", "/loop"):
            self.send_response(302)
            self.send_header("Location", "/loop" if self.path == "/loop" else "/next")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if self.path == "/limited":
            self.send_response(429)
            self.send_header("Retry-After", "0")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
        media = MKV if self.path == "/mkv-limited" else WAV
        if match:
            start = int(match[1])
            end = min(int(match[2]) if match[2] else len(media) - 1, len(media) - 1)
        else:
            start, end = 0, len(media) - 1
        if self.path in ("/mid-limited", "/mkv-limited"):
            if start > 0:
                self.send_response(429)
                self.send_header("Retry-After", "0")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, MKV_CLUSTER_END - 1 if self.path == "/mkv-limited" else 256 * 1024 - 1)
        if start >= len(media):
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{len(media)}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        self.send_response(206 if match else 200)
        self.send_header("Content-Type", "audio/x-matroska" if self.path == "/mkv-limited" else "audio/wav")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            for offset in range(start, end + 1, 16384):
                data = media[offset:min(offset + 16384, end + 1)]
                self.wfile.write(data)
                self.wfile.flush()
                with self.server.lock:
                    self.server.sent += len(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, TimeoutError):
            pass


def wait_state(mpv, client, accepted):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        state = int(get_property(mpv, client, "prefetch-state") or -1)
        if state in accepted:
            return state
        mpv.mpv_wait_event(client, 0.02)
    raise AssertionError(f"Prefetch did not reach {accepted}; last state={state}")


def run_case(library, mode):
    with contextlib.ExitStack() as stack, tempfile.TemporaryDirectory() as directory:
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server.daemon_threads = False
        server.lock = threading.Lock()
        server.requests = []
        server.sent = 0
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        stack.callback(server.server_close)
        stack.callback(worker.join, 5)
        stack.callback(server.shutdown)
        mpv = load_libmpv(stack, library)
        current = Path(directory, "current.wav")
        current.write_bytes(WAV[:44] + PCM)
        rendered = Path(directory, "rendered.pcm")
        with contextlib.ExitStack() as owner:
            client = create_client(owner, mpv, options(**{
                "pause": "yes", "keep-open": "yes", "cache": "yes",
                "curl-enabled": "no", "stream-lru-cache": "64MiB" if mode == "adopt-lru" else "0",
                "demuxer-readahead-secs": "2", "cache-secs": "2",
                "curl-retry-http-429": "yes", "curl-max-retries": "5",
                "ao": "pcm", "ao-pcm-file": str(rendered),
                "ao-pcm-waveheader": "no", "audio-format": "s16",
                "audio-samplerate": "48000", "audio-channels": "stereo",
            }))
            run_command(mpv, client, "loadfile", str(current))
            wait_event(mpv, client, 8, 5)
            assert server.requests == []
            endpoint = {
                "rate-limit": "limited", "redirect-loop": "loop",
                "mid-rate-limit": "mid-limited",
                "mkv-rate-limit": "mkv-limited",
            }.get(mode, "redirect")
            url = f"http://127.0.0.1:{server.server_port}/{endpoint}"
            run_command(mpv, client, "prefetch-file", url)
            state = wait_state(mpv, client, {2, 3})
            if mode in ("rate-limit", "redirect-loop", "mid-rate-limit", "mkv-rate-limit"):
                assert state == 3
                expected = {"rate-limit": 1, "redirect-loop": 4,
                            "mid-rate-limit": 2, "mkv-rate-limit": 2}[mode]
                assert len(server.requests) == expected, server.requests
                assert get_property(mpv, client, "prefetch-adoptions") == "0"
                assert get_property(mpv, client, "pause") == "yes"
            else:
                assert state == 2, (state, server.requests)
                assert int(get_property(mpv, client, "prefetch-bytes")) <= BUDGET
                assert int(get_property(mpv, client, "prefetch-requests")) <= 4
                assert server.sent <= BUDGET
                before = list(server.requests)
                run_command(mpv, client, "prefetch-cancel" if mode == "cancel" else "stop")
                if mode == "cancel":
                    assert wait_state(mpv, client, {0}) == 0
                    assert get_property(mpv, client, "pause") == "yes"
                    assert server.requests == before
                else:
                    stopped = wait_event(mpv, client, "end", 5)
                    assert stopped["reason"] == 2, stopped
                    run_command(mpv, client, "loadfile", url)
                    wait_event(mpv, client, 8, 5)
                    assert get_property(mpv, client, "prefetch-adoptions") == "1"
                    # The next file starts from the prepared input, without reopening byte zero.
                    zero_ranges = sum(path == "/next" and value.startswith("bytes=0-")
                                      for path, value in server.requests)
                    assert zero_ranges == 1, server.requests
                    run_command(mpv, client, "set", "keep-open", "no")
                    run_command(mpv, client, "set", "pause", "no")
                    ended = wait_event(mpv, client, "end", 12)
                    assert ended["error"] >= 0, ended
        if mode in ("adopt", "adopt-lru"):
            # The outgoing paused input must not have emitted any audio.
            assert rendered.read_bytes() == PCM
        print(json.dumps({"case": mode, "state": state,
                          "requests": len(server.requests), "sent": server.sent}), flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--child":
        run_case(sys.argv[2], sys.argv[3])
    elif len(sys.argv) in (2, 3):
        cases = ("adopt", "adopt-lru", "cancel", "rate-limit", "redirect-loop",
                 "mid-rate-limit", "mkv-rate-limit")
        if len(sys.argv) == 3:
            if sys.argv[2] not in cases:
                raise SystemExit("Unknown prefetch regression case")
            cases = (sys.argv[2],)
        for case in cases:
            with tempfile.TemporaryDirectory(prefix="mpv-prefetch-") as root:
                result = subprocess.run(
                    [sys.executable, "-B", __file__, "--child", sys.argv[1], case],
                    capture_output=True, text=True, timeout=30,
                    env={**os.environ, "TEMP": root, "TMP": root})
            if result.returncode:
                raise RuntimeError(f"{case} exited with {result.returncode}\n"
                                   + result.stdout + result.stderr)
            print(result.stdout.strip(), flush=True)
    else:
        raise SystemExit("usage: libmpv_prefetch.py <libmpv library>")
