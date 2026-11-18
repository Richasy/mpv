#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""PGS redraw/clear/content-change regression. Requires ffmpeg, Pillow and local OCR assets."""

import argparse
from collections import Counter
from itertools import groupby
import json
from pathlib import Path
import struct
import subprocess
from tempfile import TemporaryDirectory
import threading
import time

from PIL import Image, ImageDraw, ImageFont

from libmpv_sub_translation import TranslationServer
from libmpv_thumbnails import Mpv


def object_data(text):
    image = Image.new("1", (540, 70))
    ImageDraw.Draw(image).text((4, 4), text, fill=1, font=ImageFont.truetype("arial.ttf", 44))
    rle = bytearray()
    for y in range(image.height):
        for color, run in groupby(image.getpixel((x, y)) for x in range(image.width)):
            count = sum(1 for _ in run)
            flag = 0x80 if color else 0
            rle.extend([0, flag | count] if count < 64 else
                       [0, flag | 0x40 | (count >> 8), count & 255])
            if color:
                rle.append(1)
        rle.extend([0, 0])
    return (len(rle) + 4).to_bytes(3, "big") + struct.pack(">HH", *image.size) + rle


def make_video(root):
    first, second = map(object_data, ["Source number 1", "Source number 2"])
    pgs = root / "redraw.sup"
    with pgs.open("wb") as stream:
        # Six contiguous draws cross the decoder's four-slot display queue.
        for i, (pts, obj) in enumerate([(n, first) for n in range(6)] +
                                       [(6, second), (9, None), (10, second), (13, None)]):
            def segment(kind, payload):
                stream.write(b"PG" + struct.pack(">IIBH", pts * 90000, 0, kind, len(payload)) + payload)

            placement = struct.pack(">HBBHH", 0, 0, 0, 50, 80) if obj else b""
            segment(0x16, struct.pack(">HHBHBBBB", 640, 360, 0x10, i, 0x80, 0, 0, bool(obj)) + placement)
            if obj:
                segment(0x17, struct.pack(">BBHHHH", 1, 0, 50, 80, 540, 70))
                segment(0x14, bytes([0, 0, 0, 16, 128, 128, 0, 1, 235, 128, 128, 255]))
                segment(0x15, struct.pack(">HBB", 0, i, 0xC0) + obj)
            segment(0x80, b"")
    video = root / "redraw.mkv"
    subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-f", "lavfi", "-i",
                    "color=s=320x180:r=24:d=14", "-i", str(pgs), "-map", "0:v", "-map", "1:s",
                    "-c:v", "libx264", "-preset", "ultrafast", "-c:s", "copy", str(video)],
                   check=True, timeout=30)
    return video


def check(mpv, server, video, args):
    mpv.command("set", "sid", "1")
    mpv.command("set", "sub-ocr-config", json.dumps({
        "model": str(args.model.resolve()), "dictionary": str(args.dictionary.resolve()),
        "runtime": str(args.libmpv.resolve().parent / "onnxruntime.dll"), "mode": "full"}))
    mpv.command("set", "sub-translate-config", json.dumps({
        "provider": "ai", "source_lang": "en", "target_lang": "zh",
        "ai": {"endpoint": f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
               "model": "fixture", "api_key": "fixture"},
        "limits": {"enabled": True, "horizon_sec": 3, "reuse_cache_capacity": 0}}))
    mpv.load(str(video), wait_for_cache=False)
    mpv.command("set", "sub-translate", "yes")
    mpv.command("set", "pause", "no")
    seen = set()
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        pts = mpv.property("time-pos")
        if pts is not None:
            text = mpv.property("secondary-sub-text") or ""
            if 1.5 < pts < 5.9:
                assert text == "translated:Source number 1", (pts, text)
                seen.add("redraw")
            elif 7 < pts < 8.9 or 11 < pts < 12.9:
                assert text == "translated:Source number 2", (pts, text)
                seen.add("change" if pts < 9 else "after-gap")
            elif 9.1 < pts < 9.9:
                assert not text, (pts, text)
                seen.add("gap")
            if pts >= 13.1:
                break
        time.sleep(0.02)
    assert seen == {"redraw", "change", "gap", "after-gap"}, seen
    status = json.loads(mpv.property("sub-translate-status"))
    assert status["ocr_recognized"] == 3, status
    with server.lock:
        assert Counter(server.requests) == {"Source number 1": 1, "Source number 2": 2}, server.requests


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("libmpv", "model", "dictionary"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    with TemporaryDirectory(prefix="mpv-bitmap-redraw-") as directory:
        video = make_video(Path(directory))
        mpv = Mpv(str(args.libmpv.resolve()))
        with TranslationServer() as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                check(mpv, server, video, args)
                print("PGS continuous redraws, changed pixels and explicit gap passed.")
            finally:
                mpv.close()
                server.shutdown()
                thread.join()
