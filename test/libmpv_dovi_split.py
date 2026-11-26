#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Check P7 startup before EOF with absent, present, and late interleaved EL.

Usage: python test/libmpv_dovi_split.py path/to/libmpv-2.dll
Requires ffmpeg (libx265) and ffprobe; all pictures are synthetic.
"""

import json
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time

from libmpv_network_diagnostics import FixtureServer, element, integer
from libmpv_thumbnails import Mpv


def hevc_picture():
    media = subprocess.check_output([
        "ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=96x64",
        "-frames:v", "1", "-c:v", "libx265", "-x265-params",
        "log-level=error:pools=none:frame-threads=1", "-f", "matroska", "-",
    ], timeout=20)
    probe = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_data", "-show_entries",
        "packet=data:stream=extradata", "-of", "json", "-",
    ], input=media, timeout=10))

    def unpack(dump):
        return bytes.fromhex(" ".join(line.split(": ", 1)[1].split("  ")[0]
                                      for line in dump.splitlines() if line))

    return (unpack(probe["streams"][0]["extradata"]),
            unpack(probe["packets"][0]["data"]))


def fixture(config, bl, first_el):
    header = element("1a45dfa3", element("4282", b"matroska") + integer("4285", 2))
    info = element("1549a966", integer("2ad7b1", 1000000, 3)
                   + element("4489", struct.pack(">d", 12000)))
    mapping = element("41e4", integer("41f0", 1) + element("41e7", b"dvcC")
                      + element("41ed", bytes.fromhex("01000e3760") + bytes(19)))
    track = element("ae", integer("d7", 1) + integer("73c5", 1) + integer("83", 1)
                    + element("86", b"V_MPEGH/ISO/HEVC") + element("63a2", config)
                    + integer("23e383", 40000000, 4) + mapping
                    + element("e0", integer("b0", 96) + integer("ba", 64)))
    # A second independently decodable picture, wrapped as type-63 EL NALs.
    assert config[21] & 3 == 3
    el = bytearray()
    pos = 0
    while pos < len(bl):
        size = int.from_bytes(bl[pos:pos + 4], "big")
        el += struct.pack(">I", size + 2) + b"\x7e\x01" + bl[pos + 4:pos + 4 + size]
        pos += size + 4
    assert pos == len(bl)
    clusters = b"".join(element("1f43b675", integer("e7", i * 40, 2)
                                + element("a3", b"\x81\0\0\x80" + bl
                                          + (el if i >= first_el else b"")))
                        for i in range(300))
    padding = element("ec", bytes(2**20))
    media = header + element("18538067", info + element("1654ae6b", track)
                             + clusters + padding)
    return media, len(media) - len(padding)


def exercise(library, demuxer, media):
    with FixtureServer() as server:
        server.media, server.tail_offset = media
        worker = threading.Thread(target=server.serve_forever)
        worker.start()
        try:
            player = Mpv(library)
            try:
                for name, value in (("demuxer", demuxer), ("pause", "no"),
                                    ("cache-pause", "no"), ("vd-lavc-threads", "1")):
                    player.command("set", name, value)
                player.command("loadfile", f"http://127.0.0.1:{server.server_port}/stall")
                decoded = 0
                split = restarted = False
                deadline = time.monotonic() + 6
                while time.monotonic() < deadline:
                    event = player.event()
                    text = event.get("log", "")
                    split |= "virtual EL stream" in text
                    decoded += "Decoder format:" in text
                    restarted |= event["id"] == 21
                    if restarted and (player.property("time-pos") or 0) >= 0.6:
                        break
                assert split and restarted and player.property("vo-configured"), (
                    demuxer, split, restarted, decoded)
                assert (player.property("time-pos") or 0) >= 0.6
                return decoded
            finally:
                server.release_stall.set()
                player.close()
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    library = str(Path(sys.argv[1]).resolve(strict=True))
    config, bl = hevc_picture()
    for demuxer, first_el in (("mkv", 300), ("lavf", 300), ("mkv", 0), ("mkv", 5)):
        decoded = exercise(library, demuxer, fixture(config, bl, first_el))
        assert decoded == (1 if first_el == 300 else 2), (demuxer, first_el, decoded)
        print(f"PASS {demuxer}: first EL={first_el}, active decoders={decoded}")
