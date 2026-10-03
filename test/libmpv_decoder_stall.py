#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Check a stalled null audio sink using an exact local libmpv runtime.

No real window, audio device, private media, or remote server is used.
--legacy proves the previous unbounded attempt-1 recovery on an older DLL.
"""

import argparse
import contextlib
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import threading
import time

from libmpv_network_diagnostics import FixtureServer, element, integer


def audiovisual_fixture():
    header = element(
        "1a45dfa3",
        integer("4286", 1) + integer("42f7", 1)
        + integer("42f2", 4) + integer("42f3", 8)
        + element("4282", b"matroska") + integer("4287", 4)
        + integer("4285", 2),
    )
    info = element(
        "1549a966",
        integer("2ad7b1", 1000000, 3)
        + element("4489", struct.pack(">d", 10000)),
    )
    bitmap = struct.pack("<IiiHHIIiiII", 40, 16, 16, 1, 24, 0, 768,
                         0, 0, 0, 0)
    video = element(
        "ae",
        integer("d7", 1) + integer("73c5", 1) + integer("83", 1)
        + element("86", b"V_MS/VFW/FOURCC") + element("63a2", bitmap)
        + integer("23e383", 40000000, 4)
        + element("e0", integer("b0", 16) + integer("ba", 16)),
    )
    audio = element(
        "ae",
        integer("d7", 2) + integer("73c5", 2) + integer("83", 2)
        + element("86", b"A_PCM/INT/LIT")
        + element(
            "e1", element("b5", struct.pack(">d", 8000))
            + integer("9f", 1) + integer("6264", 16),
        ),
    )
    clusters = b"".join(
        element(
            "1f43b675", integer("e7", frame * 40, 2)
            + element("a3", b"\x81\x00\x00\x80" + bytes(768))
            + element("a3", b"\x82\x00\x00\x80" + bytes(640)),
        )
        for frame in range(250)
    )
    return header + element(
        "18538067", info + element("1654ae6b", video + audio) + clusters)


class Event(c.Structure):
    _fields_ = [
        ("id", c.c_int), ("error", c.c_int),
        ("userdata", c.c_uint64), ("data", c.c_void_p),
    ]


class Log(c.Structure):
    _fields_ = [
        ("prefix", c.c_char_p), ("level", c.c_char_p),
        ("text", c.c_char_p), ("log_level", c.c_int),
    ]


class Property(c.Structure):
    _fields_ = [
        ("name", c.c_char_p), ("format", c.c_int), ("data", c.c_void_p),
    ]


def load_api(library):
    api = c.CDLL(str(library))
    signatures = {
        "mpv_create": (c.c_void_p, []),
        "mpv_initialize": (c.c_int, [c.c_void_p]),
        "mpv_set_option_string":
            (c.c_int, [c.c_void_p, c.c_char_p, c.c_char_p]),
        "mpv_set_property_string":
            (c.c_int, [c.c_void_p, c.c_char_p, c.c_char_p]),
        "mpv_request_log_messages":
            (c.c_int, [c.c_void_p, c.c_char_p]),
        "mpv_observe_property":
            (c.c_int, [c.c_void_p, c.c_uint64, c.c_char_p, c.c_int]),
        "mpv_command":
            (c.c_int, [c.c_void_p, c.POINTER(c.c_char_p)]),
        "mpv_wait_event":
            (c.POINTER(Event), [c.c_void_p, c.c_double]),
        "mpv_terminate_destroy": (None, [c.c_void_p]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(api, name)
        function.restype, function.argtypes = result, arguments
    return api


class Client:
    def __init__(self, api, stalled):
        self.api = api
        self.handle = api.mpv_create()
        assert self.handle, "mpv_create"
        self.loaded = False
        self.position = None
        self.records = []
        self.legacy = []
        initialized = False
        try:
            options = {
                "config": "no", "load-scripts": "no", "terminal": "no",
                "vo": "null", "ao": "null", "sid": "no",
                "hwdec": "no", "pause": "yes" if stalled else "no",
                "ao-null-speed": "0" if stalled else "1",
                "ao-null-buffer": "0.1", "cache": "yes",
                "cache-pause": "no",
                "decoder-stall-recovery-timeout": "0.25",
                "decoder-stall-recovery-attempts": "3",
            }
            for name, value in options.items():
                assert api.mpv_set_option_string(
                    self.handle, name.encode(), value.encode()) >= 0, name
            assert api.mpv_request_log_messages(
                self.handle, b"info") >= 0
            assert api.mpv_initialize(self.handle) >= 0
            assert api.mpv_observe_property(
                self.handle, 1, b"time-pos", 5) >= 0
            initialized = True
        finally:
            if not initialized:
                self.close()

    def close(self):
        if self.handle:
            self.api.mpv_terminate_destroy(self.handle)
            self.handle = None

    def load(self, url):
        self.loaded = False
        self.position = None
        arguments = (c.c_char_p * 3)(b"loadfile", url.encode(), None)
        assert self.api.mpv_command(self.handle, arguments) >= 0
        self.wait_until(lambda: self.loaded, 10)

    def event(self):
        event = self.api.mpv_wait_event(self.handle, 0.05).contents
        if event.id == 8:
            self.loaded = True
        elif event.id == 2:
            log = c.cast(event.data, c.POINTER(Log)).contents
            text = log.text.decode("utf-8").strip()
            if text.startswith("playback_stall "):
                assert len(text) <= 1000, len(text)
                assert "http" not in text and "DO_NOT_LOG" not in text
                fields = dict(re.findall(r"(\w+)=([^\s]+)", text))
                fields["level"] = log.level.decode("ascii")
                self.records.append(fields)
            elif text.startswith("Decoder stalled at"):
                self.legacy.append(text)
        elif event.id == 22:
            prop = c.cast(event.data, c.POINTER(Property)).contents
            if prop.name == b"time-pos" and prop.format == 5 and prop.data:
                self.position = c.cast(
                    prop.data, c.POINTER(c.c_double)).contents.value

    def wait_until(self, predicate, seconds):
        deadline = time.monotonic() + seconds
        while not predicate() and time.monotonic() < deadline:
            self.event()
        assert predicate(), {
            "records": self.records, "legacy": self.legacy[-5:],
            "position": self.position,
        }

    def drain(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.event()


def check_stalled_output(api, url, legacy):
    with contextlib.closing(Client(api, stalled=True)) as client:
        client.load(url)
        client.drain(0.8)
        assert not client.records and not client.legacy, "recovered while paused"
        assert api.mpv_set_property_string(
            client.handle, b"pause", b"no") >= 0
        if legacy:
            client.wait_until(lambda: len(client.legacy) >= 4, 10)
            attempts = [
                re.search(r"attempt (\d+)/3", text).group(1)
                for text in client.legacy
            ]
            assert set(attempts) == {"1"}, attempts
            return {"legacyAttempts": attempts}

        client.wait_until(
            lambda: any(r["event"] == "exhausted" for r in client.records),
            10)
        client.drain(0.8)
        assert [(r["event"], r["attempt"]) for r in client.records] == [
            ("seek", "1/3"), ("seek", "2/3"), ("seek", "3/3"),
            ("exhausted", "3/3"),
        ], client.records
        required = {
            "stalled_seconds", "playback_pts", "video_pts",
            "audio_written_pts", "video_status", "audio_status",
            "restart_complete", "paused", "paused_for_cache", "seek_type",
            "hrseek", "video_queued", "video_frames_queued",
            "video_underrun", "audio_underrun", "audio_eof",
            "audio_queue_samples", "av_difference", "video_wait",
            "cache_percent", "cache_seconds", "cache_audio_seconds",
            "cache_video_seconds", "cache_bytes", "cache_bytes_per_second",
            "cache_idle", "cache_eof", "cache_underrun", "stream_error",
            "seekable",
        }
        for record in client.records:
            assert required <= record.keys(), required - record.keys()
            assert float(record["stalled_seconds"]) >= 0.25
            assert record["paused"] == record["paused_for_cache"] == "0"
            assert record["restart_complete"] == record["seekable"] == "1"
            assert record["level"] == (
                "error" if record["event"] == "exhausted" else "warn")

        client.load(url + "&replacement=1")
        client.wait_until(lambda: len(client.records) > 4, 10)
        assert client.records[4]["event"] == "seek"
        assert client.records[4]["attempt"] == "1/3"
        return {
            "boundedAttempts": [r["attempt"] for r in client.records[:4]],
            "snapshotFields": len(required), "replacementAttempt": "1/3",
        }


def check_normal_output(api, url):
    with contextlib.closing(Client(api, stalled=False)) as client:
        client.load(url)
        client.wait_until(
            lambda: client.position is not None and client.position >= 1,
            10)
        client.drain(0.8)
        assert not client.records and not client.legacy
        return {"normalPositionSeconds": client.position}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--legacy", action="store_true")
    args = parser.parse_args()
    library = args.library.resolve(strict=True)
    digest = hashlib.sha256(library.read_bytes()).hexdigest()
    directory = os.add_dll_directory(str(library.parent)) if os.name == "nt" \
        else contextlib.nullcontext()
    with directory:
        api = load_api(library)
        server = FixtureServer()
        server.media = audiovisual_fixture()
        worker = threading.Thread(
            target=lambda: server.serve_forever(poll_interval=0.05))
        worker.start()
        try:
            url = (f"http://127.0.0.1:{server.server_port}/playback"
                   "?api_key=DO_NOT_LOG")
            result = check_stalled_output(api, url, args.legacy)
            if not args.legacy:
                result.update(check_normal_output(api, url))
            result["librarySha256"] = digest
            assert hashlib.sha256(library.read_bytes()).hexdigest() == digest
            print(json.dumps(result, indent=2))
        finally:
            server.release_stall.set()
            server.release_cancel.set()
            server.shutdown()
            server.server_close()
            worker.join(timeout=5)
            assert not worker.is_alive(), "fixture server did not stop"


if __name__ == "__main__":
    main()
