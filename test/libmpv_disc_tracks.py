#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Headless Blu-ray track-switch regression with an authored synthetic ISO.

Requires ffmpeg and the tsMuxeR CLI. No private media, network service, window,
or physical audio device is used. Each case owns a bounded child process.
"""

import argparse
from collections import deque
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

from libmpv_thumbnails import Mpv


TARGET = 8.0
READAHEAD = 24
SWITCH_LIMIT = 3.0
CASES = ("audio-on", "audio-off", "sub-on", "sub-off", "paused", "mkv", "mp4")


def require_tool(value):
    path = shutil.which(value)
    if path is None:
        raise FileNotFoundError(f"Required fixture authoring tool not found: {value}")
    return path


def run_tool(arguments):
    result = subprocess.run(
        arguments, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
    )
    if result.returncode:
        raise RuntimeError(
            f"{Path(arguments[0]).name} failed:\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}",
        )


def create_media(root, ffmpeg, tsmuxer):
    video = root / "video.264"
    audio1, audio2 = root / "first.ac3", root / "second.ac3"
    run_tool(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "color=c=black:s=1920x1080:r=24",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=440:sample_rate=48000",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=880:sample_rate=48000",
            "-map",
            "0:v",
            "-t",
            "60",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-pix_fmt",
            "yuv420p",
            "-profile:v",
            "high",
            "-level:v",
            "4.1",
            "-b:v",
            "8M",
            "-maxrate:v",
            "8M",
            "-bufsize:v",
            "8M",
            "-x264-params",
            "bluray-compat=1:nal-hrd=cbr:filler=1:keyint=24:min-keyint=24:bframes=0",
            str(video),
            "-map",
            "1:a",
            "-t",
            "60",
            "-c:a",
            "ac3",
            "-b:a",
            "192k",
            str(audio1),
            "-map",
            "2:a",
            "-t",
            "60",
            "-c:a",
            "ac3",
            "-b:a",
            "192k",
            str(audio2),
        ],
    )
    subtitles = [root / "first.srt", root / "second.srt"]
    for index, subtitle in enumerate(subtitles):
        subtitle.write_text(
            f"1\n00:00:01,000 --> 00:00:19,000\nTrack {index + 1}: first cue\n\n"
            f"2\n00:00:21,000 --> 00:00:39,000\nTrack {index + 1}: second cue\n\n"
            f"3\n00:00:41,000 --> 00:00:59,000\nTrack {index + 1}: third cue\n",
            encoding="utf-8",
        )
    meta = root / "fixture.meta"
    tracks = [
        "MUXOPT --no-pcr-on-video-pid --new-audio-pes --vbr --vbv-len=500 --blu-ray",
        f'V_MPEG4/ISO/AVC, "{video}", fps=24, insertSEI, contSPS',
        f'A_AC3, "{audio1}", lang=eng',
        f'A_AC3, "{audio2}", lang=fra',
    ]
    for index, subtitle in enumerate(subtitles):
        language = "eng" if index == 0 else "fra"
        tracks.append(
            f'S_TEXT/UTF8, "{subtitle}", font-name="Arial", font-size=36, '
            f"font-color=0xffffffff, bottom-offset=24, video-width=1920, "
            f"video-height=1080, fps=24, lang={language}",
        )
    meta.write_text("\n".join(tracks) + "\n", encoding="utf-8")
    iso = root / "fixture.iso"
    run_tool([tsmuxer, str(meta), str(iso)])
    if not iso.is_file():
        raise AssertionError("tsMuxeR did not produce the requested Blu-ray ISO")
    mkv = root / "control.mkv"
    run_tool(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-fflags",
            "+genpts",
            "-r",
            "24",
            "-i",
            str(video),
            "-i",
            str(audio1),
            "-i",
            str(audio2),
            "-i",
            str(subtitles[0]),
            "-i",
            str(subtitles[1]),
            "-map",
            "0",
            "-map",
            "1",
            "-map",
            "2",
            "-map",
            "3",
            "-map",
            "4",
            "-c",
            "copy",
            str(mkv),
        ],
    )
    run_tool(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-i",
            str(mkv),
            "-map",
            "0",
            "-c",
            "copy",
            "-c:s",
            "mov_text",
            str(root / "control.mp4"),
        ],
    )


class Probe:
    def __init__(self, library):
        self.mpv = Mpv(library)
        assert self.mpv.api.mpv_request_log_messages(self.mpv.handle, b"debug") >= 0
        self.logs = deque(maxlen=60)
        self.audio_starts = 0
        self.restarts = 0
        self.refreshes = 0
        self.max_audio_gap = 0.0

    def event(self):
        event = self.mpv.event(0.01)
        if event["id"] == 21:
            self.restarts += 1
        text = event.get("log", "").strip()
        if text:
            self.logs.append(text)
            self.audio_starts += "starting audio playback" in text
            self.refreshes += "Refreshing disc track selection" in text
            gap = re.search(r"delaying audio start .*diff=([-0-9.]+)", text)
            if gap:
                self.max_audio_gap = max(self.max_audio_gap, float(gap[1]))
        return event

    def wait(self, predicate, description, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.event()
            if predicate():
                return
            if self.mpv.property("eof-reached"):
                raise AssertionError(f"Unexpected EOF while {description}")
        raise AssertionError(
            f"Timed out {description}; audioGap={self.max_audio_gap:.3f}s; "
            f"recent native messages={list(self.logs)}",
        )

    def drain(self):
        deadline = time.monotonic() + 0.25
        while time.monotonic() < deadline and self.event()["id"]:
            continue

    def set(self, name, value):
        self.mpv.command("set", name, value)

    def load(self, source, preserve):
        for name, value in {
            "aid": "1",
            "sid": "no",
            "pause": "yes",
            "cache": "yes",
            "cache-secs": str(READAHEAD),
            "demuxer-readahead-secs": str(READAHEAD),
            "demuxer-max-bytes": "64MiB",
            "demuxer-cache-preserve-on-track-switch": preserve,
            "hr-seek": "yes",
        }.items():
            self.set(name, value)
        self.mpv.command("loadfile", str(source))
        self.wait(lambda: self.restarts > 0, "opening the fixture", 15)
        before = self.restarts
        self.mpv.command("seek", TARGET, "absolute+exact")
        self.wait(
            lambda: self.restarts > before and not self.mpv.property("seeking"),
            "seeking to the dialogue",
        )
        self.wait_for_readahead()
        self.drain()

    def wait_for_readahead(self):
        self.wait(
            lambda: (self.mpv.property("demuxer-cache-duration") or 0) >= 18,
            "establishing at least 18 seconds of forward packets",
        )

    def has_current_subtitle(self):
        start = self.mpv.property("sub-start")
        end = self.mpv.property("sub-end")
        position = self.mpv.property("time-pos")
        return (
            isinstance(start, (int, float))
            and isinstance(end, (int, float))
            and isinstance(position, (int, float))
            and start <= position < end
            and start < TARGET
            and end > TARGET
        )

    def assert_position(self, before, elapsed=0):
        after = self.mpv.property("time-pos")
        assert before - 0.2 <= after <= before + elapsed + 0.2, {
            "before": before,
            "after": after,
            "elapsed": elapsed,
            "recentNativeMessages": list(self.logs),
        }

    def close(self):
        self.mpv.close()


def run_case(library, root, case):
    probe = Probe(library)
    try:
        ordinary = case in ("mkv", "mp4")
        source = root / (f"control.{case}" if ordinary else "fixture.iso")
        preserve = "no" if case.endswith("-off") else "yes"
        probe.load(source, preserve)
        tracks = probe.mpv.property("track-list")
        assert sum(track["type"] == "audio" for track in tracks) == 2, tracks
        if not ordinary:
            assert probe.mpv.property("partially-seekable") is True
            assert sum(track.get("codec") == "hdmv_pgs_subtitle" for track in tracks) == 2, tracks
        refreshes = probe.refreshes
        position = probe.mpv.property("time-pos")
        started = time.monotonic()
        if case.startswith("sub-") or case == "paused":
            probe.set("sid", "1")
            probe.wait(
                probe.has_current_subtitle,
                "recovering the already active first PGS cue",
                SWITCH_LIMIT,
            )
            assert probe.mpv.property("pause") is True
            probe.assert_position(position)
            probe.wait_for_readahead()
            probe.set("sid", "2")
            probe.wait(
                probe.has_current_subtitle,
                "recovering the already active second PGS cue",
                SWITCH_LIMIT,
            )
            assert probe.mpv.property("sid") == 2
            assert probe.mpv.property("pause") is True
            probe.assert_position(position)
            if case == "paused":
                before = probe.audio_starts
                probe.set("aid", "2")
                probe.wait(
                    lambda: probe.audio_starts > before and probe.has_current_subtitle(),
                    "preserving pause and the PGS cue on audio switch",
                    SWITCH_LIMIT,
                )
                assert probe.mpv.property("pause") is True
                probe.assert_position(position)
        else:
            probe.set("pause", "no")
            probe.wait(
                lambda: (probe.mpv.property("time-pos") or 0) > position + 0.1,
                "starting the original audio",
            )
            probe.drain()
            initial_starts = probe.audio_starts
            probe.max_audio_gap = 0.0
            position = probe.mpv.property("time-pos")
            started = time.monotonic()
            probe.set("aid", "2")
            probe.wait(
                lambda: probe.audio_starts > initial_starts,
                "starting the newly selected audio",
                SWITCH_LIMIT,
            )
            assert probe.max_audio_gap <= 0.5, probe.max_audio_gap
            assert probe.mpv.property("aid") == 2
            elapsed = time.monotonic() - started
            probe.assert_position(position, elapsed)
            if ordinary:
                assert probe.refreshes == refreshes
        elapsed = time.monotonic() - started
        if not ordinary:
            assert probe.refreshes > refreshes
        print(
            json.dumps(
                {
                    "case": case,
                    "switchSeconds": round(elapsed, 3),
                    "maximumAudioGapSeconds": round(probe.max_audio_gap, 3),
                    "discRefreshes": probe.refreshes - refreshes,
                    "passed": True,
                },
            ),
        )
    finally:
        probe.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--tsmuxer", default="tsMuxeR")
    parser.add_argument("--case", choices=CASES, action="append")
    parser.add_argument("--fixture-root", type=Path)
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="Author a new fixture root for baseline comparisons",
    )
    parser.add_argument("--probe", choices=CASES, help=argparse.SUPPRESS)
    args = parser.parse_args()
    library = args.library.resolve(strict=True)
    if args.probe:
        if args.fixture_root is None:
            parser.error("--probe requires --fixture-root")
        run_case(str(library), args.fixture_root.resolve(strict=True), args.probe)
        return
    ffmpeg = require_tool(args.ffmpeg)
    tsmuxer = require_tool(args.tsmuxer)
    if args.prepare_only:
        if args.fixture_root is None:
            parser.error("--prepare-only requires a new --fixture-root")
        args.fixture_root.mkdir(parents=True, exist_ok=False)
        create_media(args.fixture_root.resolve(), ffmpeg, tsmuxer)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-disc-tracks-") as temporary:
        root = args.fixture_root or Path(temporary)
        if args.fixture_root is None:
            create_media(root, ffmpeg, tsmuxer)
        for case in args.case or CASES:
            subprocess.run(
                [
                    sys.executable,
                    "-B",
                    str(Path(__file__).resolve()),
                    str(library),
                    "--fixture-root",
                    str(root),
                    "--probe",
                    case,
                ],
                check=True,
                timeout=45,
            )


if __name__ == "__main__":
    main()
