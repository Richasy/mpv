#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Exercise thumbnail cancellation while local playback seeks and closes.

Unlike the steady-state image test, this cancels at packet-read boundaries
while audio/video continue playing. Every admitted native request must reply;
the final stop and native destruction must actually finish.
"""

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
import time

from libmpv_thumbnails import Mpv, command_args


def wait_reply(client, reply_id, seconds=2):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        event = client.event(0.01)
        if event["id"] == 5 and event["reply"] == reply_id:
            return event
    raise TimeoutError(f"Thumbnail reply {reply_id} did not complete after cancellation")


def exercise(library, fixture):
    client = Mpv(library)
    settled = 0
    longest_cancel = 0
    try:
        client.command("set", "cache", "no")
        client.command("set", "aid", "auto")
        client.load(fixture, wait_for_cache=False)
        duration = client.property("duration")
        assert isinstance(duration, (int, float)) and math.isfinite(duration) and duration >= 2
        video = client.property("video-params") or {}
        measurements = []
        for fraction in (0.05, 0.25, 0.5, 0.75, 0.95):
            target = duration * fraction
            fast = client.thumbnail("local", target, "keyframes")
            assert fast["cached"] and fast["decoded-frames"] == 1, fast
            assert fast["milliseconds"] < 1000, fast
            exact = client.thumbnail("local", target, "exact")
            assert exact["cached"] and abs(exact["pts"] - target) < 0.1, exact
            measurements.append({
                "targetSeconds": target,
                "previewMs": fast["milliseconds"],
                "exactMs": exact["milliseconds"],
                "previewPts": fast["pts"],
                "exactPts": exact["pts"],
            })

        client.command("seek", duration * 0.25, "absolute+exact")
        client.command("set", "pause", "no")
        print("active playback started", file=sys.stderr, flush=True)
        for index in range(96):
            target = duration * (0.05 + 0.9 * ((index * 7) % 10) / 9)
            if index % 8 == 0:
                client.command("seek", duration * 0.75, "absolute+exact")
                client.command("seek", duration * 0.25, "absolute+exact")

            reply_id = index + 100
            assert client.api.mpv_command_async(
                client.handle, reply_id,
                command_args(["thumbnail-raw", target, 320, "local", "keyframes"])) >= 0
            deadline = time.monotonic() + 2
            completed = None
            while time.monotonic() < deadline:
                event = client.event(0.01)
                if event["id"] == 5 and event["reply"] == reply_id:
                    completed = event
                    break
                if "thumbnail: decoding keyframes" in event.get("log", ""):
                    break
            else:
                raise TimeoutError(f"Thumbnail {reply_id} did not start")
            time.sleep((index % 5) * 0.0002)
            started = time.monotonic()
            client.api.mpv_abort_async_command(client.handle, reply_id)
            if completed is None:
                wait_reply(client, reply_id)
            longest_cancel = max(longest_cancel, time.monotonic() - started)
            settled += 1
            if index % 8 == 0:
                print(f"settled {settled} requests", file=sys.stderr, flush=True)

        preview = client.thumbnail("local", duration * 0.45, "keyframes")
        assert preview["cached"] and preview["decoded-frames"] == 1, preview
        assert preview["milliseconds"] < 1000, preview
        print("settled preview completed", file=sys.stderr, flush=True)

        assert client.api.mpv_command_async(
            client.handle, 300,
            command_args(["thumbnail-raw", duration * 0.8, 320, "local", "exact"])) >= 0
        time.sleep(0.002)
        started = time.monotonic()
        client.command("stop")
        wait_reply(client, 300)
        assert client.property("idle-active") is True
        stop_ms = (time.monotonic() - started) * 1000
        print("native stop completed", file=sys.stderr, flush=True)
    finally:
        print("native destruction started", file=sys.stderr, flush=True)
        started = time.monotonic()
        client.close()
        destroy_ms = (time.monotonic() - started) * 1000
        print("native destruction completed", file=sys.stderr, flush=True)

    assert destroy_ms < 2000, destroy_ms
    print(json.dumps({
        "durationSeconds": duration,
        "videoWidth": video.get("w"),
        "videoHeight": video.get("h"),
        "positions": measurements,
        "settledRequests": settled,
        "longestCancelMs": longest_cancel * 1000,
        "settledPreviewMs": preview["milliseconds"],
        "stopMs": stop_ms,
        "destroyMs": destroy_ms,
    }))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=lambda value: str(Path(value).resolve(strict=True)))
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--media", type=lambda value: str(Path(value).resolve(strict=True)),
                        help="Read an existing local video instead of generating test media.")
    parser.add_argument("--fixture", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.fixture:
        exercise(args.library, args.fixture)
        return
    if args.media:
        run_child(args.library, args.media, timeout=90)
        return
    with tempfile.TemporaryDirectory(prefix="mpv-thumbnail-lifetime-") as directory:
        fixture = Path(directory) / "interleaved.mp4"
        subprocess.run([
            args.ffmpeg, "-hide_banner", "-loglevel", "error",
            "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=24000/1001",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-t", "12", "-c:v", "libx264", "-preset", "veryfast", "-threads", "2",
            "-profile:v", "high", "-g", "144", "-keyint_min", "144",
            "-sc_threshold", "0", "-bf", "3", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-movflags", "+faststart", str(fixture),
        ], check=True, timeout=120)
        run_child(args.library, fixture, timeout=30)


def run_child(library, fixture, timeout):
    try:
        result = subprocess.run([
            sys.executable, "-B", __file__, library, "--fixture", str(fixture),
        ], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        progress = error.stderr or b""
        if isinstance(progress, bytes):
            progress = progress.decode("utf-8", errors="replace")
        raise TimeoutError(f"Native thumbnail lifetime did not finish:\n{progress}") from error
    if result.returncode:
        raise RuntimeError(result.stderr[-5000:] + result.stdout[-1000:])
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
