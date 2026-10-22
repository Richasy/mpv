#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Exercise the built DLL's refresh API using local seekable Y4M and null I/O.

No network, window, GPU, audio device, or user configuration is used.
The output directory must be inside the caller's chosen working directory.
"""

import contextlib
import ctypes
import json
import os
from pathlib import Path
import sys
import time


class Node(ctypes.Structure):
    pass


class NodeList(ctypes.Structure):
    _fields_ = [
        ("num", ctypes.c_int),
        ("values", ctypes.POINTER(Node)),
        ("keys", ctypes.POINTER(ctypes.c_char_p)),
    ]


class Value(ctypes.Union):
    _fields_ = [
        ("string", ctypes.c_char_p),
        ("flag", ctypes.c_int),
        ("int64", ctypes.c_int64),
        ("double", ctypes.c_double),
        ("list", ctypes.POINTER(NodeList)),
        ("pointer", ctypes.c_void_p),
    ]


Node._fields_ = [("u", Value), ("format", ctypes.c_int)]


def decode(node):
    if node.format == 0:
        return None
    if node.format == 1:
        return node.u.string.decode("utf-8")
    if node.format == 3:
        return bool(node.u.flag)
    if node.format == 4:
        return node.u.int64
    if node.format == 5:
        return node.u.double
    if node.format in (7, 8):
        values = node.u.list.contents
        if node.format == 7:
            return [decode(values.values[i]) for i in range(values.num)]
        return {
            values.keys[i].decode(): decode(values.values[i])
            for i in range(values.num)
        }
    raise AssertionError(f"Unexpected node format {node.format}")


def run(library, artifacts):
    library = Path(library).resolve(strict=True)
    artifacts = Path(artifacts).resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    media = artifacts / "paused-refresh.y4m"
    with media.open("wb") as stream:
        stream.write(b"YUV4MPEG2 W16 H16 F10:1 Ip A1:1 C420jpeg\n")
        for index in range(60):
            stream.write(b"FRAME\n")
            stream.write(bytes([32 + index % 128]) * 256 + bytes([128]) * 128)

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
        mpv.mpv_command_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_get_property.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
        ]
        mpv.mpv_set_property.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p,
        ]
        mpv.mpv_free_node_contents.argtypes = [ctypes.POINTER(Node)]
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        client = mpv.mpv_create()
        assert client, "mpv_create failed"
        stack.callback(mpv.mpv_terminate_destroy, client)
        for name, value in {
            "config": "no", "load-scripts": "no", "terminal": "no",
            "vo": "null", "ao": "null", "hwdec": "no", "pause": "yes",
            "keep-open": "yes", "idle": "yes", "ytdl": "no",
            # Owned completion must also wait for its frame with this enabled.
            "video-latency-hacks": "yes",
        }.items():
            assert mpv.mpv_set_option_string(
                client, name.encode(), value.encode()) >= 0, name
        assert mpv.mpv_initialize(client) >= 0

        def get(name):
            node = Node()
            result = mpv.mpv_get_property(
                client, name.encode(), 6, ctypes.byref(node))
            assert result >= 0, (name, result)
            try:
                return decode(node)
            finally:
                mpv.mpv_free_node_contents(ctypes.byref(node))

        def command(*words):
            args = (ctypes.c_char_p * (len(words) + 1))(
                *(str(word).encode("utf-8") for word in words), None)
            return mpv.mpv_command(client, args)

        def batch(text):
            assert mpv.mpv_command_string(client, text.encode()) >= 0

        def snapshot():
            value = get("rodel-seek-state")
            assert set(value) == {
                "version", "known", "pending", "active", "source-epoch",
                "seek-revision", "owner-epoch", "owner-revision", "state",
                "position", "last-requested", "last-applied",
                "last-completed", "last-canceled",
            }, value
            assert value["version"] == 1
            for name in (
                "last-requested", "last-applied", "last-completed", "last-canceled",
            ):
                assert set(value[name]) == {"id", "saved-position"}
            return value

        def until(predicate, timeout=10):
            deadline = time.monotonic() + timeout
            last = None
            while time.monotonic() < deadline:
                last = snapshot()
                if predicate(last):
                    return last
                time.sleep(0.01)
            raise AssertionError(f"Snapshot timeout: {last}")

        def idle_source():
            return until(lambda s: s["known"] and not s["pending"] and not s["active"])

        reports = {}
        first = snapshot()
        assert not first["known"] and first["state"] == "unknown"
        assert first["position"] is None and first["last-requested"]["id"] == 0
        assert command("rodel-paused-refresh", 1, 0) < 0
        readonly = Node()
        assert mpv.mpv_get_property(
            client, b"rodel-seek-state", 6, ctypes.byref(readonly)) >= 0
        try:
            assert mpv.mpv_set_property(
                client, b"rodel-seek-state", 6, ctypes.byref(readonly)) < 0
        finally:
            mpv.mpv_free_node_contents(ctypes.byref(readonly))
        assert snapshot() == first
        commands = get("command-list")
        assert any(c["name"] == "rodel-paused-refresh" for c in commands)
        reports["unknown-readonly-entry"] = "passed"

        assert command("loadfile", str(media)) >= 0
        loaded = idle_source()
        assert get("pause") is True and get("seekable") is True
        assert loaded["source-epoch"] > 0
        assert command("seek", 1.2, "absolute+exact") >= 0
        loaded = until(lambda s: s["known"] and not s["pending"] and
                       not s["active"] and abs(s["position"] - 1.2) < 0.11)
        position = loaded["position"]
        assert command("rodel-paused-refresh", 0, position) < 0
        assert command("rodel-paused-refresh", 1, position + 0.1) < 0

        # A synchronous command list queues the ordinary/automatic seek before
        # admission; the player loop cannot execute it between these commands.
        batch(f"seek {position} absolute+exact; "
              f"rodel-paused-refresh 1 {position}")
        after = idle_source()
        assert after["last-requested"]["id"] == 0
        batch(f"vf add hflip; rodel-paused-refresh 1 {after['position']}")
        after = idle_source()
        assert after["last-requested"]["id"] == 0
        reports["prequeued-normal-and-vf-auto"] = "passed"

        position = after["position"]
        assert command("rodel-paused-refresh", 1, position) >= 0
        completed = until(lambda s: s["state"] == "completed")
        for key in ("last-requested", "last-applied", "last-completed"):
            assert completed[key] == {"id": 1, "saved-position": position}
        assert not completed["active"] and not completed["pending"]
        assert completed["owner-epoch"] == completed["source-epoch"]
        assert completed["owner-revision"] == completed["seek-revision"]
        assert abs(completed["position"] - position) < 0.11
        assert get("pause") is True
        assert command("rodel-paused-refresh", 1, position) < 0
        reports["owned-native-frame-and-restart"] = completed

        position = idle_source()["position"]
        batch(f"rodel-paused-refresh 2 {position}; seek 2 absolute+exact")
        superseded = idle_source()
        assert superseded["state"] == "canceled"
        assert superseded["last-requested"]["id"] == 2
        assert superseded["last-canceled"]["id"] == 2
        assert superseded["last-applied"]["id"] == 1
        assert superseded["last-completed"]["id"] == 1
        assert get("pause") is True
        reports["owned-pending-normal-supersession"] = "passed"

        assert command("rodel-paused-refresh", 3, superseded["position"]) >= 0
        until(lambda s: s["state"] == "completed" and
              s["last-completed"]["id"] == 3)
        assert command("vf", "add", "vflip") >= 0
        reconfigured = idle_source()
        assert reconfigured["state"] == "canceled"
        assert reconfigured["last-canceled"]["id"] == 3
        reports["completed-owner-reconfiguration"] = "passed"

        old_epoch = reconfigured["source-epoch"]
        assert command("loadfile", str(media)) >= 0
        changed = until(lambda s: s["source-epoch"] > old_epoch and s["known"] and
                        not s["pending"] and not s["active"])
        assert command("rodel-paused-refresh", 3, changed["position"]) < 0
        assert command("rodel-paused-refresh", 4, changed["position"]) >= 0
        until(lambda s: s["state"] == "completed" and
              s["last-completed"]["id"] == 4)
        assert command("stop") >= 0
        stopped = until(lambda s: not s["known"])
        assert stopped["state"] == "canceled" and stopped["last-canceled"]["id"] == 4
        assert command("rodel-paused-refresh", 5, 0) < 0
        reports["source-change-stop-and-id-replay"] = "passed"

        assert command("loadfile", str(media)) >= 0
        idle_source()
        assert command("seek", 100, "absolute+exact") >= 0
        until(lambda s: not s["pending"] and not s["active"])
        assert get("eof-reached") is True
        assert command("rodel-paused-refresh", 5, get("time-pos")) < 0
        assert get("pause") is True and get("eof-reached") is True
        reports["eof-no-replay-no-unpause"] = "passed"

        eof_epoch = snapshot()["source-epoch"]
        assert command("loadfile", str(media)) >= 0
        until(lambda s: s["source-epoch"] > eof_epoch and s["known"] and
              not s["pending"] and not s["active"])
        assert command("seek", 1.2, "absolute+exact") >= 0
        until(lambda s: s["known"] and not s["pending"] and not s["active"] and
              abs(s["position"] - 1.2) < 0.000001)
        for request_id, crop in (
            (5, "8x8+0+0"), (7, "10x10+0+0"), (9, "12x12+0+0"),
        ):
            before = idle_source()
            old_crop = get("video-crop")
            # UPDATE_IMGPAR -> mp_force_video_refresh -> issue_refresh_seek
            # sees the still-pending owned request in this synchronous list.
            batch(f"rodel-paused-refresh {request_id} {before['position']}; "
                  f"set video-crop {crop}")
            canceled = idle_source()
            actual_crop = get("video-crop")
            assert actual_crop != old_crop, (old_crop, actual_crop)
            reports[f"pending-owned-before-crop-{request_id}"] = {
                "crop": actual_crop, "state": canceled,
            }
            # Preserve the failing native snapshot before an assertion exits.
            (artifacts / "paused-refresh-api-receipt.json").write_text(
                json.dumps(reports, indent=2) + "\n", encoding="utf-8")
            assert canceled["state"] == "canceled", canceled
            assert canceled["last-requested"]["id"] == request_id
            assert canceled["last-canceled"]["id"] == request_id
            assert canceled["last-applied"]["id"] != request_id
            assert canceled["last-completed"]["id"] != request_id
            assert canceled["seek-revision"] > canceled["owner-revision"]
            assert canceled["source-epoch"] == before["source-epoch"]
            assert abs(canceled["position"] - before["position"]) < 0.000001
            assert get("pause") is True
            # After the coalesced automatic seek drains, a fresh owned ID can
            # complete normally at the same actual position and crop.
            recovery_id = request_id + 1
            assert command("rodel-paused-refresh", recovery_id,
                           canceled["position"]) >= 0
            recovered = until(lambda s: s["state"] == "completed" and
                              s["last-completed"]["id"] == recovery_id)
            assert recovered["owner-revision"] == recovered["seek-revision"]
            assert get("video-crop") == actual_crop
            reports[f"owned-after-crop-drain-{recovery_id}"] = recovered

        # Coalescing ordinary pending work keeps its target and does not create
        # or apply an owned request. Crop-first admission still fails while busy.
        batch("seek 1.4 absolute+exact; set video-crop 16x16+0+0")
        ordinary = idle_source()
        assert abs(ordinary["position"] - 1.4) < 0.000001, ordinary
        assert ordinary["last-requested"]["id"] == 10
        assert ordinary["last-completed"]["id"] == 10
        batch(f"set video-crop 8x8+0+0; "
              f"rodel-paused-refresh 11 {ordinary['position']}")
        denied = idle_source()
        assert denied["last-requested"]["id"] == 10
        assert denied["last-completed"]["id"] == 10
        assert command("rodel-paused-refresh", 11, denied["position"]) >= 0
        until(lambda s: s["state"] == "completed" and
              s["last-completed"]["id"] == 11)
        reports["ordinary-crop-coalescing-and-crop-first-admission"] = "passed"
        (artifacts / "paused-refresh-api-receipt.json").write_text(
            json.dumps(reports, indent=2) + "\n", encoding="utf-8")
        print("PAUSED_REFRESH_DLL_API_OK local-seekable-media null-vo null-ao")


if __name__ == "__main__":
    if not __debug__:
        raise SystemExit("This assertion-based feature test requires normal Python mode")
    if len(sys.argv) != 3:
        raise SystemExit("usage: libmpv_paused_refresh.py LIBRARY ARTIFACT_DIRECTORY")
    run(sys.argv[1], sys.argv[2])
