#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Headless thumbnail ownership, cancellation and long-GOP work regressions.

Run with an exact local runtime directory's libmpv-2.dll and ffmpeg on PATH.
--baseline compares legacy exact pixels and latency in a separate process.
Generated media is private temporary test data, never a committed fixture.
"""

import argparse
import ctypes as c
import hashlib
import http.server
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import threading
import time


class Node(c.Structure):
    pass


class Value(c.Union):
    _fields_ = [("pointer", c.c_void_p), ("integer", c.c_int64),
                ("flag", c.c_int), ("number", c.c_double)]


Node._fields_ = [("value", Value), ("format", c.c_int)]


class NodeList(c.Structure):
    _fields_ = [("count", c.c_int), ("values", c.POINTER(Node)),
                ("keys", c.POINTER(c.c_char_p))]


class ByteArray(c.Structure):
    _fields_ = [("data", c.c_void_p), ("size", c.c_size_t)]


class Event(c.Structure):
    _fields_ = [("id", c.c_int), ("error", c.c_int),
                ("userdata", c.c_uint64), ("data", c.c_void_p)]


class Log(c.Structure):
    _fields_ = [("prefix", c.c_char_p), ("level", c.c_char_p),
                ("text", c.c_char_p), ("log_level", c.c_int)]


def copy_node(node):
    if node.format == 0:
        return None
    if node.format == 1:
        return c.string_at(node.value.pointer).decode("utf-8")
    if node.format == 3:
        return bool(node.value.flag)
    if node.format == 4:
        return node.value.integer
    if node.format == 5:
        return node.value.number
    if node.format in (7, 8):
        items = c.cast(node.value.pointer, c.POINTER(NodeList)).contents
        values = [copy_node(items.values[n]) for n in range(items.count)]
        return values if node.format == 7 else {
            items.keys[n].decode("utf-8"): values[n] for n in range(items.count)}
    if node.format == 9:
        data = c.cast(node.value.pointer, c.POINTER(ByteArray)).contents
        return c.string_at(data.data, data.size)
    raise AssertionError(f"Unexpected native format: {node.format}")


def command_args(args):
    return (c.c_char_p * (len(args) + 1))(
        *(str(arg).encode("utf-8") for arg in args), None)


def image_result(result):
    if result.get("cached"):
        pixels = result.pop("data")
        assert result["format"] == "bgra"
        assert 0 < result["w"] <= 320 and result["h"] > 0
        assert len(pixels) == result["stride"] * result["h"]
        tight = b"".join(pixels[y * result["stride"]:
                               y * result["stride"] + result["w"] * 4]
                         for y in range(result["h"]))
        result["pixelSha256"] = hashlib.sha256(tight).hexdigest()
    return result


class Mpv:
    def __init__(self, library):
        self.directory = os.add_dll_directory(str(Path(library).parent)) if os.name == "nt" else None
        self.api = c.CDLL(library)
        signatures = {
            "mpv_create": (c.c_void_p, []),
            "mpv_initialize": (c.c_int, [c.c_void_p]),
            "mpv_set_option_string": (c.c_int, [c.c_void_p, c.c_char_p, c.c_char_p]),
            "mpv_request_log_messages": (c.c_int, [c.c_void_p, c.c_char_p]),
            "mpv_command_ret": (c.c_int, [c.c_void_p, c.POINTER(c.c_char_p), c.POINTER(Node)]),
            "mpv_command_async": (c.c_int, [c.c_void_p, c.c_uint64, c.POINTER(c.c_char_p)]),
            "mpv_abort_async_command": (None, [c.c_void_p, c.c_uint64]),
            "mpv_get_property": (c.c_int, [c.c_void_p, c.c_char_p, c.c_int, c.POINTER(Node)]),
            "mpv_free_node_contents": (None, [c.POINTER(Node)]),
            "mpv_wait_event": (c.POINTER(Event), [c.c_void_p, c.c_double]),
            "mpv_terminate_destroy": (None, [c.c_void_p]),
        }
        for name, (result, arguments) in signatures.items():
            function = getattr(self.api, name)
            function.restype, function.argtypes = result, arguments
        self.handle = self.api.mpv_create()
        assert self.handle
        options = {
            "config": "no", "load-scripts": "no", "terminal": "no",
            "vo": "null", "ao": "null", "aid": "no", "sid": "no",
            "hwdec": "no", "pause": "yes", "keep-open": "yes",
            "cache": "yes", "demuxer-readahead-secs": "60",
            "demuxer-max-bytes": "128MiB",
        }
        for name, value in options.items():
            assert self.api.mpv_set_option_string(
                self.handle, name.encode(), value.encode()) >= 0, name
        assert self.api.mpv_request_log_messages(self.handle, b"v") >= 0
        assert self.api.mpv_initialize(self.handle) >= 0

    def close(self):
        self.api.mpv_terminate_destroy(self.handle)
        if self.directory:
            self.directory.close()

    def command(self, *args):
        node = Node()
        error = self.api.mpv_command_ret(self.handle, command_args(args), c.byref(node))
        try:
            assert error >= 0, (args[0], error)
            return copy_node(node)
        finally:
            self.api.mpv_free_node_contents(c.byref(node))

    def property(self, name):
        node = Node()
        error = self.api.mpv_get_property(self.handle, name.encode(), 6, c.byref(node))
        try:
            return copy_node(node) if error >= 0 else None
        finally:
            self.api.mpv_free_node_contents(c.byref(node))

    def event(self, wait=0.1):
        event = self.api.mpv_wait_event(self.handle, wait).contents
        result = {"id": event.id, "error": event.error, "reply": event.userdata}
        if event.id == 2:
            result["log"] = c.cast(event.data, c.POINTER(Log)).contents.text.decode()
        elif event.id == 5 and event.error >= 0:
            result["result"] = image_result(copy_node(c.cast(event.data, c.POINTER(Node)).contents))
        return result

    def load(self, source, wait_for_cache=True):
        self.command("loadfile", source)
        deadline = time.monotonic() + 20
        loaded = False
        restarted = False
        while time.monotonic() < deadline:
            event = self.event()
            loaded |= event["id"] == 8
            restarted |= event["id"] == 21
            if not wait_for_cache and loaded and restarted:
                return
            if wait_for_cache:
                cache = self.property("demuxer-cache-state") or {}
                if loaded and cache.get("eof-cached"):
                    return
        raise TimeoutError("Media did not reach the requested playback/cache state")

    def thumbnail(self, mode, position, precision=None):
        args = ["thumbnail-raw", position, 320, mode]
        if precision:
            args.append(precision)
        started = time.perf_counter()
        result = image_result(self.command(*args))
        result["milliseconds"] = (time.perf_counter() - started) * 1000
        return result

    def cancellation(self, mode):
        while self.event(0)["id"]:
            pass
        assert self.api.mpv_command_async(
            self.handle, 91, command_args(
                ["thumbnail-raw", 5.5, 320, mode, "exact"])) >= 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            event = self.event()
            if "thumbnail: decoding exact" in event.get("log", ""):
                break
            assert not (event["id"] == 5 and event["reply"] == 91), event
        else:
            raise TimeoutError("The exact decode never started")
        started = time.monotonic()
        self.api.mpv_abort_async_command(self.handle, 91)
        assert self.api.mpv_command_async(
            self.handle, 92, command_args(
                ["thumbnail-raw", 1, 320, mode, "keyframes"])) >= 0
        replies = {}
        while time.monotonic() - started < 2:
            event = self.event()
            if event["id"] == 5:
                replies[event["reply"]] = event
            if 91 in replies and 92 in replies:
                assert replies[91]["error"] < 0, replies[91]
                assert replies[92]["error"] == 0 and replies[92]["result"]["cached"], replies[92]
                return (time.monotonic() - started) * 1000
        raise TimeoutError("A cancelled preview blocked its replacement for two seconds")


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, media):
        self.media = media
        self.requests = 0
        self.guard = threading.Lock()
        super().__init__(("127.0.0.1", 0), FixtureHandler)


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.guard:
            self.server.requests += 1
        data = self.server.media
        start, end = 0, len(data) - 1
        header = self.headers.get("Range")
        if header:
            first, last = header.removeprefix("bytes=").split("-", 1)
            start = int(first)
            if last:
                end = min(int(last), end)
        self.send_response(206 if header else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if header:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
        self.end_headers()
        try:
            self.wfile.write(data[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass


def exercise(args):
    media = Path(args.fixture)
    results = {}
    server = FixtureServer(media.read_bytes())
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        for mode in ("local", "cache"):
            print(f"{mode}: create", file=sys.stderr, flush=True)
            client = Mpv(args.library)
            try:
                print(f"{mode}: load", file=sys.stderr, flush=True)
                client.load(str(media) if mode == "local" else
                            f"http://127.0.0.1:{server.server_port}/sample.mp4")
                requests = server.requests
                print(f"{mode}: cold preview", file=sys.stderr, flush=True)
                cold = client.thumbnail(mode, 4.5, None if args.legacy else "keyframes")
                assert cold["cached"], cold
                if not args.legacy:
                    assert not cold["decoder-reused"] and cold["decoded-frames"] == 1, cold
                records = []
                for position in (1.0, 4.5, 7.5):
                    print(f"{mode}: exact {position}", file=sys.stderr, flush=True)
                    exact = client.thumbnail(mode, position)
                    assert exact["cached"], (mode, position, exact)
                    if not args.legacy:
                        assert abs(exact["pts"] - position) <= 1 / 30 + 1e-6, exact
                        assert exact["decoded-frames"] <= int((position % 6) * 30) + 2, exact
                        fast = client.thumbnail(mode, position, "keyframes")
                        assert fast["cached"] and fast["precision"] == "keyframes", fast
                        assert fast["decoded-frames"] == 1, fast
                        assert fast["decoded-packets"] <= 8, fast
                        assert fast["decoder-reused"], fast
                        assert (fast["w"], fast["h"]) == (exact["w"], exact["h"])
                    else:
                        fast = None
                    records.append({"target": position, "exact": exact, "preview": fast})
                result = {"coldFirst": cold, "requests": records}
                if not args.legacy:
                    print(f"{mode}: cancellation", file=sys.stderr, flush=True)
                    if mode == "cache":
                        assert not client.thumbnail(mode, 100, "keyframes")["cached"]
                    result["cancelAndReplacementMs"] = client.cancellation(mode)
                    if mode == "cache":
                        assert server.requests == requests, (requests, server.requests)
                    first = client.thumbnail(mode, 1, "keyframes")
                    print(f"{mode}: track replacement", file=sys.stderr, flush=True)
                    client.command("set", "vid", "2")
                    second = client.thumbnail(mode, 1, "keyframes")
                    # A newly selected cached track has no packets until it is read.
                    if mode == "local":
                        assert second["cached"] and not second["decoder-reused"], second
                        assert first["pixelSha256"] != second["pixelSha256"]
                    print(f"{mode}: file replacement", file=sys.stderr, flush=True)
                    client.load(str(media))
                    fresh = client.thumbnail("local", 1, "keyframes")
                    assert fresh["cached"] and not fresh["decoder-reused"], fresh
                results[mode] = result
            finally:
                print(f"{mode}: destroy", file=sys.stderr, flush=True)
                try:
                    client.close()
                except OSError as error:
                    raise RuntimeError(f"{mode} native teardown failed") from error
        if not args.legacy:
            for cycle in range(3):
                print(f"teardown cycle {cycle}", file=sys.stderr, flush=True)
                client = Mpv(args.library)
                try:
                    client.load(str(media))
                    assert client.thumbnail("local", 1, "keyframes")["cached"]
                    assert client.thumbnail("cache", 1, "keyframes")["cached"]
                finally:
                    client.close()
            results["completedTeardownCycles"] = 3
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        assert not thread.is_alive()
    print(json.dumps(results))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=lambda path: str(Path(path).resolve(strict=True)))
    parser.add_argument("--baseline", type=lambda path: str(Path(path).resolve(strict=True)))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--width", type=int, default=1920)
    parser.add_argument("--height", type=int, default=1080)
    parser.add_argument("--codec", choices=("h264", "hevc"), default="h264")
    parser.add_argument("--fixture", help=argparse.SUPPRESS)
    parser.add_argument("--legacy", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.fixture:
        exercise(args)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-thumbnail-test-") as directory:
        fixture = Path(directory) / "long-gop.mp4"
        size = f"{args.width}x{args.height}"
        codec = ["-c:v", "libx264", "-pix_fmt", "yuv420p"] if args.codec == "h264" else [
            "-c:v", "libx265", "-pix_fmt", "yuv420p10le", "-x265-params",
            "keyint=180:min-keyint=180:scenecut=0:open-gop=0:pools=2:frame-threads=2:log-level=error"]
        subprocess.run([
            args.ffmpeg, "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", f"testsrc2=size={size}:rate=30",
            "-f", "lavfi", "-i", f"color=blue:size={size}:rate=30",
            "-map", "0:v", "-map", "1:v", "-t", "12",
            *codec, "-preset", "ultrafast", "-threads", "2",
            "-g", "180", "-keyint_min", "180", "-sc_threshold", "0",
            "-bf", "2", "-movflags", "+faststart",
            str(fixture)], check=True, timeout=180)
        reports = {}
        for name, library in (("current", args.library), ("baseline", args.baseline)):
            if not library:
                continue
            command = [sys.executable, __file__, library, "--fixture", str(fixture)]
            if name == "baseline":
                command.append("--legacy")
            try:
                completed = subprocess.run(command, capture_output=True, text=True, timeout=180)
            except subprocess.TimeoutExpired as error:
                progress = error.stderr or b""
                if isinstance(progress, bytes):
                    progress = progress.decode("utf-8", errors="replace")
                raise TimeoutError(f"{name} {args.codec} regression timed out:\n{progress[-6000:]}") from error
            if completed.returncode:
                raise RuntimeError(f"{name} {args.codec} regression failed:\n"
                                   + completed.stderr[-6000:] + completed.stdout[-1000:])
            reports[name] = json.loads(completed.stdout)
        if args.baseline:
            for mode in ("local", "cache"):
                current = reports["current"][mode]["requests"]
                baseline = reports["baseline"][mode]["requests"]
                for after, before in zip(current, baseline, strict=True):
                    assert after["exact"]["pixelSha256"] == before["exact"]["pixelSha256"], (mode, after, before)
                reports["current"][mode]["previewSpeedup"] = (
                    statistics.median(item["exact"]["milliseconds"] for item in baseline) /
                    statistics.median(item["preview"]["milliseconds"] for item in current))
                reports["current"][mode]["coldPreviewSpeedup"] = (
                    reports["baseline"][mode]["coldFirst"]["milliseconds"] /
                    reports["current"][mode]["coldFirst"]["milliseconds"])
        reports["codec"] = args.codec
        with Path(args.library).open("rb") as stream:
            reports["librarySha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()
