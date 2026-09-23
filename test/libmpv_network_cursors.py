#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Prove that the byte-range cache keeps one connection per file region.

The fixture mp4 stores its subtitle samples far away from the audio samples
with the same timestamps, so the mov demuxer alternates between both regions
of the file for every subtitle sample. The fixture server answers the media
URL with a 302 to a freshly signed CDN URL, as an Emby 302 deployment does,
and delays every response, so each region switch that needs a new connection
costs a full redirect chain. The cases verify:

- default: playback is byte-exact and needs only a few redirect chains,
  because each region keeps its own connection;
- legacy: with one connection and no forward reading, the same file needs a
  redirect chain for every region switch, i.e. the fixture reproduces the
  defect the default case guards against;
- exclusive: a server that refuses a request while another response is still
  in flight still plays byte-exact, and the cache falls back to one
  connection after a bounded number of failures;
- ranged-open: an additional connection starts right at the offset it serves,
  and the open-time probe of the file end costs no request from offset 0.
"""

import array
import contextlib
import ctypes
import http.server
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

AUDIO_RATE = 48000
AUDIO_CHANNELS = 2
SECONDS = 12
CHUNK_FRAMES = AUDIO_RATE // 4
# Far beyond the forward-read limit (4 MiB) and every stream buffer.
SUBTITLE_GAP = 24 * 1024 * 1024
# Within the forward-read limit: one subtitle connection reads through.
SUBTITLE_SPACING = 512 * 1024
# Filler between the last subtitle sample and the end of mdat, which keeps
# the subtitle region out of the file tail (see TAIL_THRESHOLD).
TRAILING_FILLER = 4 * 1024 * 1024
# The reported file had a 67-byte box after mdat, so the demuxer read the
# file end while opening it.
TAIL_SIZE = 67
# The reported file was 9.2 GB, so only its last 64 MiB counted as the tail
# that the cache prefetches into. This fixture is far smaller than that
# default threshold; a small threshold keeps the prefetch confined to the
# actual end of the file as it was there.
TAIL_THRESHOLD = "1MiB"
ORIGIN_LATENCY = 0.05
CDN_LATENCY = 0.1
# How long the exclusive server waits for another response to finish before
# refusing. Covers the gap between a client closing one connection and
# opening the next.
EXCLUSIVE_GRACE = 0.05

MATRIX = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)


def box(kind, *payload):
    data = b"".join(payload)
    return struct.pack(">I4s", 8 + len(data), kind) + data


def full_box(kind, version, flags, *payload):
    return box(kind, struct.pack(">I", (version << 24) | flags), *payload)


def tkhd(track_id, duration_ms, volume):
    return full_box(b"tkhd", 0, 3,
                    struct.pack(">5I", 0, 0, track_id, 0, duration_ms),
                    bytes(8), struct.pack(">4h", 0, 0, volume, 0), MATRIX,
                    struct.pack(">2I", 0, 0))


def mdhd(timescale, duration):
    return full_box(b"mdhd", 0, 0,
                    struct.pack(">4I2H", 0, 0, timescale, duration, 0x55C4, 0))


def hdlr(kind, name):
    return full_box(b"hdlr", 0, 0, struct.pack(">I4s", 0, kind), bytes(12),
                    name + b"\0")


def stco(offsets):
    return full_box(b"stco", 0, 0, struct.pack(">I", len(offsets)),
                    b"".join(struct.pack(">I", offset) for offset in offsets))


DINF = box(b"dinf", full_box(b"dref", 0, 0, struct.pack(">I", 1),
                             full_box(b"url ", 0, 1)))


class Media:
    """The fixture file and the values playback must reproduce from it."""

    def __init__(self):
        frames = AUDIO_RATE * SECONDS
        # Distinct values per frame and channel make any misplaced byte range
        # visible in the decoded output.
        samples = array.array("h", (((n * 7919) % 60001) - 30000
                                    for n in range(frames * AUDIO_CHANNELS)))
        if sys.byteorder != "little":
            samples.byteswap()
        self.pcm = samples.tobytes()
        self.texts = [f"line {n}" for n in range(SECONDS)]
        subtitles = [struct.pack(">H", len(text)) + text.encode()
                     for text in self.texts]

        ftyp = box(b"ftyp", b"isom", struct.pack(">I", 512),
                   b"isom", b"iso2", b"mp41")
        chunk_bytes = CHUNK_FRAMES * AUDIO_CHANNELS * 2
        chunks = frames // CHUNK_FRAMES

        def moov(audio_offsets, subtitle_offsets):
            return box(b"moov", self._mvhd(), self._audio_trak(frames, audio_offsets),
                       self._subtitle_trak(subtitles, subtitle_offsets))

        head = len(ftyp) + len(moov([0] * chunks, [0] * len(subtitles)))
        data_start = head + 8
        audio_offsets = [data_start + n * chunk_bytes for n in range(chunks)]
        self.audio_end = data_start + len(self.pcm)
        self.subtitle_start = self.audio_end + SUBTITLE_GAP
        subtitle_offsets = [self.subtitle_start + n * SUBTITLE_SPACING
                            for n in range(len(subtitles))]
        self.subtitle_end = subtitle_offsets[-1] + len(subtitles[-1])
        self.mdat_end = self.subtitle_end + TRAILING_FILLER

        body = bytearray(b"\xa5" * (self.mdat_end - data_start))
        body[:len(self.pcm)] = self.pcm
        for offset, sample in zip(subtitle_offsets, subtitles):
            body[offset - data_start:offset - data_start + len(sample)] = sample
        header = ftyp + moov(audio_offsets, subtitle_offsets)
        if len(header) != head:
            raise RuntimeError("The moov size changed with its chunk offsets")
        self.data = (header + struct.pack(">I4s", 8 + len(body), b"mdat")
                     + bytes(body) + box(b"free", bytes(TAIL_SIZE - 8)))

    @staticmethod
    def _mvhd():
        return full_box(b"mvhd", 0, 0,
                        struct.pack(">4I", 0, 0, 1000, SECONDS * 1000),
                        struct.pack(">IH", 0x10000, 0x100), bytes(10), MATRIX,
                        bytes(24), struct.pack(">I", 3))

    @staticmethod
    def _audio_trak(frames, offsets):
        # QuickTime-style PCM: one sample per frame, fixed chunk size.
        sowt = box(b"sowt", bytes(6), struct.pack(">H", 1),
                   struct.pack(">2HI", 0, 0, 0),
                   struct.pack(">2HhH", AUDIO_CHANNELS, 16, 0, 0),
                   struct.pack(">I", AUDIO_RATE << 16))
        stbl = box(b"stbl",
                   full_box(b"stsd", 0, 0, struct.pack(">I", 1), sowt),
                   full_box(b"stts", 0, 0, struct.pack(">3I", 1, frames, 1)),
                   full_box(b"stsc", 0, 0,
                            struct.pack(">4I", 1, 1, CHUNK_FRAMES, 1)),
                   full_box(b"stsz", 0, 0,
                            struct.pack(">2I", AUDIO_CHANNELS * 2, frames)),
                   stco(offsets))
        minf = box(b"minf", full_box(b"smhd", 0, 0, struct.pack(">hH", 0, 0)),
                   DINF, stbl)
        mdia = box(b"mdia", mdhd(AUDIO_RATE, frames),
                   hdlr(b"soun", b"SoundHandler"), minf)
        return box(b"trak", tkhd(1, SECONDS * 1000, 0x100), mdia)

    @staticmethod
    def _subtitle_trak(samples, offsets):
        # 3GPP timed text; one sample per chunk so each can live anywhere.
        tx3g = box(b"tx3g", bytes(6), struct.pack(">H", 1),
                   struct.pack(">I", 0), struct.pack(">bb", 1, -1),
                   struct.pack(">I", 0), struct.pack(">4h", 0, 0, 0, 0),
                   struct.pack(">3H2BI", 0, 0, 1, 0, 18, 0xFFFFFFFF),
                   box(b"ftab", struct.pack(">2HB", 1, 1, 5), b"Serif"))
        stbl = box(b"stbl",
                   full_box(b"stsd", 0, 0, struct.pack(">I", 1), tx3g),
                   full_box(b"stts", 0, 0,
                            struct.pack(">3I", 1, len(samples), 1000)),
                   full_box(b"stsc", 0, 0, struct.pack(">4I", 1, 1, 1, 1)),
                   full_box(b"stsz", 0, 0, struct.pack(">2I", 0, len(samples)),
                            b"".join(struct.pack(">I", len(sample))
                                     for sample in samples)),
                   stco(offsets))
        minf = box(b"minf", full_box(b"nmhd", 0, 0), DINF, stbl)
        mdia = box(b"mdia", mdhd(1000, len(samples) * 1000),
                   hdlr(b"sbtl", b"SubtitleHandler"), minf)
        return box(b"trak", tkhd(2, len(samples) * 1000, 0), mdia)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(10)
        with self.server.lock:
            self.server.connection_count += 1
            self.connection_id = self.server.connection_count

    def log_message(self, *args):
        pass

    def reply_empty(self, status, headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        server = self.server
        if not server.admit(self.connection_id):
            self.reply_empty(503)
            return
        path = urllib.parse.urlsplit(self.path).path
        if path == "/origin/media.mp4":
            with server.lock:
                server.origins.append(time.time())
                signature = len(server.origins)
            time.sleep(ORIGIN_LATENCY)
            self.reply_empty(302, [("Location", f"/cdn/media.mp4?sig={signature}")])
            return
        if path != "/cdn/media.mp4":
            self.reply_empty(404)
            return

        media = server.media
        start, end = 0, len(media) - 1
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.reply_empty(416)
                return
            start = int(match[1])
            if match[2]:
                end = min(end, int(match[2]))
            if start > end:
                self.reply_empty(416)
                return
        with server.lock:
            server.requests.append((time.time(), self.connection_id, start))
        time.sleep(CDN_LATENCY)
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if requested:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
        self.end_headers()
        server.set_active(self.connection_id, True)
        try:
            view = memoryview(media)
            while start <= end:
                size = min(65536, end + 1 - start)
                self.wfile.write(view[start:start + size])
                start += size
        except (OSError, socket.timeout):
            # The client abandoned the response; so does this connection.
            self.close_connection = True
        finally:
            server.set_active(self.connection_id, False)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, media):
        super().__init__(("127.0.0.1", 0), Handler)
        self.media = media
        self.lock = threading.Lock()
        self.changed = threading.Condition(self.lock)
        self.exclusive = False
        self.reset()

    def reset(self, exclusive=False):
        with self.lock:
            self.exclusive = exclusive
            self.connection_count = getattr(self, "connection_count", 0)
            self.origins = []
            self.requests = []
            self.refused = 0
            self.active = set()

    def set_active(self, connection_id, active):
        with self.changed:
            if active:
                self.active.add(connection_id)
            else:
                self.active.discard(connection_id)
            self.changed.notify_all()

    def admit(self, connection_id):
        """Refuse a request while another connection's response is in flight."""
        with self.changed:
            if not self.exclusive:
                return True
            deadline = time.monotonic() + EXCLUSIVE_GRACE
            while self.active - {connection_id}:
                left = deadline - time.monotonic()
                if left <= 0:
                    self.refused += 1
                    return False
                self.changed.wait(left)
            return True


@contextlib.contextmanager
def serve(media):
    server = Server(media)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


class Event(ctypes.Structure):
    _fields_ = [
        ("event_id", ctypes.c_int),
        ("error", ctypes.c_int),
        ("reply_userdata", ctypes.c_uint64),
        ("data", ctypes.c_void_p),
    ]


class EndFile(ctypes.Structure):
    _fields_ = [("reason", ctypes.c_int), ("error", ctypes.c_int)]


class LogMessage(ctypes.Structure):
    _fields_ = [
        ("prefix", ctypes.c_char_p),
        ("level", ctypes.c_char_p),
        ("text", ctypes.c_char_p),
        ("log_level", ctypes.c_int),
    ]


EVENT_LOG_MESSAGE = 2
EVENT_END_FILE = 7
EVENT_PLAYBACK_RESTART = 21


def run_client(library, url, options, pcm_path, count):
    with contextlib.ExitStack() as stack:
        if os.name == "nt":
            stack.enter_context(os.add_dll_directory(str(Path(library).parent)))
        mpv = ctypes.CDLL(library)
        mpv.mpv_create.restype = ctypes.c_void_p
        mpv.mpv_set_option_string.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ]
        mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
        mpv.mpv_request_log_messages.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_command.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ]
        mpv.mpv_get_property_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_get_property_string.restype = ctypes.c_void_p
        mpv.mpv_free.argtypes = [ctypes.c_void_p]
        mpv.mpv_free.restype = None
        mpv.mpv_wait_event.argtypes = [ctypes.c_void_p, ctypes.c_double]
        mpv.mpv_wait_event.restype = ctypes.POINTER(Event)
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        client = mpv.mpv_create()
        if not client:
            raise RuntimeError("Could not create the native client")
        stack.callback(mpv.mpv_terminate_destroy, client)
        if mpv.mpv_request_log_messages(client, b"v") < 0:
            raise RuntimeError("Native diagnostic subscription failed")
        settings = {
            "config": "no",
            "load-scripts": "no",
            "terminal": "no",
            "ytdl": "no",
            "vo": "null",
            "ao": "pcm",
            "ao-pcm-file": pcm_path,
            "ao-pcm-waveheader": "no",
            "untimed": "yes",
            "keep-open": "yes",
            "sid": "1",
            "cache": "yes",
            "demuxer-max-bytes": "256MiB",
            "demuxer-max-back-bytes": "256MiB",
            "network-timeout": "10",
            "curl-enabled": "no",
            "stream-lru-cache-tail-threshold": TAIL_THRESHOLD,
            # Deterministic failures: no reconnects inside libavformat.
            "stream-lavf-o": "reconnect=0,reconnect_on_network_error=0",
        }
        settings.update(options)
        for name, value in settings.items():
            result = mpv.mpv_set_option_string(client, name.encode(), value.encode())
            if result < 0 and not (name == "curl-enabled" and result == -5):
                raise RuntimeError(f"Native option rejected: {name}")
        if mpv.mpv_initialize(client) < 0:
            raise RuntimeError("Native initialization failed")

        lines = []

        def command(*words):
            argv = (ctypes.c_char_p * (len(words) + 1))(*[w.encode() for w in words], None)
            if mpv.mpv_command(client, argv) < 0:
                raise RuntimeError(f"Native command failed: {words}")

        def prop(name):
            value = mpv.mpv_get_property_string(client, name.encode())
            if not value:
                return None
            try:
                return ctypes.string_at(value).decode("utf-8", errors="replace")
            finally:
                mpv.mpv_free(value)

        def pump(timeout, until):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(client, 0.05).contents
                if event.event_id == EVENT_LOG_MESSAGE:
                    message = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
                    text = message.text.decode("utf-8", errors="replace")
                    if "lru_cache:" in text or "reopen" in text:
                        lines.append(text.rstrip())
                elif event.event_id == EVENT_END_FILE:
                    end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
                    if until != "ended":
                        raise RuntimeError(
                            f"Unexpected end-file: reason={end.reason}, error={end.error}\n"
                            + "\n".join(lines[-20:]))
                    return None
                if until == "restart" and event.event_id == EVENT_PLAYBACK_RESTART:
                    return None
                if until == "eof" and prop("eof-reached") == "yes":
                    return time.time()
            raise TimeoutError(f"Native playback did not reach {until}\n"
                               + "\n".join(lines[-20:]))

        command("loadfile", url)
        eof_time = pump(90, "eof")

        texts = []
        for n in range(count):
            command("seek", f"{n + 0.5}", "absolute+exact")
            pump(10, "restart")
            deadline = time.monotonic() + 2
            text = prop("sub-text")
            while text != f"line {n}" and time.monotonic() < deadline:
                mpv.mpv_wait_event(client, 0.02)
                text = prop("sub-text")
            texts.append(text)

        # Stopping frees the stream, which logs the cache statistics.
        command("stop")
        pump(10, "ended")
        print(json.dumps({"eof_time": eof_time, "texts": texts, "log": lines}),
              flush=True)


def run_case(library, server, media, name, options, exclusive=False):
    server.reset(exclusive)
    url = f"http://127.0.0.1:{server.server_port}/origin/media.mp4"
    with tempfile.TemporaryDirectory(prefix="mpv-network-cursors-") as directory:
        pcm_path = str(Path(directory, "audio.pcm"))
        result = subprocess.run(
            [sys.executable, str(Path(__file__).resolve()), "--child",
             library, url, json.dumps(options), pcm_path, str(len(media.texts))],
            capture_output=True, text=True, timeout=180,
        )
        if result.returncode:
            raise RuntimeError(f"{name}: {result.stdout}{result.stderr}")
        report = json.loads(result.stdout.strip().splitlines()[-1])
        pcm = Path(pcm_path).read_bytes()

    if pcm != media.pcm:
        size = min(len(pcm), len(media.pcm))
        first = next((n for n in range(size) if pcm[n] != media.pcm[n]), size)
        raise RuntimeError(f"{name}: decoded audio differs at byte {first} "
                           f"({len(pcm)} of {len(media.pcm)} bytes)")
    if report["texts"] != media.texts:
        raise RuntimeError(f"{name}: subtitles {report['texts']} != {media.texts}")

    with server.lock:
        until = report["eof_time"]
        origins = [t for t in server.origins if t <= until]
        requests = [r for r in server.requests if r[0] <= until]
        refused = server.refused
    return report["log"], origins, requests, refused


def check_default(library, server, media):
    log, origins, requests, _ = run_case(library, server, media, "default", {})
    if len(origins) > 4:
        raise RuntimeError(f"default: {len(origins)} redirect chains, expected at "
                           f"most 4\n" + "\n".join(log))
    print(f"PASS cursors/default ({len(origins)} redirect chains)", flush=True)

    # The first request opens the file; every later one must start where
    # the connection that issued it is needed.
    from_start = [r for r in requests if r[2] == 0]
    if len(from_start) != 1:
        raise RuntimeError(f"ranged-open: {len(from_start)} requests from offset 0 "
                           f"in {[(r[1], r[2]) for r in requests]}")
    probe = [r for r in requests if r[2] >= media.mdat_end]
    in_subtitles = [r for r in requests
                    if media.subtitle_start <= r[2] < media.subtitle_end]
    if not probe or not in_subtitles:
        raise RuntimeError(f"ranged-open: no ranged request for the file end or the "
                           f"subtitle region in {[(r[1], r[2]) for r in requests]}")
    print("PASS cursors/ranged-open", flush=True)


def check_legacy(library, server, media):
    options = {"stream-lru-cache-cursors": "1",
               "stream-lru-cache-read-through": "0"}
    log, origins, _, _ = run_case(library, server, media, "legacy", options)
    if len(origins) < len(media.texts):
        raise RuntimeError(f"legacy: {len(origins)} redirect chains; the fixture "
                           f"no longer forces a switch per subtitle sample")
    print(f"PASS cursors/legacy ({len(origins)} redirect chains)", flush=True)


def check_exclusive(library, server, media):
    log, _, _, refused = run_case(library, server, media, "exclusive", {},
                                  exclusive=True)
    failed_opens = sum("could not open connection" in line for line in log)
    if refused < 1 or failed_opens > 2 or not any(
            "continuing with one" in line for line in log):
        raise RuntimeError(f"exclusive: refused={refused} failed_opens={failed_opens}\n"
                           + "\n".join(log))
    print(f"PASS cursors/exclusive ({refused} refused requests)", flush=True)


def main(library):
    media = Media()
    with serve(media.data) as server:
        check_default(library, server, media)
        check_legacy(library, server, media)
        check_exclusive(library, server, media)


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "--child":
        run_client(sys.argv[2], sys.argv[3], json.loads(sys.argv[4]),
                   sys.argv[5], int(sys.argv[6]))
    elif len(sys.argv) == 2:
        main(str(Path(sys.argv[1]).resolve(strict=True)))
    else:
        raise SystemExit("usage: libmpv_network_cursors.py <libmpv shared library>")
