#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Real-DLL owned-frame timeout/skip/error/success tests with software rendering.

No window, GPU, audio device or network. All pixels stay in a local CPU buffer.
"""

import contextlib
import ctypes
import json
import os
from pathlib import Path
import sys
import threading
import time

from libmpv_paused_refresh import Node, decode


class RenderParam(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("data", ctypes.c_void_p)]


def run(library, artifacts):
    library = Path(library).resolve(strict=True)
    artifacts = Path(artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    media = artifacts / "paused-refresh-render.y4m"
    with media.open("wb") as stream:
        stream.write(b"YUV4MPEG2 W16 H16 F10:1 Ip A1:1 C420jpeg\n")
        for index in range(60):
            stream.write(b"FRAME\n" + bytes([32 + index]) * 256 + bytes([128]) * 128)

    with contextlib.ExitStack() as stack:
        if os.name == "nt":
            stack.enter_context(os.add_dll_directory(str(library.parent)))
        mpv = ctypes.CDLL(str(library))
        mpv.mpv_create.restype = ctypes.c_void_p
        mpv.mpv_set_option_string.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ]
        mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
        mpv.mpv_command.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ]
        mpv.mpv_get_property.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
        ]
        mpv.mpv_free_node_contents.argtypes = [ctypes.POINTER(Node)]
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        mpv.mpv_render_context_create.argtypes = [
            ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
            ctypes.POINTER(RenderParam),
        ]
        mpv.mpv_render_context_update.argtypes = [ctypes.c_void_p]
        mpv.mpv_render_context_update.restype = ctypes.c_uint64
        mpv.mpv_render_context_render.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(RenderParam),
        ]
        mpv.mpv_render_context_free.argtypes = [ctypes.c_void_p]
        mpv.mpv_render_context_free.restype = None
        client = mpv.mpv_create()
        assert client
        stack.callback(mpv.mpv_terminate_destroy, client)
        for name, value in {
            "config": "no", "load-scripts": "no", "terminal": "no",
            "vo": "libmpv", "ao": "null", "hwdec": "no", "pause": "yes",
            "keep-open": "yes", "idle": "yes", "ytdl": "no",
            "video-latency-hacks": "yes",
        }.items():
            assert mpv.mpv_set_option_string(
                client, name.encode(), value.encode()) >= 0, name
        assert mpv.mpv_initialize(client) >= 0
        api = ctypes.create_string_buffer(b"sw")
        create_params = (RenderParam * 2)(
            RenderParam(1, ctypes.cast(api, ctypes.c_void_p)), RenderParam())
        ctx = ctypes.c_void_p()
        assert mpv.mpv_render_context_create(ctypes.byref(ctx), client, create_params) >= 0
        stack.callback(mpv.mpv_render_context_free, ctx)

        size = (ctypes.c_int * 2)(16, 16)
        fmt = ctypes.create_string_buffer(b"rgb0")
        stride = ctypes.c_size_t(64)
        bad_stride = ctypes.c_size_t(1)
        block = ctypes.c_int(0)
        skip = ctypes.c_int(1)
        buffer = ctypes.create_string_buffer(16 * 16 * 4 + 63)
        aligned = (ctypes.addressof(buffer) + 63) & ~63

        def params(mode):
            if mode == "skip":
                return (RenderParam * 3)(
                    RenderParam(13, ctypes.addressof(skip)),
                    RenderParam(12, ctypes.addressof(block)), RenderParam())
            return (RenderParam * 6)(
                RenderParam(17, ctypes.addressof(size)),
                RenderParam(18, ctypes.addressof(fmt)),
                RenderParam(19, ctypes.addressof(
                    bad_stride if mode == "error" else stride)),
                RenderParam(20, aligned),
                RenderParam(12, ctypes.addressof(block)), RenderParam())

        def get(name):
            node = Node()
            assert mpv.mpv_get_property(
                client, name.encode(), 6, ctypes.byref(node)) >= 0, name
            try:
                return decode(node)
            finally:
                mpv.mpv_free_node_contents(ctypes.byref(node))

        def command(*words):
            args = (ctypes.c_char_p * (len(words) + 1))(
                *(str(word).encode("utf-8") for word in words), None)
            return mpv.mpv_command(client, args)

        def until(predicate, timeout=10):
            deadline = time.monotonic() + timeout
            last = None
            while time.monotonic() < deadline:
                last = get("rodel-seek-state")
                if predicate(last):
                    return last
                time.sleep(0.005)
            raise AssertionError(f"Software-render receipt timeout: {last}")

        def drained():
            return until(lambda s: s["known"] and not s["pending"] and not s["active"])

        reports = {}
        assert command("loadfile", str(media)) >= 0
        state = drained()
        assert command("rodel-paused-refresh", 1, state["position"]) >= 0
        timed_out = until(lambda s: not s["pending"] and not s["active"])
        reports["timeout-with-no-render-consumer"] = timed_out
        (artifacts / "render-receipt.json").write_text(
            json.dumps(reports, indent=2) + "\n", encoding="utf-8")
        assert timed_out["state"] == "canceled", timed_out
        assert timed_out["last-applied"]["id"] == 1
        assert timed_out["last-completed"]["id"] == 0
        assert get("pause") is True
        # A late successful render of the timed-out current frame is not allowed
        # to resurrect the same owned receipt.
        assert mpv.mpv_render_context_render(ctx, params("success")) >= 0
        assert get("rodel-seek-state")["state"] == "canceled"
        reports["late-render-after-timeout"] = "passed"

        stopped = threading.Event()
        mode = ["success"]
        errors = []
        calls = []

        def render_loop():
            try:
                while not stopped.wait(0.002):
                    if mpv.mpv_render_context_update(ctx) & 1:
                        selected = mode[0]
                        result = mpv.mpv_render_context_render(ctx, params(selected))
                        calls.append((selected, result))
                        if selected != "error" and result < 0:
                            errors.append(f"{selected} render failed: {result}")
                            return
            except BaseException as error:
                errors.append(repr(error))

        thread = threading.Thread(target=render_loop, name="local-sw-receipt")
        thread.start()

        def stop_renderer():
            stopped.set()
            thread.join(5)
            assert not thread.is_alive(), "Session-owned software renderer did not stop"

        stack.callback(stop_renderer)
        assert command("rodel-paused-refresh", 2, drained()["position"]) >= 0
        succeeded = until(lambda s: s["state"] == "completed" and
                          s["last-completed"]["id"] == 2)
        assert not errors, errors
        reports["exact-software-processing-success"] = succeeded

        for request_id, selected in ((3, "error"), (4, "skip")):
            mode[0] = selected
            state = drained()
            assert command("rodel-paused-refresh", request_id, state["position"]) >= 0
            failed = until(lambda s: not s["pending"] and not s["active"])
            assert failed["state"] == "canceled", failed
            assert failed["last-canceled"]["id"] == request_id
            assert failed["last-completed"]["id"] == 2
            assert get("pause") is True
            reports[f"{selected}-does-not-complete"] = failed
        assert any(selected == "error" and result < 0
                   for selected, result in calls), calls
        assert any(selected == "skip" and result == 0
                   for selected, result in calls), calls
        assert not errors, errors
        reports["render-calls"] = calls
        (artifacts / "render-receipt.json").write_text(
            json.dumps(reports, indent=2) + "\n", encoding="utf-8")
        assert command("stop") >= 0
        print("PAUSED_REFRESH_RENDER_API_OK cpu-only timeout/drop/error/skip/success")


if __name__ == "__main__":
    if not __debug__:
        raise SystemExit("This assertion-based feature test requires normal Python mode")
    if len(sys.argv) != 3:
        raise SystemExit("usage: libmpv_paused_refresh_render.py LIBRARY ARTIFACT_DIRECTORY")
    run(sys.argv[1], sys.argv[2])
