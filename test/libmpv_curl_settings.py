#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Exercise libcurl settings against deterministic loopback HTTP servers."""

import array
import contextlib
import ctypes
import email.utils
import http.server
import io
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import wave

from libmpv_network_cursors import (
    EndFile,
    EVENT_END_FILE,
    Media,
    create_client,
    get_property,
    load_libmpv,
    run_command,
)

EVENT_FILE_LOADED = 8
ERROR_REASON = 4
CHILD_TIMEOUT = 30
STOP_LIMIT = 1.5


def make_wav():
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(16000))
    return output.getvalue()


def make_range_wav():
    samples = array.array(
        "h", (((index * 7919) % 60001) - 30000 for index in range(8000 * 6)))
    if sys.byteorder != "little":
        samples.byteswap()
    pcm = samples.tobytes()
    output = io.BytesIO()
    with wave.open(output, "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(pcm)
    return output.getvalue(), pcm


WAV = make_wav()
RANGE_WAV, RANGE_PCM = make_range_wav()
SRT = b"1\n00:00:00,000 --> 00:00:05,000\nloopback\n"
RAW = bytes(2 * 8000 * 24)
RANGE_CHUNK = 8192


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.changed = threading.Condition(self.lock)
        self.connection_count = 0
        self.scenario = None
        self.requests = []
        self.counts = {}
        self.first_429 = threading.Event()
        self.primary_blocked = threading.Event()
        self.subtitle_429 = threading.Event()
        self.range_retry_offset = None
        self.range_retry_succeeded = False
        self.range_retry_429s = 0

    def reset(self, scenario):
        with self.lock:
            self.scenario = scenario
            self.requests = []
            self.counts = {}
            self.first_429.clear()
            self.primary_blocked.clear()
            self.subtitle_429.clear()
            self.range_retry_offset = None
            self.range_retry_succeeded = False
            self.range_retry_429s = 0

    def record(self, path, connection_id, requested):
        with self.changed:
            count = self.counts.get(path, 0) + 1
            self.counts[path] = count
            self.requests.append(
                (path, connection_id, time.monotonic(), requested))
            self.changed.notify_all()
            return count

    def snapshot(self):
        with self.lock:
            return list(self.requests)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(10)
        with self.server.state.lock:
            self.server.state.connection_count += 1
            self.connection_id = self.server.state.connection_count

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", content_type="application/octet-stream",
              headers=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError, OSError):
                self.close_connection = True

    def media(self, body, content_type):
        start, end = 0, len(body) - 1
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.reply(416)
                return
            start = int(match[1])
            if match[2]:
                end = min(end, int(match[2]))
            if start > end:
                self.reply(416, headers=[("Content-Range", f"bytes */{len(body)}")])
                return
        headers = [("Accept-Ranges", "bytes")]
        if requested:
            headers.append(("Content-Range", f"bytes {start}-{end}/{len(body)}"))
        self.reply(206 if requested else 200, body[start:end + 1], content_type,
                   headers)

    def retry(self, path, count):
        scenario = self.server.state.scenario
        if scenario == "seconds" and count == 1:
            self.server.state.first_429.set()
            self.reply(429, headers=[("Retry-After", "1")])
        elif scenario == "date" and count == 1:
            retry = email.utils.formatdate(time.time() + 2, usegmt=True)
            self.server.state.first_429.set()
            self.reply(429, headers=[("Retry-After", retry)])
        elif scenario == "opt-out":
            self.reply(429, headers=[("Retry-After", "0")])
        elif scenario == "always-zero":
            self.reply(429, headers=[("Retry-After", "0")])
        elif scenario in ("delay-max", "budget", "cancel"):
            self.server.state.first_429.set()
            self.reply(429, headers=[("Retry-After", "3")])
        elif scenario == "ignore-date" and count == 1:
            self.reply(429, headers=[("Retry-After", "30")])
        elif scenario == "isolation" and count == 1:
            self.reply(429, headers=[("Retry-After", "0")])
        else:
            self.media(WAV, "audio/wav")

    def concurrent(self, path, count):
        state = self.server.state
        if path == "/subtitle.srt":
            if count == 1:
                state.subtitle_429.set()
                self.reply(429, headers=[("Retry-After", "3")])
            else:
                self.reply(200, SRT, "application/x-subrip")
            return

        requested = self.headers.get("Range")
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested or "")
        start = int(match[1]) if match else 0
        if start >= len(RAW):
            self.reply(416, headers=[("Content-Range", f"bytes */{len(RAW)}")])
            return
        end = min(len(RAW) - 1, start + 32767)
        if count == 2:
            state.primary_blocked.set()
            if not state.subtitle_429.wait(10):
                self.reply(500)
                return
        headers = [
            ("Accept-Ranges", "bytes"),
            ("Content-Range", f"bytes {start}-{end}/{len(RAW)}"),
        ]
        self.reply(206, RAW[start:end + 1], "application/octet-stream", headers)

    def range_retry(self, requested):
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested or "")
        if not match:
            self.reply(416)
            return
        start = int(match[1])
        if start >= len(RANGE_WAV):
            self.reply(
                416, headers=[("Content-Range", f"bytes */{len(RANGE_WAV)}")])
            return
        state = self.server.state
        send_429 = False
        with state.lock:
            if start > 0 and state.range_retry_offset is None:
                state.range_retry_offset = start
                state.range_retry_429s += 1
                send_429 = True
            elif (state.range_retry_offset is not None
                  and not state.range_retry_succeeded):
                if start != state.range_retry_offset:
                    self.reply(500)
                    return
                state.range_retry_succeeded = True
        if send_429:
            self.reply(429, headers=[("Retry-After", "0")])
            return
        end = min(len(RANGE_WAV) - 1, start + RANGE_CHUNK - 1)
        self.reply(
            206, RANGE_WAV[start:end + 1], "audio/wav",
            [
                ("Accept-Ranges", "bytes"),
                ("Content-Range", f"bytes {start}-{end}/{len(RANGE_WAV)}"),
            ],
        )

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        requested = self.headers.get("Range")
        count = self.server.state.record(path, self.connection_id, requested)
        scenario = self.server.state.scenario
        if scenario == "keepalive":
            self.media(WAV, "audio/wav")
        elif scenario in {
            "seconds", "date", "opt-out", "always-zero", "delay-max",
            "budget", "cancel", "ignore-date", "isolation",
        }:
            self.retry(path, count)
        elif scenario == "concurrent":
            self.concurrent(path, count)
        elif scenario == "range-retry":
            self.range_retry(requested)
        else:
            self.reply(404)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.state = State()


@contextlib.contextmanager
def serve():
    server = Server()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        if thread.is_alive():
            raise RuntimeError("Loopback fixture server did not stop")


def options(**extra):
    values = {
        "ao": "null",
        "pause": "no",
        "untimed": "yes",
        "keep-open": "yes",
        "cache": "no",
        "stream-lru-cache": "0",
        "curl-enabled": "yes",
        "network-timeout": "10",
    }
    values.update({name.replace("_", "-"): str(value) for name, value in extra.items()})
    return values


def wait_event(mpv, client, wanted, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(client, 0.05).contents
        if event.event_id == EVENT_END_FILE:
            end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
            if wanted == "end":
                return {"reason": end.reason, "error": end.error}
            raise RuntimeError(
                f"Unexpected end-file: reason={end.reason}, error={end.error}")
        if event.event_id == wanted:
            return None
    raise TimeoutError(f"Native event {wanted!r} did not arrive")


def load_complete(mpv, client, url, per_file=None):
    if per_file:
        run_command(mpv, client, "loadfile", url, "replace", "-1", per_file)
    else:
        run_command(mpv, client, "loadfile", url)
    wait_event(mpv, client, EVENT_FILE_LOADED)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(client, 0.05).contents
        if event.event_id == EVENT_END_FILE:
            end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
            raise RuntimeError(
                f"File ended before clean EOF: reason={end.reason}, error={end.error}")
        if get_property(mpv, client, "eof-reached") == "yes":
            run_command(mpv, client, "stop")
            wait_event(mpv, client, "end")
            return
    raise TimeoutError("Native playback did not reach EOF")


def load_failure(mpv, client, url, per_file=None, timeout=15):
    if per_file:
        run_command(mpv, client, "loadfile", url, "replace", "-1", per_file)
    else:
        run_command(mpv, client, "loadfile", url)
    result = wait_event(mpv, client, "end", timeout)
    if result["reason"] != ERROR_REASON or result["error"] >= 0:
        raise RuntimeError(f"Expected an explicit load failure, got {result}")
    return result


def child_keepalive(library, base, enabled):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options(
            **{"curl-http-keepalive": enabled}))
        load_complete(mpv, client, f"{base}/wav?load=1")
        load_complete(mpv, client, f"{base}/wav?load=2")
        print(json.dumps({"property": get_property(
            mpv, client, "curl-http-keepalive")}), flush=True)


def child_retry(library, url, case):
    case_options = {
        "seconds": {"curl-retry-http-429": "yes", "curl-max-retries": "1"},
        "date": {"curl-retry-http-429": "yes", "curl-max-retries": "1"},
        "opt-out": {},
        "always-zero": {
            "curl-retry-http-429": "yes", "curl-max-retries": "2",
        },
        "delay-max": {
            "curl-retry-http-429": "yes", "curl-max-retries": "1",
            "curl-retry-delay-max": "1",
        },
        "budget": {
            "curl-retry-http-429": "yes", "curl-max-retries": "1",
            "network-timeout": "1",
        },
        "ignore-date": {
            "curl-retry-http-429": "yes", "curl-max-retries": "1",
            "curl-respect-retry-after": "no",
        },
    }[case]
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options(**case_options))
        if case in ("seconds", "date", "ignore-date"):
            load_complete(mpv, client, url)
            result = {"success": True}
        else:
            result = load_failure(mpv, client, url)
        print(json.dumps(result), flush=True)


def child_range_retry(library, url, pcm_path):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options(**{
            "ao": "pcm",
            "keep-open": "no",
            "ao-pcm-file": pcm_path,
            "ao-pcm-waveheader": "no",
            "audio-format": "s16",
            "audio-samplerate": "8000",
            "audio-channels": "mono",
            "sid": "no",
            "curl-retry-http-429": "yes",
            "curl-max-retries": "1",
        }))
        run_command(mpv, client, "loadfile", url)
        wait_event(mpv, client, EVENT_FILE_LOADED)
        end = wait_event(mpv, client, "end")
        if end["reason"] != 0 or end["error"] < 0:
            raise RuntimeError(f"Range retry did not finish normally: {end}")
    print(json.dumps({"bytes": Path(pcm_path).stat().st_size}), flush=True)


def child_isolation(library, base):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options())
        load_complete(
            mpv, client, f"{base}/isolation-first",
            "curl-retry-http-429=yes,curl-max-retries=1")
        after_first = get_property(mpv, client, "curl-retry-http-429")
        load_failure(mpv, client, f"{base}/isolation-second")
        after_second = get_property(mpv, client, "curl-retry-http-429")
        print(json.dumps({
            "after_first": after_first,
            "after_second": after_second,
        }), flush=True)


def child_cancel(library, url):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options(**{
            "curl-retry-http-429": "yes",
            "curl-max-retries": "1",
        }))
        requested = threading.Event()
        threading.Thread(
            target=lambda: (sys.stdin.readline(), requested.set()),
            daemon=True,
        ).start()
        run_command(mpv, client, "loadfile", url)
        while not requested.wait(0.02):
            event = mpv.mpv_wait_event(client, 0).contents
            if event.event_id == EVENT_END_FILE:
                raise RuntimeError("Retrying file ended before cancellation")
        started = time.monotonic()
        run_command(mpv, client, "stop")
        result = wait_event(mpv, client, "end", STOP_LIMIT)
        print(json.dumps({
            "latency": time.monotonic() - started,
            "reason": result["reason"],
        }), flush=True)


def child_concurrent(library, base):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options(**{
            "demuxer": "rawaudio",
            "demuxer-rawaudio-rate": "8000",
            "demuxer-rawaudio-channels": "mono",
            "demuxer-rawaudio-format": "s16le",
            "curl-retry-http-429": "yes",
            "curl-max-retries": "1",
        }))
        run_command(mpv, client, "loadfile", f"{base}/primary.raw")
        wait_event(mpv, client, EVENT_FILE_LOADED)
        result = []

        def add_subtitle():
            try:
                run_command(
                    mpv, client, "sub-add", f"{base}/subtitle.srt", "select")
                result.append("ok")
            except Exception as error:
                result.append(repr(error))

        thread = threading.Thread(target=add_subtitle)
        thread.start()
        thread.join(timeout=12)
        if thread.is_alive():
            raise TimeoutError("Asynchronous subtitle command did not finish")
        if result != ["ok"]:
            raise RuntimeError(f"Subtitle command failed: {result}")
        run_command(mpv, client, "stop")
        wait_event(mpv, client, "end")
        print(json.dumps({"subtitle": get_property(
            mpv, client, "sub-text")}), flush=True)


def run_child(library, mode, *args):
    command = [
        sys.executable, str(Path(__file__).resolve()), "--child",
        library, mode, *args,
    ]
    result = subprocess.run(
        command, capture_output=True, text=True, timeout=CHILD_TIMEOUT)
    if result.returncode:
        raise RuntimeError(f"{mode}: {result.stdout}{result.stderr}")
    lines = result.stdout.strip().splitlines()
    return json.loads(lines[-1]) if lines else {}


def requests_for(server, path):
    return [entry for entry in server.state.snapshot() if entry[0] == path]


def check_keepalive(library, server):
    base = f"http://127.0.0.1:{server.server_port}"
    for enabled in ("yes", "no"):
        server.state.reset("keepalive")
        report = run_child(library, "keepalive", base, enabled)
        requests = requests_for(server, "/wav")
        ids = [entry[1] for entry in requests]
        if len(requests) != 2:
            raise RuntimeError(
                f"keepalive={enabled}: expected two complete loads, got {requests}")
        if enabled == "yes" and len(set(ids)) != 1:
            raise RuntimeError(f"keepalive=yes did not reuse its connection: {ids}")
        if enabled == "no" and len(set(ids)) != 2:
            raise RuntimeError(f"keepalive=no reused a connection: {ids}")
        if report["property"] != enabled:
            raise RuntimeError(f"keepalive property mismatch: {report}")
        print(f"PASS curl-settings/keepalive-{enabled}", flush=True)


def check_retries(library, server):
    base = f"http://127.0.0.1:{server.server_port}"
    expectations = {
        "seconds": (2, True),
        "date": (2, True),
        "opt-out": (1, False),
        "always-zero": (3, False),
        "delay-max": (1, False),
        "budget": (1, False),
        "ignore-date": (2, True),
    }
    for case, (count, success) in expectations.items():
        server.state.reset(case)
        started = time.monotonic()
        report = run_child(library, "retry", f"{base}/retry", case)
        elapsed = time.monotonic() - started
        requests = requests_for(server, "/retry")
        if len(requests) != count:
            raise RuntimeError(
                f"{case}: expected {count} requests, got {len(requests)}")
        if bool(report.get("success")) != success:
            raise RuntimeError(f"{case}: unexpected result {report}")
        if case == "seconds" and requests[1][2] - requests[0][2] < 0.8:
            raise RuntimeError("Retry-After seconds was not respected")
        if case == "date" and requests[1][2] - requests[0][2] < 0.7:
            raise RuntimeError("Retry-After HTTP-date was not respected")
        if case == "ignore-date" and requests[1][2] - requests[0][2] > 5:
            raise RuntimeError("curl-respect-retry-after=no still waited for the date")
        if case in ("delay-max", "budget") and elapsed > 2.5:
            raise RuntimeError(
                f"{case}: rejected retry exceeded its budget ({elapsed:.2f} s)")
        print(f"PASS curl-settings/retry-{case}", flush=True)


def check_range_retry(library, server):
    server.state.reset("range-retry")
    pcm_path = Path.cwd() / f".libmpv-curl-range-{os.getpid()}.pcm"
    pcm_path.unlink(missing_ok=True)
    try:
        url = f"http://127.0.0.1:{server.server_port}/range-retry.wav"
        report = run_child(library, "range-retry", url, str(pcm_path))
        pcm = pcm_path.read_bytes()
    finally:
        pcm_path.unlink(missing_ok=True)
    requests = requests_for(server, "/range-retry.wav")
    offsets = [
        int(match[1])
        for request in requests
        if (match := re.fullmatch(r"bytes=(\d+)-(\d*)", request[3] or ""))
    ]
    nonzero = next(
        (index for index, offset in enumerate(offsets) if offset > 0), None)
    if nonzero is None or nonzero + 1 >= len(offsets):
        raise RuntimeError(f"No retried nonzero range was observed: {offsets}")
    if offsets[nonzero] != offsets[nonzero + 1]:
        raise RuntimeError(
            f"The 429 retry changed offsets: {offsets[nonzero:nonzero + 2]}")
    if (server.state.range_retry_429s != 1
            or not server.state.range_retry_succeeded):
        raise RuntimeError("The nonzero range did not complete one 429 retry")
    if report["bytes"] != len(RANGE_PCM) or pcm != RANGE_PCM:
        size = min(len(pcm), len(RANGE_PCM))
        first = next(
            (index for index in range(size) if pcm[index] != RANGE_PCM[index]),
            size,
        )
        raise RuntimeError(
            f"Decoded PCM differs at byte {first} "
            f"({len(pcm)} of {len(RANGE_PCM)} bytes)")
    print(
        f"PASS curl-settings/range-retry-offset-{offsets[nonzero]}",
        flush=True,
    )


def check_isolation(library, server):
    server.state.reset("isolation")
    base = f"http://127.0.0.1:{server.server_port}"
    report = run_child(library, "isolation", base)
    if len(requests_for(server, "/isolation-first")) != 2:
        raise RuntimeError("Per-file retry option did not retry its own file")
    if len(requests_for(server, "/isolation-second")) != 1:
        raise RuntimeError("Per-file retry option leaked into the next file")
    if report != {"after_first": "no", "after_second": "no"}:
        raise RuntimeError(f"Per-file retry property was not restored: {report}")
    print("PASS curl-settings/per-file-isolation", flush=True)


def check_cancel(library, server):
    server.state.reset("cancel")
    url = f"http://127.0.0.1:{server.server_port}/cancel"
    process = subprocess.Popen(
        [sys.executable, str(Path(__file__).resolve()), "--child",
         library, "cancel", url],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True,
    )
    try:
        if not server.state.first_429.wait(5):
            raise TimeoutError("Cancellation fixture did not observe the initial 429")
        process.stdin.write("\n")
        process.stdin.flush()
        stdout, stderr = process.communicate(timeout=5)
    except BaseException:
        process.kill()
        process.communicate(timeout=5)
        raise
    if process.returncode:
        raise RuntimeError(f"cancel: {stdout}{stderr}")
    report = json.loads(stdout.strip().splitlines()[-1])
    if report["latency"] > STOP_LIMIT:
        raise RuntimeError(f"Cancellation took {report['latency']:.2f} seconds")
    deadline = time.monotonic() + 3.3
    with server.state.changed:
        while len([entry for entry in server.state.requests
                   if entry[0] == "/cancel"]) == 1:
            left = deadline - time.monotonic()
            if left <= 0:
                break
            server.state.changed.wait(left)
    if len(requests_for(server, "/cancel")) != 1:
        raise RuntimeError("A retry request arrived after cancellation")
    print(f"PASS curl-settings/cancel ({report['latency']:.2f} s)", flush=True)


def check_concurrent(library, server):
    server.state.reset("concurrent")
    base = f"http://127.0.0.1:{server.server_port}"
    run_child(library, "concurrent", base)
    primary = requests_for(server, "/primary.raw")
    subtitle = requests_for(server, "/subtitle.srt")
    if not server.state.primary_blocked.is_set():
        raise RuntimeError("The primary continuation was never held for the 429")
    if len(primary) < 3 or len(subtitle) != 2:
        raise RuntimeError(
            f"Concurrent fixture was incomplete: primary={primary}, subtitle={subtitle}")
    if subtitle[1][2] - subtitle[0][2] < 2.7:
        raise RuntimeError("The subtitle retry did not retain its three-second delay")
    if primary[2][2] < subtitle[0][2]:
        raise RuntimeError("The primary continuation escaped before the 429")
    if primary[2][2] >= subtitle[1][2]:
        raise RuntimeError(
            "The primary continuation waited behind the subtitle retry delay")
    print("PASS curl-settings/nonblocking-concurrent", flush=True)


class FamilyHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", content_type="application/octet-stream",
              headers=()):
        self.send_response(status)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Content-Type", content_type)
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if body:
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        with self.server.lock:
            self.server.requests.append(path)
        if not self.server.good:
            self.reply(403)
            return
        port = self.server.server_port
        if path == "/redirect":
            self.reply(302, headers=[
                ("Location", f"http://localhost:{port}/playlist.m3u8")])
        elif path == "/playlist.m3u8":
            body = (
                "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:12\n"
                "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:12.0,\n"
                f"http://localhost:{port}/segment.mp4\n#EXT-X-ENDLIST\n"
            ).encode()
            self.reply(200, body, "application/vnd.apple.mpegurl")
        elif path == "/segment.mp4":
            data = self.server.media
            requested = self.headers.get("Range")
            start, end = 0, len(data) - 1
            if requested:
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
                if not match:
                    self.reply(416)
                    return
                start = int(match[1])
                if match[2]:
                    end = min(end, int(match[2]))
            headers = [("Accept-Ranges", "bytes")]
            if requested:
                headers.append(
                    ("Content-Range", f"bytes {start}-{end}/{len(data)}"))
            self.reply(206 if requested else 200, data[start:end + 1],
                       "video/mp4", headers)
        else:
            self.reply(404)


class FamilyServer(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, family, host, port, good, media):
        self.address_family = family
        super().__init__((host, port), FamilyHandler)
        self.good = good
        self.media = media
        self.lock = threading.Lock()
        self.requests = []


@contextlib.contextmanager
def family_serve(media):
    for _ in range(20):
        v4 = FamilyServer(socket.AF_INET, "127.0.0.1", 0, True, media)
        try:
            v6 = FamilyServer(
                socket.AF_INET6, "::1", v4.server_port, False, media)
        except OSError:
            v4.server_close()
            continue
        threads = [
            threading.Thread(target=server.serve_forever, daemon=True)
            for server in (v4, v6)
        ]
        for thread in threads:
            thread.start()
        try:
            yield v4, v6
        finally:
            for server in (v4, v6):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join(timeout=5)
                if thread.is_alive():
                    raise RuntimeError("Dual-stack fixture server did not stop")
        return
    raise RuntimeError("Could not bind dual-stack loopback fixture")


def child_hls(library, url):
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = create_client(stack, mpv, options())
        load_complete(
            mpv, client, url, "curl-prefer-family=ipv4")
        print(json.dumps({
            "restored": get_property(mpv, client, "curl-prefer-family"),
        }), flush=True)


def has_dual_stack():
    if not socket.has_ipv6:
        return False
    families = {
        item[0] for item in socket.getaddrinfo(
            "localhost", 80, type=socket.SOCK_STREAM)
    }
    return {socket.AF_INET, socket.AF_INET6} <= families


def check_hls(library):
    if not has_dual_stack():
        print("SKIP curl-settings/redirect-nested-hls "
              "(no dual-stack localhost)", flush=True)
        return
    media = Media().data
    with family_serve(media) as (v4, v6):
        url = f"http://127.0.0.1:{v4.server_port}/redirect"
        report = run_child(library, "hls", url)
        if "/redirect" not in v4.requests:
            raise RuntimeError("The literal redirect request was not observed")
        if "/playlist.m3u8" not in v4.requests or "/segment.mp4" not in v4.requests:
            raise RuntimeError(
                f"Redirected/nested IPv4 requests were incomplete: {v4.requests}")
        if v6.requests:
            raise RuntimeError(
                f"Per-file family did not propagate through HLS: {v6.requests}")
        if report["restored"] != "auto":
            raise RuntimeError(f"Per-file family was not restored: {report}")
    print("PASS curl-settings/redirect-nested-hls", flush=True)


CASES = {
    "keepalive",
    "retry",
    "range-retry",
    "isolation",
    "cancel",
    "concurrent",
    "hls",
}


def main(library, selected):
    unknown = set(selected) - CASES
    if unknown:
        raise SystemExit(f"Unknown cases: {', '.join(sorted(unknown))}")
    selected = set(selected) if selected else CASES
    if "hls" in selected:
        check_hls(library)
    with serve() as server:
        if "keepalive" in selected:
            check_keepalive(library, server)
        if "retry" in selected:
            check_retries(library, server)
        if "range-retry" in selected:
            check_range_retry(library, server)
        if "isolation" in selected:
            check_isolation(library, server)
        if "cancel" in selected:
            check_cancel(library, server)
        if "concurrent" in selected:
            check_concurrent(library, server)


if __name__ == "__main__":
    if len(sys.argv) >= 5 and sys.argv[1] == "--child":
        library, mode, args = sys.argv[2], sys.argv[3], sys.argv[4:]
        if mode == "keepalive":
            child_keepalive(library, *args)
        elif mode == "retry":
            child_retry(library, *args)
        elif mode == "range-retry":
            child_range_retry(library, *args)
        elif mode == "isolation":
            child_isolation(library, *args)
        elif mode == "cancel":
            child_cancel(library, *args)
        elif mode == "concurrent":
            child_concurrent(library, *args)
        elif mode == "hls":
            child_hls(library, *args)
        else:
            raise SystemExit(f"Unknown child mode: {mode}")
    elif len(sys.argv) >= 2:
        main(str(Path(sys.argv[1]).resolve(strict=True)), sys.argv[2:])
    else:
        raise SystemExit(
            "usage: libmpv_curl_settings.py <absolute libmpv library> [case ...]")
