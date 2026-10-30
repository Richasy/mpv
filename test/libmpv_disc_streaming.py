#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Headless local/HTTP ISO and mapped BDMV regression and latency probe.

Accepts an authored, unencrypted Blu-ray ISO and optionally the matching BDMV
folder. The server is loopback-only, never writes its input, and implements
Range, redirects, authentication, throttling and request accounting. Each run
uses a new libmpv handle and cache. Private media names/URLs are not reported.
"""

import argparse
from collections import deque
import contextlib
import ctypes as c
import http.server
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import threading
import time
from urllib.parse import unquote, urlsplit

from libmpv_thumbnails import Mpv, Node, NodeList, Value


def author_fixture(root, ffmpeg, tsmuxer):
    def checked(arguments):
        result = subprocess.run(arguments, capture_output=True, timeout=120)
        if result.returncode:
            raise RuntimeError(result.stderr.decode("utf-8", errors="replace")[-3000:])

    media = root / "source.ts"
    checked([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
        "-i", "testsrc2=s=1280x720:r=24", "-f", "lavfi", "-i",
        "sine=frequency=440:sample_rate=48000", "-t", "24", "-c:v", "libx264",
        "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-x264-params",
        "keyint=24:min-keyint=24:bframes=0:bluray-compat=1", "-c:a", "ac3",
        "-f", "mpegts", str(media),
    ])
    meta = root / "fixture.meta"
    meta.write_text(
        "MUXOPT --blu-ray --custom-chapters=00:00:00.000;00:00:08.000;00:00:16.000\n"
        f'V_MPEG4/ISO/AVC, "{media}", track=256, insertSEI, contSPS\n'
        f'A_AC3, "{media}", track=257, lang=eng\n', encoding="utf-8")
    for target in (root / "fixture.iso", root / "folder"):
        checked([tsmuxer, str(meta), str(target)])
    return root / "fixture.iso", root / "folder"


def author_multiclip(folder, target):
    shutil.copytree(folder, target)
    playlist = target / "BDMV/PLAYLIST/00000.mpls"
    data = playlist.read_bytes()
    section, marks, extension = struct.unpack_from(">III", data, 8)
    assert extension == 0 and struct.unpack_from(">HH", data, section + 6) == (1, 0)
    length = struct.unpack_from(">H", data, section + 10)[0] + 2
    item = bytearray(data[section + 10:section + 10 + length])
    in_time, out_time = struct.unpack_from(">II", item, 14)
    items, chapters = [], []
    # Two distinct clips with nonzero, different in/out cuts. A largest-file
    # shortcut or byte concatenation cannot produce this 36-second timeline.
    for index, (head, tail) in enumerate(((4, 2), (2, 4))):
        clip = bytearray(item)
        clip[2:7] = f"{index:05}".encode()
        start, end = in_time + head * 45000, out_time - tail * 45000
        struct.pack_into(">II", clip, 14, start, end)
        items.append(clip)
        for offset in (0, 8 * 45000):
            chapters.append(b"\x00\x01" + struct.pack(">HIHI", index, start + offset, 0xffff, 0))
    body = b"\0\0" + struct.pack(">HH", 2, 0) + b"".join(items)
    mark_body = struct.pack(">H", len(chapters)) + b"".join(chapters)
    result = bytearray(data[:section] + struct.pack(">I", len(body)) + body)
    struct.pack_into(">I", result, 12, len(result))
    result += struct.pack(">I", len(mark_body)) + mark_body
    playlist.write_bytes(result)
    (target / "BDMV/BACKUP/PLAYLIST/00000.mpls").write_bytes(result)
    for subdir, suffix in (("STREAM", "m2ts"), ("CLIPINF", "clpi")):
        shutil.copyfile(target / f"BDMV/{subdir}/00000.{suffix}",
                        target / f"BDMV/{subdir}/00001.{suffix}")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, iso, folder, delay, rate, authenticated=False):
        super().__init__(("127.0.0.1", 0), Handler)
        self.iso, self.folder = iso, folder
        self.delay, self.rate = delay, rate
        self.authenticated = authenticated
        self.requests = []
        self.bytes_sent = 0
        self.lock = threading.Lock()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server_port}"

    def stats(self):
        with self.lock:
            return len(self.requests), self.bytes_sent


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, ConnectionAbortedError):
            pass

    def do_HEAD(self):
        self.respond(False)

    def do_GET(self):
        self.respond(True)

    def respond(self, body):
        server = self.server
        route = unquote(urlsplit(self.path).path)
        with server.lock:
            server.requests.append((route, self.headers.get("Range")))
        if server.authenticated and self.headers.get("X-Disc-Test") != "allowed":
            self.send_error(403)
            return
        time.sleep(server.delay)
        if route == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/disc.iso?token=fixture")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if route in ("/disc.iso", "/no-range.iso", "/bad-range.iso"):
            source = server.iso
        elif server.folder:
            source = (server.folder / route.lstrip("/")).resolve()
            if not source.is_relative_to(server.folder.resolve()):
                self.send_error(403)
                return
        else:
            self.send_error(404)
            return
        if not source.is_file():
            self.send_error(404)
            return
        size = source.stat().st_size
        start, end = 0, size - 1
        requested = self.headers.get("Range")
        partial = requested is not None and route != "/no-range.iso"
        if route == "/bad-range.iso" and requested != "bytes=0-":
            partial = False
        if partial:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match or int(match[1]) >= size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            start = int(match[1])
            end = min(int(match[2]) if match[2] else end, end)
        self.send_response(206 if partial else 200)
        if route != "/no-range.iso":
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(end - start + 1))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if not body:
            return
        try:
            with source.open("rb") as stream:
                stream.seek(start)
                remaining = end - start + 1
                while remaining:
                    data = stream.read(min(16 * 1024, remaining))
                    if not data:
                        break
                    self.wfile.write(data)
                    with server.lock:
                        server.bytes_sent += len(data)
                    remaining -= len(data)
                    if server.rate:
                        time.sleep(len(data) / server.rate)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass


@contextlib.contextmanager
def serving(*args, **kwargs):
    server = Server(*args, **kwargs)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        worker.join()


class Probe:
    def __init__(self, library):
        self.mpv = Mpv(str(library))
        self.logs = deque(maxlen=35)
        self.loaded = False
        self.restarts = 0
        self.ended = False
        for name, value in {
            "demuxer-readahead-secs": "0", "demuxer-hysteresis-secs": "0",
            "demuxer-max-bytes": "2MiB", "network-timeout": "5",
            "cache-pause": "no", "http-header-fields": "X-Disc-Test: allowed",
        }.items():
            self.mpv.command("set", name, value)

    def event(self):
        event = self.mpv.event(0.01)
        self.loaded |= event["id"] == 8
        self.restarts += event["id"] == 21
        self.ended |= event["id"] == 7
        if event.get("log"):
            self.logs.append(event["log"])

    def wait(self, predicate, description, limit=20):
        deadline = time.monotonic() + limit
        while time.monotonic() < deadline:
            self.event()
            if predicate():
                return
            if self.ended:
                raise AssertionError(f"Playback ended while {description}")
        raise AssertionError(f"Timed out {description}")

    def close(self):
        self.mpv.close()

    def file_map(self, mapping):
        keys = [key.encode() for key in mapping]
        values = [value.encode() for value in mapping.values()]
        native_keys = (c.c_char_p * len(keys))(*keys)
        native_values = (Node * len(values))(*[
            Node(Value(pointer=c.cast(c.c_char_p(value), c.c_void_p).value), 1)
            for value in values])
        entries = NodeList(len(keys), native_values, native_keys)
        node = Node(Value(pointer=c.addressof(entries)), 8)
        setter = self.mpv.api.mpv_set_property
        setter.argtypes = [c.c_void_p, c.c_char_p, c.c_int, c.POINTER(Node)]
        setter.restype = c.c_int
        assert setter(self.mpv.handle, b"bluray-files", 6, c.byref(node)) >= 0

    def load(self, source):
        start = time.monotonic()
        self.mpv.command("loadfile", str(source))
        self.wait(lambda: self.loaded and self.restarts > 0, "loading disc")
        return time.monotonic() - start

    def seek(self, target):
        while self.mpv.event(0)["id"]:
            pass
        restarts = self.restarts
        start = time.monotonic()
        self.mpv.command("seek", target, "absolute+exact")
        self.wait(
            lambda: self.restarts > restarts and
            abs((self.mpv.property("time-pos") or 0) - target) < 0.5,
            "seeking disc",
        )
        return time.monotonic() - start

    def reject(self, source):
        self.mpv.command("loadfile", source)
        self.wait(lambda: self.ended, "rejecting invalid source", 10)
        assert not self.loaded


def run(library, source, case, server=None, file_map=None, expected=None):
    probe = Probe(library)
    try:
        if file_map:
            probe.file_map(file_map)
        before = server.stats() if server else (0, 0)
        opened = probe.load(source)
        props = {key: probe.mpv.property(key) for key in
                 ("duration", "chapter-list", "edition-list", "track-list")}
        assert props["duration"] and props["duration"] > 1
        assert any(t["type"] == "video" for t in props["track-list"])
        assert any(t["type"] == "audio" for t in props["track-list"])
        assert props["chapter-list"], "Disc chapters were lost"
        if expected:
            assert abs(props["duration"] - expected["duration"]) < 0.1
            assert props["chapter-list"] == expected["chapter-list"]
            assert [(t["type"], t.get("lang")) for t in props["track-list"]] == [
                (t["type"], t.get("lang")) for t in expected["track-list"]]
        after_open = server.stats() if server else (0, 0)
        timings = []
        for fraction in (0.65, 0.2, 0.65, 0.9, 0.01):
            timings.append(probe.seek(props["duration"] * fraction))
        report = {
            "case": case, "openSeconds": round(opened, 4),
            "seekSeconds": [round(t, 4) for t in timings],
            "duration": round(props["duration"], 3),
            "chapters": len(props["chapter-list"]),
            "tracks": len(props["track-list"]),
            "openRequests": after_open[0] - before[0],
            "openBytes": after_open[1] - before[1], "passed": True,
        }
        print(json.dumps(report), flush=True)
        return props
    finally:
        probe.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--iso", type=Path)
    parser.add_argument("--folder", type=Path)
    parser.add_argument("--multi-folder", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--tsmuxer", default="tsMuxeR")
    parser.add_argument("--rtt-ms", type=float, default=20)
    parser.add_argument("--mib-per-second", type=float, default=8)
    parser.add_argument("--baseline", action="store_true",
                        help="Skip new file-map/entry-point regressions")
    args = parser.parse_args()
    args.library = args.library.resolve(strict=True)
    with contextlib.ExitStack() as stack:
        if args.iso is None:
            root = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="mpv-disc-http-")))
            ffmpeg, tsmuxer = shutil.which(args.ffmpeg), shutil.which(args.tsmuxer)
            if not ffmpeg or not tsmuxer:
                parser.error("Fixture authoring requires ffmpeg and tsMuxeR")
            args.iso, args.folder = author_fixture(root, ffmpeg, tsmuxer)
            args.multi_folder = root / "multi"
            author_multiclip(args.folder, args.multi_folder)
        exercise(args)


def exercise(args):
    args.iso = args.iso.resolve(strict=True)
    folder = args.folder.resolve(strict=True) if args.folder else None
    expected = run(args.library, args.iso, "local-iso")
    with serving(args.iso, folder, args.rtt_ms / 1000,
                 args.mib_per_second * 1024 * 1024, True) as server:
        run(args.library, server.url + "/disc.iso", "http-iso", server,
            expected=expected)
        run(args.library, server.url + "/redirect?name=disc.iso", "redirect-iso",
            server, expected=expected)
        if folder and not args.baseline:
            mapping = {p.relative_to(folder).as_posix(): server.url + "/" +
                       p.relative_to(folder).as_posix() + "?token=fixture,opaque&x=y"
                       for p in sorted(folder.rglob("*")) if p.is_file()}
            run(args.library, "bd://", "http-bdmv-map", server,
                mapping, expected)
        if not args.baseline:
            for route, case in (("no-range", "reject-nonseekable"),
                                ("bad-range", "reject-wrong-range")):
                probe = Probe(args.library)
                try:
                    probe.mpv.command("set", "bluray-device",
                                      server.url + f"/{route}.iso")
                    probe.reject("bd://")
                    print(json.dumps({"case": case, "passed": True}))
                finally:
                    probe.close()
                probe = Probe(args.library)
                try:
                    probe.reject(server.url + f"/{route}.iso")
                    print(json.dumps({"case": case + "-autoprobe", "passed": True}))
                finally:
                    probe.close()
            for disabled in (False, True):
                probe = Probe(args.library)
                try:
                    before = server.stats()[0]
                    key = "BDMV/index.bdmv" if disabled else "../BDMV/index.bdmv"
                    probe.mpv.command("set", "bluray-files",
                                      key + "=" + server.url + "/index.bdmv")
                    if disabled:
                        probe.mpv.command("set", "access-references", "no")
                    probe.reject("bd://")
                    assert server.stats()[0] == before
                    print(json.dumps({"case": "reject-disabled-references" if disabled
                                      else "reject-traversal-map", "passed": True}))
                finally:
                    probe.close()
    if not args.baseline:
        with serving(args.iso, folder, 0.5, 32 * 1024, True) as server:
            probe = Probe(args.library)
            try:
                probe.mpv.command("loadfile", server.url + "/disc.iso")
                probe.wait(lambda: server.stats()[0] > 0, "starting canceled open")
                start = time.monotonic()
                probe.mpv.command("stop")
                probe.wait(lambda: probe.mpv.property("idle-active"), "canceling open", 2)
                elapsed = time.monotonic() - start
                assert elapsed < 1, elapsed
                print(json.dumps({"case": "cancel-open", "seconds": round(elapsed, 4),
                                  "passed": True}))
            finally:
                probe.close()
    if folder and not args.baseline:
        run(args.library, folder / "BDMV/index.bdmv", "local-index",
            expected=expected)
        for playlist in sorted((folder / "BDMV/PLAYLIST").glob("*.mpls"))[:1]:
            run(args.library, playlist, "local-mpls", expected=expected)
    if args.multi_folder and not args.baseline:
        multi = args.multi_folder.resolve(strict=True)
        expected = run(args.library, multi, "local-multiclip")
        assert 36 <= expected["duration"] < 37
        assert len(expected["chapter-list"]) == 4
        with serving(args.iso, multi, args.rtt_ms / 1000,
                     args.mib_per_second * 1024 * 1024, True) as server:
            mapping = {p.relative_to(multi).as_posix(): server.url + "/" +
                       p.relative_to(multi).as_posix() for p in multi.rglob("*")
                       if p.is_file()}
            run(args.library, "bd://", "http-multiclip", server, mapping, expected)


if __name__ == "__main__":
    main()
