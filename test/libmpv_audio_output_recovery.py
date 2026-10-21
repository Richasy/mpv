#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Exercise lost-output recovery without physical audio or device mutation.

A missing PCM output directory reproduces AO initialization failure. The public
ao-reload command enters the same recovery path as an audio hotplug notification.
Each case runs in a bounded child with its own native client and temporary files.
"""

import argparse
import ctypes as c
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from libmpv_thumbnails import Mpv


class EndFile(c.Structure):
    _fields_ = [("reason", c.c_int), ("error", c.c_int)]


def assert_clean_eof(player):
    player.command("set", "keep-open", "no")
    player.command("seek", "19", "absolute+exact")
    player.command("set", "speed", "100")
    player.command("set", "pause", "no")
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        event = player.api.mpv_wait_event(player.handle, 0.05).contents
        if event.id == 7:
            ended = c.cast(event.data, c.POINTER(EndFile)).contents
            assert ended.reason == 0 and ended.error == 0, (ended.reason, ended.error)
            return
    raise AssertionError("Playback did not finish with a clean EOF")


def wait_for(player, predicate):
    deadline = time.monotonic() + 10
    logs = []
    while time.monotonic() < deadline:
        if predicate():
            return
        event = player.event(0.05)
        if "log" in event:
            logs.append(event["log"])
    raise AssertionError("Audio output state did not settle:\n" + "".join(logs[-30:]))


def run_case(library, media, root, case):
    output = root / "unavailable" / "output.wav"
    player = Mpv(library)
    try:
        player.command("set", "ao-pcm-file", str(output))
        player.command("set", "ao", "pcm")
        player.command("set", "aid", "auto")
        player.load(str(media), wait_for_cache=False)
        wait_for(player, lambda: player.property("current-ao") is None
                 and not any(t.get("selected") for t in player.property("track-list")
                             if t["type"] == "audio"))
        assert player.property("pause")
        player.command("seek", "4", "absolute+exact")
        wait_for(player, lambda: (player.property("time-pos") or 0) >= 3.9)
        position = player.property("time-pos")

        if case == "retry":
            player.command("ao-reload")
            retry_started = False
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                log = player.event(0.05).get("log", "")
                retry_started |= "Retrying audio output for track" in log
                if retry_started and "Could not open/initialize audio device" in log:
                    break
            else:
                raise AssertionError("The failed output was not retried")
            assert player.property("current-ao") is None
        elif case == "off":
            player.command("set", "aid", "no")
        elif case == "switch":
            player.command("set", "aid", "2")
            wait_for(player, lambda: player.property("aid") in (None, "auto", "no", False))
        elif case == "stop":
            player.command("stop")
            wait_for(player, lambda: player.property("idle-active"))

        output.parent.mkdir()
        if case == "manual":
            player.command("set", "aid", "1")
        else:
            player.command("ao-reload")
        if case in ("off", "stop"):
            # A synchronous property read is ordered after the reload command.
            assert player.property("current-ao") is None
            assert not any(t.get("selected")
                           for t in (player.property("track-list") or [])
                           if t["type"] == "audio")
        else:
            wait_for(player, lambda: player.property("current-ao") == "pcm")
            expected = 2 if case == "switch" else 1
            tracks = player.property("track-list")
            assert [t["id"] for t in tracks
                    if t["type"] == "audio" and t.get("selected")] == [expected]
            assert player.property("pause")
            assert abs(player.property("time-pos") - position) < 0.05
        if case != "stop":
            assert_clean_eof(player)
    finally:
        player.close()
    print(f"PASS audio-output-recovery {case}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("library")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--case", choices=("retry", "manual", "off", "switch", "stop"))
    parser.add_argument("--media", type=Path)
    parser.add_argument("--root", type=Path)
    args = parser.parse_args()
    if args.case:
        run_case(args.library, args.media, args.root, args.case)
        return
    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg:
        raise FileNotFoundError("ffmpeg is required to author the audio recovery fixture")
    with tempfile.TemporaryDirectory(prefix="mpv-audio-recovery-") as directory:
        root = Path(directory)
        media = root / "fixture.mkv"
        subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
             "-f", "lavfi", "-i", "color=size=64x64:rate=5",
             "-f", "lavfi", "-i", "sine=frequency=440",
             "-f", "lavfi", "-i", "sine=frequency=880",
             "-map", "0:v", "-map", "1:a", "-map", "2:a",
             "-t", "20", "-c:v", "ffv1", "-c:a", "pcm_s16le", str(media)],
            check=True, timeout=30, capture_output=True,
        )
        for case in ("retry", "manual", "off", "switch", "stop"):
            case_root = root / case
            case_root.mkdir()
            subprocess.run(
                [sys.executable, "-B", str(Path(__file__).resolve()),
                 str(Path(args.library).resolve()), "--case", case,
                 "--media", str(media), "--root", str(case_root)],
                check=True, timeout=35,
            )


if __name__ == "__main__":
    main()
