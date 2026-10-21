#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Compare real-source seek recovery without a window or retained private URLs.

The request URL and optional HTTP headers arrive only through stdin. Each
invocation loads exactly one libmpv build; callers run comparison cases in
separate processes to keep native dependencies isolated.
"""

import argparse
import contextlib
import ctypes
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time

from libmpv_network_cursors import (
    EVENT_LOG_MESSAGE,
    EVENT_PLAYBACK_RESTART,
    EndFile,
    LogMessage,
    get_property,
    load_libmpv,
)


def command(mpv, client, name, *arguments):
    words = [name, *arguments]
    argv = (ctypes.c_char_p * (len(words) + 1))(
        *[word.encode() for word in words], None)
    result = mpv.mpv_command(client, argv)
    if result < 0:
        raise RuntimeError(f"Native command {name} failed ({result})")


def number(mpv, client, name):
    value = get_property(mpv, client, name)
    if value is None:
        return None
    try:
        parsed = float(value)
    except ValueError:
        return None
    return parsed if math.isfinite(parsed) else None


def sample(mpv, client, started):
    return {
        "elapsed": round(time.monotonic() - started, 3),
        "position": number(mpv, client, "time-pos"),
        "seeking": get_property(mpv, client, "seeking") == "yes",
        "paused": get_property(mpv, client, "pause") == "yes",
        "buffering": get_property(mpv, client, "paused-for-cache") == "yes",
        "cacheSeconds": number(mpv, client, "demuxer-cache-duration"),
        "cacheBytesPerSecond": number(mpv, client, "cache-speed"),
    }


def observe(mpv, client, seconds, target=None, startup=False):
    started = time.monotonic()
    next_sample = started
    samples = []
    restart_seconds = None
    buffering_entries = 0
    transport_pending = 0
    lru_enabled = False
    file_loaded = not startup
    terminal_error = False
    terminal_code = None
    failure_kinds = {}
    transport_events = []
    confirmed_seconds = None
    while time.monotonic() - started < seconds:
        event = mpv.mpv_wait_event(client, 0.05).contents
        if event.event_id == 8:
            file_loaded = True
        elif event.event_id == 7:
            terminal_error = True
            terminal_code = ctypes.cast(
                event.data, ctypes.POINTER(EndFile)).contents.error
            break
        elif event.event_id == EVENT_PLAYBACK_RESTART:
            if restart_seconds is None:
                restart_seconds = round(time.monotonic() - started, 3)
        elif event.event_id == EVENT_LOG_MESSAGE:
            message = ctypes.cast(event.data,
                                  ctypes.POINTER(LogMessage)).contents
            text = message.text.decode("utf-8", errors="replace")
            # Native message bodies can contain credentials. Retain counters,
            # not the text, even when the input looks like a harmless URL.
            buffering_entries += "Enter buffering" in text
            transport_pending += bool(re.search(
                r"(?:network_io|curl_request) pending ", text))
            lru_enabled |= "byte-range LRU cache enabled:" in text
            if text.startswith(("network_io ", "curl_request ")):
                numeric = {
                    key: float(value) if "." in value else int(value)
                    for key, value in re.findall(
                        r"\b([a-z_]+)=(-?\d+(?:\.\d+)?)\b", text)
                }
                transport_events.append({
                    "elapsed": round(time.monotonic() - started, 3),
                    "kind": text.split()[0],
                    "phase": text.split()[1],
                    **numeric,
                })
                transport_events = transport_events[-500:]
            match = re.search(
                r"lru_cache: connection (\d+) seek (-?\d+) -> (-?\d+)", text)
            if match:
                transport_events.append({
                    "elapsed": round(time.monotonic() - started, 3),
                    "kind": "lru-seek",
                    "connection": int(match[1]),
                    "from": int(match[2]),
                    "to": int(match[3]),
                })
            for kind, pattern in {
                "certificate": r"certificate.*(?:fail|error)|TLS.*verify",
                "http-error": r"HTTP(?:/\d(?:\.\d)?\s+|\s+error\s+)(?:401|403|404|416|429|500|502|503)\b",
                "timeout": r"timed out|timeout (?:expired|reached)|deadline expired",
                "range-mismatch": r"unexpected offset|range.*mismatch",
            }.items():
                if re.search(pattern, text, re.IGNORECASE):
                    failure_kinds[kind] = failure_kinds.get(kind, 0) + 1
        now = time.monotonic()
        if now < next_sample:
            continue
        next_sample = now + 0.25
        current = sample(mpv, client, started)
        samples.append(current)
        position = current["position"]
        settled = (file_loaded and position is not None
                   and not current["seeking"] and not current["paused"]
                   and not current["buffering"])
        if settled and confirmed_seconds is None:
            if startup or abs(position - target) <= 1:
                confirmed_seconds = current["elapsed"]
        if startup and settled:
            break
    return {
        "confirmedSeconds": confirmed_seconds,
        "restartSeconds": restart_seconds,
        "bufferingEntries": buffering_entries,
        "transportPending": transport_pending,
        "lruEnabledObserved": lru_enabled,
        "terminalEvent": terminal_error,
        "terminalCode": terminal_code,
        "failureKinds": failure_kinds,
        "samples": samples,
        "transportEvents": transport_events,
    }


def run(args, request):
    if not isinstance(request.get("url"), str) or not request["url"].startswith(
            ("https://", "http://")):
        raise ValueError("stdin must provide one HTTP resource")
    library = str(Path(args.library).resolve(strict=True))
    report = {
        "case": args.label,
        "librarySha256": hashlib.sha256(Path(library).read_bytes()).hexdigest(),
        "lru": args.lru,
        "backend": args.backend,
        "addressFamily": args.address_family,
        "stages": [],
    }
    with contextlib.ExitStack() as stack:
        mpv = load_libmpv(stack, library)
        client = mpv.mpv_create()
        if not client:
            raise RuntimeError("Native client creation failed")
        stack.callback(mpv.mpv_terminate_destroy, client)
        options = {
            "config": "no",
            "load-scripts": "no",
            "terminal": "no",
            "ytdl": "no",
            "vo": "null",
            "ao": "null",
            "hwdec": "no",
            "cache": "yes",
            "demuxer-max-bytes": "300MiB",
            "demuxer-readahead-secs": "300",
            "stream-buffer-size": "512KiB",
            "video-sync": "audio",
            "hr-seek": "default",
            "network-timeout": "15",
            "http-proxy": "",
            "tls-verify": "no",
            "user-agent": request.get("userAgent", "libmpv"),
            "stream-lavf-o": (
                "reconnect=1,reconnect_on_network_error=1,"
                "reconnect_on_http_error=429,respect_retry_after=1,"
                "reconnect_delay_max=30"),
        }
        if request.get("headers"):
            options["http-header-fields"] = ",".join(request["headers"])
        if args.lru == "disabled":
            options["stream-lru-cache"] = "0"
        if args.address_family != "auto":
            options["stream-lavf-o"] += ",prefer_family=" + args.address_family
            options["curl-prefer-family"] = args.address_family
        for name, value in options.items():
            result = mpv.mpv_set_option_string(
                client, name.encode(), value.encode())
            if result < 0:
                raise RuntimeError(f"Native option {name} rejected ({result})")
        backend = b"yes" if args.backend == "curl" else b"no"
        result = mpv.mpv_set_option_string(client, b"curl-enabled", backend)
        if result < 0 and not (args.backend == "lavf" and result == -5):
            raise RuntimeError("Could not select the shared FFmpeg backend")
        if mpv.mpv_request_log_messages(client, b"v") < 0:
            raise RuntimeError("Native diagnostics unavailable")
        if mpv.mpv_initialize(client) < 0:
            raise RuntimeError("Native initialization failed")
        command(mpv, client, "loadfile", request["url"])
        startup = observe(mpv, client, args.startup_seconds, startup=True)
        report["stages"].append({"name": "startup", **startup})
        report["fileSize"] = number(mpv, client, "file-size")
        if report["fileSize"] != request["expectedSize"]:
            report["outcome"] = "wrong-or-unloaded-resource"
            return report
        if startup["confirmedSeconds"] is None:
            report["outcome"] = "startup-not-confirmed"
            return report
        for target in args.targets:
            # Drain observations predating this command so an old restart
            # cannot be counted as the new seek's receipt.
            while mpv.mpv_wait_event(client, 0).contents.event_id:
                pass
            command(mpv, client, "seek", str(target), "absolute")
            measured = observe(mpv, client, args.observe_seconds, target)
            report["stages"].append({
                "name": "seek", "target": target, **measured,
            })
            if measured["terminalEvent"]:
                break
        command(mpv, client, "stop")
        report["outcome"] = "measured"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library")
    parser.add_argument("--label", required=True)
    parser.add_argument("--lru", choices=["native", "disabled"], default="native")
    parser.add_argument("--backend", choices=["lavf", "curl"], default="lavf")
    parser.add_argument("--address-family", choices=["auto", "ipv4", "ipv6"],
                        default="auto")
    parser.add_argument("--startup-seconds", type=float, default=60)
    parser.add_argument("--observe-seconds", type=float, default=30)
    parser.add_argument("--targets", type=float, nargs="+",
                        default=[4340.920937, 8048.174923])
    args = parser.parse_args()
    request = json.load(sys.stdin)
    report = run(args, request)
    print(json.dumps(report, allow_nan=False), flush=True)
    return 0 if report["outcome"] == "measured" else 1


if __name__ == "__main__":
    raise SystemExit(main())
