#!/usr/bin/env python3
"""Decode caller-supplied AVS fixtures with an exact libmpv build, without a window."""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


class Event(ctypes.Structure):
    _fields_ = [("id", ctypes.c_int), ("error", ctypes.c_int),
                ("userdata", ctypes.c_uint64), ("data", ctypes.c_void_p)]


class EndFile(ctypes.Structure):
    _fields_ = [("reason", ctypes.c_int), ("error", ctypes.c_int)]


class LogMessage(ctypes.Structure):
    _fields_ = [("prefix", ctypes.c_char_p), ("level", ctypes.c_char_p),
                ("text", ctypes.c_char_p), ("log_level", ctypes.c_int)]


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def decode(library, fixture, decoder, width, height, pixel_format):
    directory = os.add_dll_directory(str(Path(library).parent))
    try:
        mpv = ctypes.CDLL(library)
        mpv.mpv_create.restype = ctypes.c_void_p
        mpv.mpv_set_option_string.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
        mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
        mpv.mpv_request_log_messages.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_command.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
        mpv.mpv_get_property_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_get_property_string.restype = ctypes.c_void_p
        mpv.mpv_free.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        mpv.mpv_wait_event.argtypes = [ctypes.c_void_p, ctypes.c_double]
        mpv.mpv_wait_event.restype = ctypes.POINTER(Event)
        client = mpv.mpv_create()
        if not client:
            raise RuntimeError("mpv_create failed")
        try:
            options = {
                "config": "no", "load-scripts": "no", "terminal": "no",
                "ytdl": "no", "vo": "null", "ao": "null", "aid": "no",
                "hwdec": "no", "frames": "3", "untimed": "yes",
                "keep-open": "yes", "vd": decoder + ",-",
            }
            if Path(fixture).suffix.lower() == ".avs3":
                options["demuxer-lavf-format"] = "avs3"
            for key, value in options.items():
                if mpv.mpv_set_option_string(client, key.encode(), value.encode()) < 0:
                    raise RuntimeError("Native option rejected: " + key)
            if mpv.mpv_request_log_messages(client, b"warn") < 0:
                raise RuntimeError("Native log subscription failed")
            if mpv.mpv_initialize(client) < 0:
                raise RuntimeError("mpv_initialize failed")

            def read(name):
                pointer = mpv.mpv_get_property_string(client, name.encode())
                if not pointer:
                    return None
                try:
                    return ctypes.string_at(pointer).decode("utf-8")
                finally:
                    mpv.mpv_free(pointer)

            decoders = json.loads(read("decoder-list") or "null")
            if not isinstance(decoders, list) or not any(
                    entry.get("driver") == decoder for entry in decoders):
                raise RuntimeError("Required decoder is not registered")
            command = (ctypes.c_char_p * 3)(b"loadfile", fixture.encode(), None)
            if mpv.mpv_command(client, command) < 0:
                raise RuntimeError("loadfile rejected")
            deadline = time.monotonic() + 30
            observed = None
            restarted = False
            logs = []
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(client, 0.1).contents
                if event.id == 2:
                    message = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
                    logs.append(message.text.decode("utf-8", errors="replace")[:500])
                    logs = logs[-8:]
                elif event.id == 7:
                    end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
                    if end.reason == 4 or end.error < 0:
                        raise RuntimeError(
                            f"Native media failed: reason={end.reason}, error={end.error}; "
                            + "".join(logs))
                elif event.id in (8, 17, 21):
                    raw = read("video-dec-params")
                    if raw:
                        observed = json.loads(raw)
                    restarted |= event.id == 21
                if restarted and observed:
                    if (observed.get("w"), observed.get("h"), observed.get("pixelformat")) != (
                            width, height, pixel_format):
                        raise RuntimeError("Decoded frame format mismatch: " + json.dumps(observed))
                    print(json.dumps({
                        "status": "passed", "decoder": decoder,
                        "librarySha256": sha256(library),
                        "fixtureSha256": sha256(fixture),
                        "width": width, "height": height, "pixelFormat": pixel_format,
                        "observedDecodedFrame": True,
                        "colorPrimaries": observed.get("primaries"),
                        "colorTransfer": observed.get("gamma"),
                        "colorMatrix": observed.get("colormatrix"),
                    }))
                    return
            raise TimeoutError("No decoded video frame: " + "".join(logs))
        finally:
            mpv.mpv_terminate_destroy(client)
    finally:
        directory.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=lambda value: str(Path(value).resolve(strict=True)))
    parser.add_argument("fixture", type=lambda value: str(Path(value).resolve(strict=True)))
    parser.add_argument("--decoder", choices=("libdavs2", "libuavs3d"), required=True)
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--pixel-format", required=True)
    parser.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.child:
        decode(args.library, args.fixture, args.decoder, args.width, args.height, args.pixel_format)
        return
    result = subprocess.run(
        [sys.executable, __file__, *sys.argv[1:], "--child"],
        capture_output=True, text=True, timeout=45)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr)[-6000:])
    print(result.stdout, end="")


if __name__ == "__main__":
    main()
