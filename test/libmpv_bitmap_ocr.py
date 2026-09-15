#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opt-in real-model bitmap OCR integration; translation stays on loopback."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import tempfile
import threading
import time

from libmpv_sub_translation import TranslationServer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--libmpv", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--dictionary", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--video", required=True, type=Path,
                        help="Video with a PGS primary subtitle at time zero")
    parser.add_argument("--expected-text", required=True)
    parser.add_argument("--mode", choices=["auto", "source", "full"], default="full")
    parser.add_argument("--source-lang", default="auto")
    parser.add_argument("--expect-target-reuse", action="store_true")
    args = parser.parse_args()
    dll_directories = []
    if hasattr(os, "add_dll_directory"):
        for directory in [args.libmpv.parent, args.runtime.parent,
                          *map(Path, os.environ.get("PATH", "").split(os.pathsep))]:
            if directory.is_dir():
                dll_directories.append(os.add_dll_directory(str(directory.resolve())))
    lib = ctypes.CDLL(str(args.libmpv.resolve()))
    signatures = {
        "mpv_create": (ctypes.c_void_p, []),
        "mpv_initialize": (ctypes.c_int, [ctypes.c_void_p]),
        "mpv_set_option_string": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]),
        "mpv_set_property_string": (ctypes.c_int, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]),
        "mpv_get_property_string": (ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]),
        "mpv_command": (ctypes.c_int, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]),
        "mpv_wait_event": (ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_double]),
        "mpv_free": (None, [ctypes.c_void_p]),
        "mpv_terminate_destroy": (None, [ctypes.c_void_p]),
    }
    for name, (result, arguments) in signatures.items():
        function = getattr(lib, name)
        function.restype = result
        function.argtypes = arguments
    ctx = lib.mpv_create()
    if not ctx:
        raise RuntimeError("mpv_create failed")

    def set_property(name, value):
        code = lib.mpv_set_property_string(ctx, name.encode(), value.encode("utf-8"))
        if code < 0:
            raise RuntimeError(f"Setting {name} failed: {code}")

    def get_property(name):
        pointer = lib.mpv_get_property_string(ctx, name.encode())
        if not pointer:
            return None
        try:
            return ctypes.string_at(pointer).decode("utf-8")
        finally:
            lib.mpv_free(pointer)

    def command(*values):
        argv = (ctypes.c_char_p * (len(values) + 1))(
            *(value.encode("utf-8") for value in values), None)
        code = lib.mpv_command(ctx, argv)
        if code < 0:
            raise RuntimeError(f"Command {values[0]} failed: {code}")

    def wait_for(predicate, description, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            lib.mpv_wait_event(ctx, 0.05)
        raise RuntimeError(
            f"Timed out waiting for {description}: {get_property('sub-translate-status')}; "
            f"output={get_property('secondary-sub-text')!r}; requests={server.requests!r}")

    server = TranslationServer()
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for name, value in {
            "config": "no", "vo": "null", "ao": "null", "pause": "yes",
            "keep-open": "yes", "idle": "yes", "sid": "1", "terminal": "yes",
            "load-scripts": "no", "sub-auto": "no",
        }.items():
            if lib.mpv_set_option_string(ctx, name.encode(), value.encode()) < 0:
                raise RuntimeError(f"Option {name} rejected")
        if lib.mpv_initialize(ctx) < 0:
            raise RuntimeError("mpv_initialize failed")
        configuration = json.dumps({
            "model": str(args.model.resolve()),
            "dictionary": str(args.dictionary.resolve()),
            "runtime": str(args.runtime.resolve()), "mode": args.mode,
            "source_lang": args.source_lang,
        })
        set_property("sub-ocr-config", configuration)
        before = get_property("sub-ocr-config")
        if lib.mpv_set_property_string(ctx, b"sub-ocr-config", b'{"unknown":"value"}') >= 0:
            raise RuntimeError("Invalid OCR configuration was accepted")
        if get_property("sub-ocr-config") != before:
            raise RuntimeError("Rejected configuration changed the current model")
        set_property("sub-translate-config", json.dumps({
            "provider": "ai", "source_lang": "auto", "target_lang": "zh",
            "ai": {"endpoint": f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                   "model": "fixture", "api_key": "fixture-secret"},
        }))
        set_property("sub-translate", "yes")
        command("loadfile", str(args.video.resolve()))
        expected = args.expected_text if args.expect_target_reuse else "translated:" + args.expected_text
        wait_for(lambda: get_property("secondary-sub-text") == expected, "bitmap text output")
        status = json.loads(get_property("sub-translate-status"))
        if status["presentation"] != "bitmap-text" or status["ocr_recognized"] < 1:
            raise RuntimeError(f"Bitmap OCR status is inconsistent: {status}")
        if args.expect_target_reuse and status["ocr_selection"] != "bilingual-target-reused":
            raise RuntimeError(f"Target text was not supplied by bilingual selection: {status}")
        source_sid = get_property("sid")
        set_property("sub-translate", "no")
        wait_for(lambda: json.loads(get_property("sub-translate-status"))["state"] == "disabled",
                 "disabled translation")
        if get_property("sid") != source_sid:
            raise RuntimeError("Disabling OCR changed the selected source track")
        command("seek", "0", "absolute+exact")
        set_property("sub-translate", "yes")
        wait_for(lambda: get_property("secondary-sub-text") == expected, "post-seek bitmap text")
        with tempfile.TemporaryDirectory(prefix="mpv-ocr-text-") as directory:
            subtitle = Path(directory) / "text.srt"
            subtitle.write_text(
                "1\n00:00:00,000 --> 00:00:10,000\nText regression\n", encoding="utf-8")
            command("sub-add", str(subtitle), "select")
            wait_for(lambda: get_property("secondary-sub-text") == "translated:Text regression",
                     "ordinary text subtitle after bitmap track")
            status = json.loads(get_property("sub-translate-status"))
            if status["presentation"] != "companion":
                raise RuntimeError("Bitmap presentation leaked into the text track")
            command("stop")
        with server.lock:
            if ((not args.expect_target_reuse and args.expected_text not in server.requests) or
                    "Text regression" not in server.requests):
                raise RuntimeError(f"Expected translation requests missing: {server.requests!r}")
        print("Bitmap OCR, text output, configuration rejection, seek, disable and text-track switch passed.")
    finally:
        lib.mpv_terminate_destroy(ctx)
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()
        for directory in dll_directories:
            directory.close()
        if thread.is_alive():
            raise RuntimeError("Loopback translation fixture did not stop")


if __name__ == "__main__":
    main()
