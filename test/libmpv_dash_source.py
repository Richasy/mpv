#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Exercise the opt-in typed source against two locally generated DASH MP4 tracks.

Run with the published local-build directory, not an older/system libmpv:
    python test/libmpv_dash_source.py build/local-libmpv/x86_64
The --scheduler mode requires a separate test-only DLL compiled from the same
snapshot with MPV_DASH_TEST_HOOKS; the production DLL must not export its hook.
All media is generated under that repository-owned ignored build directory and
removed after the test. This never requests a remote media service.
"""

import contextlib
import ctypes
from datetime import datetime, timezone
import faulthandler
import gc
import gzip
import http.server
import os
from pathlib import Path
import re
import shutil
import ssl
import subprocess
import sys
import threading
import time
import urllib.parse


VERSION = 1
ALLOW_LOOPBACK = 1
SEGMENT_BASE = 1
INVALID = -4
UNSUPPORTED = -18
IDLE, QUEUED, BOUND, FAILED, STOPPED = range(5)
AUTH, RISK, HTTP_STATUS, HTTP_RANGE, TRANSPORT = 3, 4, 5, 6, 7
ORIGIN_NONE, ORIGIN_OTHER, ORIGIN_MALFORMED, ORIGIN_DUPLICATE, \
    ORIGIN_INTERIM, ORIGIN_REDIRECT, ORIGIN_LENGTH, ORIGIN_OFFSET, \
    ORIGIN_WINDOW, ORIGIN_PARTIAL, ORIGIN_BODY_LENGTH = range(11)
VIDEO, AUDIO = 1, 2
FILE_LOADED, END_FILE, LOG_MESSAGE = 8, 7, 2
SEEK_EVENT, PLAYBACK_RESTART, DOUBLE = 20, 21, 5
END_ERROR = 4
MARKER = b"private-local-test"
TEST_ARM, TEST_WAIT, TEST_RELEASE, TEST_SUCCESS_BRANCHES, \
    TEST_BLOCKED_CONTINUATIONS, TEST_ADDED_AFTER_STOP = range(1, 7)
TEST_AUDIO_WINDOW, TEST_VIDEO_WINDOW = 7, 8
TEST_COMPLETE_RECV_ERROR, TEST_ABORT_WAITING, TEST_OTHER_TRACK_RISK = 9, 10, 11


class Range(ctypes.Structure):
    _fields_ = [("start", ctypes.c_int64), ("end", ctypes.c_int64)]


class Track(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("url", ctypes.c_char_p),
        ("mime_type", ctypes.c_char_p),
        ("codec", ctypes.c_char_p),
        ("bandwidth", ctypes.c_uint64),
        ("quality", ctypes.c_int32),
        ("width", ctypes.c_int32),
        ("height", ctypes.c_int32),
        ("initialization", Range),
        ("index", Range),
    ]


class Source(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("video", Track),
        ("audio", Track),
        ("duration_seconds", ctypes.c_double),
        ("user_agent", ctypes.c_char_p),
        ("referer", ctypes.c_char_p),
    ]


class Status(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("generation", ctypes.c_uint64),
        ("phase", ctypes.c_int32),
        ("failure", ctypes.c_int32),
        ("failed_track", ctypes.c_int32),
        ("video_http_status", ctypes.c_int32),
        ("audio_http_status", ctypes.c_int32),
        ("video_responses", ctypes.c_uint32),
        ("audio_responses", ctypes.c_uint32),
    ]


class FailureDetail(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("source_generation", ctypes.c_uint64),
        ("origin", ctypes.c_int32),
        ("failed_track", ctypes.c_int32),
        ("http_status", ctypes.c_int32),
        ("response_count", ctypes.c_uint32),
    ]


class RangeCapability(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("source_generation", ctypes.c_uint64),
        ("video_validated_206", ctypes.c_uint32),
        ("audio_validated_206", ctypes.c_uint32),
    ]


class FrameStatus(ctypes.Structure):
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("api_version", ctypes.c_uint32),
        ("source_generation", ctypes.c_uint64),
        ("presented_generation", ctypes.c_uint64),
        ("presented_frame_serial", ctypes.c_uint64),
        ("presented_surface_epoch", ctypes.c_uint64),
    ]


class CompositionSurface(ctypes.Structure):
    _fields_ = [("swapchain", ctypes.c_void_p), ("epoch", ctypes.c_uint64)]


class Event(ctypes.Structure):
    _fields_ = [
        ("event_id", ctypes.c_int),
        ("error", ctypes.c_int),
        ("reply_userdata", ctypes.c_uint64),
        ("data", ctypes.c_void_p),
    ]


class EndFile(ctypes.Structure):
    _fields_ = [("reason", ctypes.c_int), ("error", ctypes.c_int)]


class EventProperty(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p)]


class LogMessage(ctypes.Structure):
    _fields_ = [
        ("prefix", ctypes.c_char_p),
        ("level", ctypes.c_char_p),
        ("text", ctypes.c_char_p),
        ("log_level", ctypes.c_int),
    ]


def boxes(data):
    result = []
    pos = 0
    while pos + 8 <= len(data):
        size = int.from_bytes(data[pos:pos + 4], "big")
        if size < 8 or pos + size > len(data):
            break
        result.append(data[pos + 4:pos + 8])
        pos += size
    return result


def first_fragment_offset(data):
    pos = 0
    while pos + 8 <= len(data):
        size = int.from_bytes(data[pos:pos + 4], "big")
        if data[pos + 4:pos + 8] == b"moof":
            return pos
        if size < 8 or pos + size > len(data):
            break
        pos += size
    raise AssertionError("Generated video has no first media fragment")


def generate_tracks(directory):
    files = {}
    inputs = {
        "video": [
            "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=12",
            "-t", "18", "-an", "-c:v", "libx264", "-preset", "ultrafast",
            "-crf", "17", "-pix_fmt", "yuv420p", "-g", "12",
        ],
        "audio": [
            "-f", "lavfi", "-i",
            "anoisesrc=color=white:amplitude=0.03:sample_rate=48000",
            "-t", "18", "-vn", "-c:a", "aac", "-b:a", "160k",
        ],
    }
    for role, arguments in inputs.items():
        path = directory / (role + ".mp4")
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error",
             "-y", *arguments, "-movflags",
             "+frag_keyframe+empty_moov+global_sidx+dash",
             "-f", "mp4", str(path)],
            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
            check=False,
        )
        if result.returncode:
            raise RuntimeError(f"Local media generator failed for {role}: "
                               f"{result.stderr[-400:]}")
        data = path.read_bytes()
        top = boxes(data)
        if not all(box in top for box in (b"ftyp", b"moov", b"sidx",
                                           b"moof", b"mdat")):
            raise AssertionError(f"Local {role} fixture is not indexed DASH fMP4")
        files[role] = data
    return files


def generate_tls_certificates(directory):
    def wsl_path(path):
        absolute = path.resolve()
        return f"/mnt/{absolute.drive[0].lower()}/{absolute.relative_to(absolute.anchor).as_posix()}"

    def openssl(*args):
        result = subprocess.run(
            ["wsl.exe", "-d", "mpv-build", "--", "openssl", *args],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            timeout=30, check=False,
        )
        if result.returncode:
            raise AssertionError("Local synthetic TLS certificate generation failed")

    ca_cert, ca_key = directory / "ca.pem", directory / "ca-key.pem"
    client_cert, client_key = directory / "client.pem", directory / "client-key.pem"
    server_cert, server_key = directory / "server.pem", directory / "server-key.pem"
    client_request = directory / "client.csr"
    client_extensions = directory / "client-ext.cnf"
    client_extensions.write_text(
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature\n"
        "extendedKeyUsage=clientAuth\n", encoding="ascii")

    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", wsl_path(ca_key), "-out", wsl_path(ca_cert),
            "-subj", "/CN=local-test-ca", "-days", "1")
    openssl("req", "-new", "-newkey", "rsa:2048", "-nodes",
            "-keyout", wsl_path(client_key), "-out", wsl_path(client_request),
            "-subj", "/CN=local-test-client")
    openssl("x509", "-req", "-in", wsl_path(client_request),
            "-CA", wsl_path(ca_cert), "-CAkey", wsl_path(ca_key),
            "-CAcreateserial", "-out", wsl_path(client_cert),
            "-extfile", wsl_path(client_extensions), "-days", "1")
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", wsl_path(server_key), "-out", wsl_path(server_cert),
            "-subj", "/CN=server-local-test",
            "-addext", "subjectAltName=IP:127.0.0.1", "-days", "1")
    validity = subprocess.run(
        ["wsl.exe", "-d", "mpv-build", "--", "openssl", "x509",
         "-in", wsl_path(client_cert), "-noout", "-startdate"],
        capture_output=True, text=True, timeout=15, check=True,
    ).stdout.strip()
    if not validity.startswith("notBefore="):
        raise AssertionError("Synthetic client certificate validity is unavailable")
    not_before = datetime.strptime(
        validity.removeprefix("notBefore="), "%b %d %H:%M:%S %Y GMT"
    ).replace(tzinfo=timezone.utc).timestamp()
    wait = max(0, not_before - time.time() + 0.5)
    if wait > 30:
        raise AssertionError("Windows/WSL certificate clock skew exceeds 30 seconds")
    time.sleep(wait)
    return {
        "ca": (ca_cert, ca_key),
        "client": (client_cert, client_key),
        "server": (server_cert, server_key),
    }


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    block_on_close = False

    def __init__(self, tracks, certificates=None):
        super().__init__(("127.0.0.1", 0), Handler)
        self.secure = certificates is not None
        if certificates:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(*map(str, certificates["server"]))
            context.load_verify_locations(cafile=str(certificates["ca"][0]))
            context.verify_mode = ssl.CERT_OPTIONAL
            self.socket = context.wrap_socket(self.socket, server_side=True)
        self.tracks = tracks
        self.lock = threading.Lock()
        self.requests = []
        self.early_hints_roles = set()
        self.interim_count_roles = {}
        self.malformed_status_roles = set()
        self.duplicate_final_roles = set()
        self.short_initial_roles = set()
        self.short_initial_bytes = 2048
        self.pause_first_body_role = None
        self.first_body_held = threading.Event()
        self.release_first_body = threading.Event()
        self.release_first_body.set()
        self.hold_followup_role = None
        self.followup_requested = threading.Event()
        self.release_followup_headers = threading.Event()
        self.release_followup_headers.set()
        self.redirect = None
        self.bad_range = None
        self.deny = None
        self.deny_code = 412
        self.arm_failure = None
        self.truncate_range_role = None
        self.disconnect_role = None
        self.disconnect_attempts = 0
        self.disconnect_bytes = 32768
        self.wrong_total_role = None
        self.arm_code = 412
        self.full_body_roles = set()
        self.no_range_advertisement = set()
        self.full_body_after_first_zero = set()
        self.full_body_status = {}
        self.full_body_payloads = {}
        self.full_body_types = {}
        self.full_body_lengths = {}
        self.full_body_encodings = {}
        self.duplicate_identity_encoding_roles = set()
        self.range_encodings = {}
        self.full_body_with_range = set()
        self.content_length_conflict_roles = set()
        self.redirect_hits = 0
        self.error_body_bytes = 0
        self.error_headers = threading.Event()
        self.release_error_body = threading.Event()
        self.video_header_bytes = first_fragment_offset(tracks["video"])
        self.hold_video_body = False
        self.video_body_blocked = threading.Event()
        self.release_video_body = threading.Event()
        self.release_video_body.set()
        self.hold_audio_body = False
        self.audio_body_blocked = threading.Event()
        self.release_audio_body = threading.Event()
        self.release_audio_body.set()
        self.active = 0

    def record(self, role, start, status, headers, has_client_cert):
        with self.lock:
            self.requests.append((role, start, status,
                                  headers.get("User-Agent"),
                                  headers.get("Referer"),
                                  headers.get("Cookie"), has_client_cert,
                                  headers.get("Range")))

    def requests_for(self, role):
        with self.lock:
            return [r for r in self.requests if r[0] == role]


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path == "/redirect-target":
            with self.server.lock:
                self.server.redirect_hits += 1
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        role = path.removeprefix("/")
        if role not in self.server.tracks:
            self.send_error(404)
            return

        requested = self.headers.get("Range", "")
        match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
        if not match:
            self.send_error(416)
            return
        data = self.server.tracks[role]
        start = int(match[1])
        end = min(int(match[2]) if match[2] else len(data) - 1, len(data) - 1)
        if self.server.hold_video_body and role == "video" and start == 0:
            end = min(end, self.server.video_header_bytes - 1)
        if role in self.server.short_initial_roles and start == 0:
            end = min(end, self.server.short_initial_bytes - 1)
        if start > end:
            self.send_error(416)
            return
        if self.server.hold_followup_role == role and start > 0:
            self.server.followup_requested.set()
            if not self.server.release_followup_headers.wait(10):
                return

        media_candidate = False
        if self.server.redirect == role:
            code = 302
        elif self.server.deny == role:
            code = self.server.deny_code
        elif self.server.arm_failure == role and start > 0:
            code = self.server.arm_code
        elif start == 0 and (
            role in self.server.full_body_roles or
            (role in self.server.full_body_after_first_zero and
             self.server.requests_for(role))
        ):
            code = self.server.full_body_status.get(role, 200)
            media_candidate = True
        else:
            code = 206
        client_cert = (
            isinstance(self.connection, ssl.SSLSocket) and
            bool(self.connection.getpeercert(binary_form=True))
        )
        self.server.record(role, start, code, self.headers, client_cert)
        with self.server.lock:
            self.server.active += 1
        try:
            if role in self.server.malformed_status_roles:
                self.wfile.write(
                    b"HTTP/1.1 20X Invalid\r\nContent-Length: 0\r\n"
                    b"Connection: close\r\n\r\n")
                return
            hints = self.server.interim_count_roles.get(
                role, int(role in self.server.early_hints_roles))
            for _ in range(hints):
                self.send_response_only(103, "Early Hints")
                self.send_header("Link", "</unused>; rel=preload")
                self.end_headers()
                self.wfile.flush()
            if role in self.server.duplicate_final_roles:
                self.send_response_only(code)
            self.send_response(code)
            if code == 302:
                self.send_header(
                    "Location",
                    f"http://127.0.0.1:{self.server.server_port}/redirect-target",
                )
                self.send_header("Content-Length", "0")
            elif media_candidate:
                body = self.server.full_body_payloads.get(role, data)
                self.send_header(
                    "Content-Type",
                    self.server.full_body_types.get(role, f"{role}/mp4"),
                )
                if role not in self.server.no_range_advertisement:
                    self.send_header("Accept-Ranges", "bytes")
                if role in self.server.full_body_encodings:
                    if role in self.server.duplicate_identity_encoding_roles:
                        self.send_header("Content-Encoding", "identity")
                    self.send_header(
                        "Content-Encoding", self.server.full_body_encodings[role])
                if role in self.server.full_body_with_range:
                    self.send_header(
                        "Content-Range", f"bytes 0-{len(body) - 1}/{len(body)}")
                length = self.server.full_body_lengths.get(role, str(len(body)))
                if length is not None:
                    self.send_header("Content-Length", str(length))
                if role in self.server.content_length_conflict_roles:
                    self.send_header("Transfer-Encoding", "chunked")
            elif code in (200, 401, 403, 404, 412, 416):
                self.send_header("Content-Length", "64")
            else:
                self.send_header("Content-Type", f"{role}/mp4")
                self.send_header("Accept-Ranges", "bytes")
                if role in self.server.range_encodings:
                    self.send_header("Content-Encoding", "identity")
                    self.send_header("Content-Encoding", self.server.range_encodings[role])
                actual_start = start + 1 if self.server.bad_range == role else start
                total = len(data) + int(self.server.wrong_total_role == role)
                self.send_header(
                    "Content-Range", f"bytes {actual_start}-{end}/{total}"
                )
                self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Connection", "close")
            self.end_headers()
            if (role == self.server.disconnect_role and
                len(self.server.requests_for(role)) <= self.server.disconnect_attempts and
                (media_candidate or code == 206)):
                payload = body if media_candidate else data[start:end + 1]
                cut = min(self.server.disconnect_bytes, max(0, len(payload) - 1))
                self.wfile.write(payload[:cut])
                self.wfile.flush()
                return
            if code == 206:
                if self.server.truncate_range_role == role and start > 0:
                    self.wfile.write(data[start:start + min(32, end - start)])
                    self.wfile.flush()
                    return
                if self.server.pause_first_body_role == role and start == 0:
                    self.server.first_body_held.set()
                    if not self.server.release_first_body.wait(15):
                        return
                if (self.server.hold_video_body and role == "video" and
                    end >= self.server.video_header_bytes):
                    self.server.video_body_blocked.set()
                    if not self.server.release_video_body.wait(15):
                        return
                if self.server.hold_audio_body and role == "audio":
                    self.server.audio_body_blocked.set()
                    if not self.server.release_audio_body.wait(15):
                        return
                self.wfile.write(data[start:end + 1])
            elif media_candidate:
                if role in self.server.content_length_conflict_roles:
                    self.wfile.write(
                        f"{len(body):X}\r\n".encode() + body + b"\r\n0\r\n\r\n")
                else:
                    self.wfile.write(body)
            elif code in (200, 401, 403, 404, 412, 416):
                self.server.error_headers.set()
                if self.server.release_error_body.wait(10):
                    self.wfile.write(bytes(64))
                    with self.server.lock:
                        self.server.error_body_bytes += 64
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError,
                ssl.SSLEOFError):
            pass
        finally:
            self.close_connection = True
            with self.server.lock:
                self.server.active -= 1


@contextlib.contextmanager
def serve(tracks, certificates=None):
    server = Server(tracks, certificates)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.release_error_body.set()
        server.release_video_body.set()
        server.release_audio_body.set()
        server.release_first_body.set()
        server.release_followup_headers.set()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            with server.lock:
                if server.active == 0:
                    break
            time.sleep(0.02)
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)
        with server.lock:
            if server.active:
                raise AssertionError("Media request survived native session teardown")


def configure_library(mpv):
    mpv.mpv_client_api_version.restype = ctypes.c_ulong
    mpv.mpv_create.restype = ctypes.c_void_p
    mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
    mpv.mpv_set_option_string.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    mpv.mpv_request_log_messages.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    mpv.mpv_dash_source_load.argtypes = [ctypes.c_void_p, ctypes.POINTER(Source)]
    mpv.mpv_dash_source_get_status.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(Status)]
    mpv.mpv_dash_source_get_failure_detail.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(FailureDetail)]
    mpv.mpv_dash_source_get_range_capability.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(RangeCapability)]
    mpv.mpv_dash_source_get_frame_status.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(FrameStatus)]
    mpv.mpv_set_property.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
    mpv.mpv_observe_property.argtypes = [
        ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int]
    mpv.mpv_acquire_d3d11_composition_surface.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(CompositionSurface)]
    mpv.mpv_release_d3d11_composition_surface.argtypes = [
        ctypes.POINTER(CompositionSurface)]
    mpv.mpv_release_d3d11_composition_surface.restype = None
    mpv.mpv_wait_event.argtypes = [ctypes.c_void_p, ctypes.c_double]
    mpv.mpv_wait_event.restype = ctypes.POINTER(Event)
    mpv.mpv_command.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
    mpv.mpv_get_property_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    mpv.mpv_get_property_string.restype = ctypes.c_void_p
    mpv.mpv_free.argtypes = [ctypes.c_void_p]
    mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]


@contextlib.contextmanager
def client(mpv, overrides=None):
    if os.getenv("DASH_TEST_DEBUG"):
        print("[dash] create core", flush=True)
    handle = mpv.mpv_create()
    if not handle:
        raise AssertionError("mpv_create failed")
    try:
        settings = {
            "config": "no", "load-scripts": "no", "terminal": "no",
            "ytdl": "no",
            "vo": "null", "ao": "null", "pause": "yes",
            "network-timeout": "6", "stream-buffer-size": "128KiB",
            "curl-buffer-size": "32KiB", "demuxer-readahead-secs": "1",
            "demuxer-max-bytes": "128KiB", "demuxer-max-back-bytes": "128KiB",
            "keep-open": "yes", "hwdec": "no",
        }
        settings.update(overrides or {})
        for name, value in settings.items():
            result = mpv.mpv_set_option_string(
                handle, name.encode(), value.encode())
            if result < 0:
                raise AssertionError(f"Option {name} rejected: {result}")
        if mpv.mpv_request_log_messages(handle, b"trace") < 0:
            raise AssertionError("Could not subscribe to log messages")
        if mpv.mpv_initialize(handle) < 0:
            raise AssertionError("Could not initialize mpv")
        if os.getenv("DASH_TEST_DEBUG"):
            print("[dash] initialized core", flush=True)
        yield handle
    finally:
        if os.getenv("DASH_TEST_DEBUG"):
            print("[dash] terminate core", flush=True)
        worker = threading.Thread(
            target=mpv.mpv_terminate_destroy, args=(handle,), daemon=True)
        worker.start()
        worker.join(timeout=8)
        if worker.is_alive():
            raise AssertionError("Native DASH termination exceeded 8 seconds")
        if os.getenv("DASH_TEST_DEBUG"):
            print("[dash] terminated core", flush=True)


def source_for(server):
    scheme = "https" if server.secure else "http"
    origin = f"{scheme}://127.0.0.1:{server.server_port}"
    tracks = []
    for role, codec, dimensions in (
        ("video", b"avc1.42E01E", (160, 90)),
        ("audio", b"mp4a.40.2", (0, 0)),
    ):
        tracks.append(Track(
            ctypes.sizeof(Track), 0,
            f"{origin}/{role}?ticket={MARKER.decode()}".encode(),
            f"{role}/mp4".encode(), codec, 160000, 64, *dimensions,
            Range(), Range(),
        ))
    return Source(
        ctypes.sizeof(Source), VERSION, ALLOW_LOOPBACK, 0,
        *tracks, 18.0, b"mpv-dash-local", b"https://example.invalid/",
    )


def snapshot(mpv, handle):
    state = Status()
    state.struct_size = ctypes.sizeof(Status)
    state.api_version = VERSION
    result = mpv.mpv_dash_source_get_status(handle, ctypes.byref(state))
    if result != 0:
        raise AssertionError(f"Status failed: {result}")
    return state


def failure_detail(mpv, handle):
    detail = FailureDetail()
    detail.struct_size = ctypes.sizeof(FailureDetail)
    detail.api_version = VERSION
    result = mpv.mpv_dash_source_get_failure_detail(
        handle, ctypes.byref(detail))
    if result != 0:
        raise AssertionError(f"Failure detail query failed: {result}")
    return detail


def range_capability(mpv, handle):
    state = RangeCapability()
    state.struct_size = ctypes.sizeof(RangeCapability)
    state.api_version = VERSION
    result = mpv.mpv_dash_source_get_range_capability(
        handle, ctypes.byref(state))
    if result != 0:
        raise AssertionError(f"Range capability failed: {result}")
    return state


def frame_snapshot(mpv, handle):
    state = FrameStatus()
    state.struct_size = ctypes.sizeof(FrameStatus)
    state.api_version = VERSION
    result = mpv.mpv_dash_source_get_frame_status(handle, ctypes.byref(state))
    if result != 0:
        raise AssertionError(f"Frame status failed: {result}")
    return state


def prop(mpv, handle, name):
    ptr = mpv.mpv_get_property_string(handle, name.encode())
    if not ptr:
        return None
    try:
        return ctypes.string_at(ptr).decode(errors="replace")
    finally:
        mpv.mpv_free(ptr)


def command(mpv, handle, *args):
    argv = (ctypes.c_char_p * (len(args) + 1))(
        *(arg.encode() for arg in args), None)
    result = mpv.mpv_command(handle, argv)
    if result < 0:
        raise AssertionError(f"Command {args[0]} failed: {result}")


def await_dual_decoder_ao(mpv, handle, context):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(handle, 0.05).contents
        if event.event_id == LOG_MESSAGE:
            message = ctypes.cast(
                event.data, ctypes.POINTER(LogMessage)).contents
            if MARKER in ctypes.string_at(message.text):
                raise AssertionError("Private media URL appeared in a native log")
        if event.event_id == END_FILE:
            raise AssertionError(f"{context} ended before decoder/AO readiness")
        if (prop(mpv, handle, "video-params/w") == "160" and
            prop(mpv, handle, "audio-params/samplerate") == "48000" and
            prop(mpv, handle, "audio-out-params/samplerate") == "48000"):
            return
    raise AssertionError(f"{context} did not produce dual decoder/AO parameters")


def set_position(mpv, handle, name, value):
    position = ctypes.c_double(value)
    return mpv.mpv_set_property(
        handle, name.encode(), DOUBLE, ctypes.byref(position))


def confirm_position(mpv, handle, target, generation, submit):
    while True:
        event = mpv.mpv_wait_event(handle, 0).contents
        if event.event_id == 0:
            break
        if event.event_id == END_FILE:
            raise AssertionError("Source ended before a control command")
    submit()
    restarted = False
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(handle, 0.02).contents
        if event.event_id in (END_FILE, FILE_LOADED):
            raise AssertionError("Seek ended or reloaded the source")
        if event.event_id == PLAYBACK_RESTART:
            restarted = True
        state = snapshot(mpv, handle)
        assert state.generation == generation
        assert state.phase == BOUND and not state.failure, \
            (state.phase, state.failure, state.failed_track)
        position = prop(mpv, handle, "time-pos")
        if (restarted and prop(mpv, handle, "seeking") == "no" and
            prop(mpv, handle, "eof-reached") == "no" and position is not None and
            abs(float(position) - target) <= 0.4):
            return
    raise AssertionError("Native playback restart did not confirm the seek")


def await_event(mpv, handle, wanted, timeout=12):
    deadline = time.monotonic() + timeout
    event_counts = {}
    recent_logs = []
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(handle, 0.05).contents
        event_counts[event.event_id] = event_counts.get(event.event_id, 0) + 1
        if event.event_id == LOG_MESSAGE:
            message = ctypes.cast(
                event.data, ctypes.POINTER(LogMessage)).contents
            text = ctypes.string_at(message.text)
            if MARKER in text:
                raise AssertionError("Private media URL appeared in a native log")
            if os.getenv("DASH_TEST_DEBUG"):
                safe = re.sub(r"\S+://\S+", "[uri]",
                              text.decode(errors="replace").strip())
                recent_logs.append(safe[:160])
                recent_logs = recent_logs[-25:]
        if event.event_id in (FILE_LOADED, END_FILE):
            if event.event_id != wanted:
                raise AssertionError(
                    f"Unexpected event {event.event_id}; "
                    f"phase={snapshot(mpv, handle).phase}, "
                    f"failure={snapshot(mpv, handle).failure}"
                )
            return event
        if wanted == END_FILE and snapshot(mpv, handle).phase == FAILED:
            # The core's terminal event should follow the status promptly.
            continue
    state = snapshot(mpv, handle)
    if os.getenv("DASH_TEST_DEBUG"):
        result = []

        def get_core_status():
            result.append(prop(mpv, handle, "pause"))

        worker = threading.Thread(target=get_core_status, daemon=True)
        worker.start()
        worker.join(2)
        print(f"[dash] timeout events={event_counts}, "
              f"core_responsive={not worker.is_alive()}, "
              f"pause={result[0] if result else None}", flush=True)
        print("[dash] latest sanitized logs=" + repr(recent_logs), flush=True)
    raise AssertionError(f"Timed out awaiting event {wanted}; "
                         f"phase={state.phase}, failure={state.failure}")


def assert_private(mpv, handle):
    for name in ("path", "stream-open-filename", "audio-files",
                 "track-list/0/external-filename"):
        value = prop(mpv, handle, name)
        if value and MARKER.decode() in value:
            raise AssertionError(f"Property {name} exposed media URL")


def assert_headers(server, roles=("video", "audio")):
    for role in roles:
        requests = server.requests_for(role)
        if not requests:
            raise AssertionError(f"No HTTP Range response for {role}")
        for entry in requests:
            if entry[3] != "mpv-dash-local" or \
                    entry[4] != "https://example.invalid/" or entry[5]:
                raise AssertionError(f"Unexpected {role} request headers")


def test_invalid(mpv, server):
    with client(mpv) as handle:
        assert snapshot(mpv, handle).phase == IDLE
        empty_failure = failure_detail(mpv, handle)
        assert not empty_failure.source_generation
        assert empty_failure.origin == ORIGIN_NONE
        bad_detail = FailureDetail()
        bad_detail.struct_size = 0
        bad_detail.api_version = VERSION
        assert mpv.mpv_dash_source_get_failure_detail(
            handle, ctypes.byref(bad_detail)) == INVALID
        bad_detail.struct_size = ctypes.sizeof(FailureDetail)
        bad_detail.api_version = VERSION + 1
        assert mpv.mpv_dash_source_get_failure_detail(
            handle, ctypes.byref(bad_detail)) == INVALID
        empty_range = range_capability(mpv, handle)
        assert not empty_range.source_generation
        assert not empty_range.video_validated_206
        assert not empty_range.audio_validated_206
        bad_range = RangeCapability()
        bad_range.struct_size = 0
        bad_range.api_version = VERSION
        assert mpv.mpv_dash_source_get_range_capability(
            handle, ctypes.byref(bad_range)) == INVALID
        bad_range.struct_size = ctypes.sizeof(RangeCapability)
        bad_range.api_version = VERSION + 1
        assert mpv.mpv_dash_source_get_range_capability(
            handle, ctypes.byref(bad_range)) == INVALID
        empty_frame = frame_snapshot(mpv, handle)
        assert not empty_frame.source_generation
        assert not empty_frame.presented_frame_serial
        bad_frame = FrameStatus()
        bad_frame.struct_size = 0
        bad_frame.api_version = VERSION
        assert mpv.mpv_dash_source_get_frame_status(
            handle, ctypes.byref(bad_frame)) == INVALID
        bad_frame.struct_size = ctypes.sizeof(FrameStatus)
        bad_frame.api_version = VERSION + 1
        assert mpv.mpv_dash_source_get_frame_status(
            handle, ctypes.byref(bad_frame)) == INVALID
        source = source_for(server)
        source.api_version = VERSION + 1
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.video.struct_size = 0
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.audio.url = None
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.video.url = b"http://localhost:99/fake"
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source.video.url = b"https://user@example.invalid/fake"
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.user_agent = b"safe\r\nCookie: fake"
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.referer = b"http://127.0.0.1/"
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source = source_for(server)
        source.video.flags = SEGMENT_BASE
        source.video.initialization = Range(-1, 20)
        source.video.index = Range(21, 40)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source.video.initialization = Range(10, 20)
        source.video.index = Range(20, 40)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        source.video.index = Range(21, 40)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == UNSUPPORTED
        source.video.flags = 0
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == INVALID
        assert snapshot(mpv, handle).generation == 0
    assert not server.requests


def test_playback(mpv, server):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        source.video.url = b"https://example.invalid/replaced"
        source.audio.url = b"https://example.invalid/replaced"
        source.user_agent = b"replaced"
        source.referer = b"https://example.invalid/replaced"
        gc.collect()
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == UNSUPPORTED
        await_event(mpv, handle, FILE_LOADED)
        state = snapshot(mpv, handle)
        assert state.phase == BOUND, (state.phase, state.failure)
        detail = failure_detail(mpv, handle)
        assert detail.source_generation == state.generation
        assert detail.origin == ORIGIN_NONE
        frame = frame_snapshot(mpv, handle)
        assert frame.source_generation == state.generation
        assert not frame.presented_frame_serial, \
            "FILE_LOADED on a headless VO cannot mean Composition first frame"
        assert state.video_http_status == state.audio_http_status == 206
        ranged = range_capability(mpv, handle)
        assert ranged.source_generation == state.generation
        assert ranged.video_validated_206 == ranged.audio_validated_206 == 1
        assert prop(mpv, handle, "vid") not in (None, "no")
        assert prop(mpv, handle, "aid") not in (None, "no")
        assert_private(mpv, handle)
        command(mpv, handle, "seek", "8", "absolute+exact")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            mpv.mpv_wait_event(handle, 0.05)
            pos = prop(mpv, handle, "time-pos")
            if pos and abs(float(pos) - 8) <= 0.25:
                break
        else:
            raise AssertionError("Typed dual-track seek failed")
        if os.getenv("DASH_TEST_DEBUG"):
            names = ("video-params/w", "audio-params/samplerate",
                     "audio-out-params/samplerate")
            print("[dash] decoder properties=" +
                  repr({name: prop(mpv, handle, name) for name in names}),
                  flush=True)
        command(mpv, handle, "set", "pause", "no")
        await_dual_decoder_ao(mpv, handle, "Dual-206 initial seek")
        command(mpv, handle, "set", "pause", "yes")
        for name, value, expected in (
            ("time-pos", 4.0, 4.0),
            ("percent-pos", 50.0, 9.0),
            ("playback-time", 2.0, 2.0),
        ):
            assert set_position(mpv, handle, name, value) == 0, name
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                if event.event_id == END_FILE:
                    raise AssertionError(f"{name} ended dual-206 playback")
                position = prop(mpv, handle, "time-pos")
                if position and abs(float(position) - expected) <= 0.4:
                    break
            else:
                raise AssertionError(f"{name} did not seek both 206 tracks")
            assert range_capability(mpv, handle).audio_validated_206 == 1
            assert snapshot(mpv, handle).phase == BOUND
        command(mpv, handle, "set", "pause", "no")
        await_dual_decoder_ao(mpv, handle, "Dual-206 property seeks")
        assert not frame_snapshot(mpv, handle).presented_frame_serial
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        assert snapshot(mpv, handle).phase == STOPPED
        stopped_range = range_capability(mpv, handle)
        assert stopped_range.source_generation == state.generation
        assert not stopped_range.video_validated_206
        assert not stopped_range.audio_validated_206
    assert_headers(server)
    return state.generation


def test_full_body_playback(mpv, server, full_role, http_status=200,
                            options=None, expected_range="bytes=0-"):
    server.full_body_roles.add(full_role)
    server.full_body_status[full_role] = http_status
    with client(mpv, options) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        state = snapshot(mpv, handle)
        ranged = range_capability(mpv, handle)
        assert state.phase == BOUND and not state.failure
        assert failure_detail(mpv, handle).origin == ORIGIN_NONE
        assert server.requests_for(full_role)[0][2] == http_status
        assert ranged.source_generation == state.generation
        for role, observed in (("video", ranged.video_validated_206),
                               ("audio", ranged.audio_validated_206)):
            assert observed == any(r[2] == 206 for r in server.requests_for(role))
        command(mpv, handle, "set", "pause", "no")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.05).contents
            if event.event_id == LOG_MESSAGE:
                message = ctypes.cast(
                    event.data, ctypes.POINTER(LogMessage)).contents
                if MARKER in ctypes.string_at(message.text):
                    raise AssertionError("Private media URL appeared in a native log")
            if event.event_id == END_FILE:
                raise AssertionError("Full-body MP4 ended before both tracks decoded")
            if (prop(mpv, handle, "video-params/w") == "160" and
                prop(mpv, handle, "audio-params/samplerate") == "48000" and
                prop(mpv, handle, "audio-out-params/samplerate") == "48000"):
                break
        else:
            raise AssertionError("Full-body MP4 did not decode both tracks and AO")
        assert_private(mpv, handle)
        command(mpv, handle, "set", "pause", "yes")
        assert prop(mpv, handle, "seekable") == "yes", \
            "Advertised range capability must not require prior 206 on both tracks"
        confirm_position(mpv, handle, 8, state.generation,
                         lambda: command(mpv, handle, "seek", "8", "absolute+exact"))
        for name, value, target in (
            ("time-pos", 4.0, 4.0),
            ("percent-pos", 50.0, 9.0),
            ("playback-time", 2.0, 2.0),
        ):
            def submit():
                assert set_position(mpv, handle, name, value) == 0, name
            confirm_position(mpv, handle, target, state.generation, submit)
            assert prop(mpv, handle, "pause") == "yes"
        assert snapshot(mpv, handle).phase == BOUND
        command(mpv, handle, "set", "pause", "no")
        await_dual_decoder_ao(mpv, handle, "Mixed response seek")
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        assert snapshot(mpv, handle).phase == STOPPED
        stopped_range = range_capability(mpv, handle)
        assert stopped_range.source_generation == state.generation
        assert not stopped_range.video_validated_206
        assert not stopped_range.audio_validated_206
    assert server.requests_for(full_role)[0][7] == expected_range
    assert_headers(server)


def test_unseekable_track(mpv, server, role):
    server.full_body_roles.add(role)
    server.no_range_advertisement.add(role)
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        state = snapshot(mpv, handle)
        assert state.phase == BOUND and not state.failure
        assert prop(mpv, handle, "seekable") == "no"
        before = len(server.requests)
        argv = (ctypes.c_char_p * 4)(b"seek", b"8", b"absolute+exact", None)
        assert mpv.mpv_command(handle, argv) < 0
        for name in ("time-pos", "percent-pos", "playback-time"):
            assert set_position(mpv, handle, name, 8.0) < 0
        until = time.monotonic() + 0.2
        while time.monotonic() < until:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            assert event.event_id not in (SEEK_EVENT, FILE_LOADED, END_FILE)
        assert len(server.requests) == before
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        assert snapshot(mpv, handle).phase == STOPPED


def test_control_lifecycle(mpv, server):
    server.full_body_roles.add("audio")
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        generation = snapshot(mpv, handle).generation
        for name, value in (("volume", "37"), ("mute", "yes"), ("speed", "1.5")):
            command(mpv, handle, "set", name, value)
        for target in (12, 3, 14, 1):
            confirm_position(mpv, handle, target, generation,
                             lambda: command(mpv, handle, "seek", str(target),
                                             "absolute+exact"))
            assert prop(mpv, handle, "pause") == "yes"
            assert float(prop(mpv, handle, "volume")) == 37
            assert prop(mpv, handle, "mute") == "yes"
            assert float(prop(mpv, handle, "speed")) == 1.5
        confirm_position(mpv, handle, 4, generation,
                         lambda: command(mpv, handle, "seek", "3", "relative+exact"))
        command(mpv, handle, "set", "mute", "no")
        command(mpv, handle, "set", "speed", "1")
        confirm_position(mpv, handle, 17, generation,
                         lambda: command(mpv, handle, "seek", "17", "absolute+exact"))
        command(mpv, handle, "set", "pause", "no")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            assert event.event_id not in (FILE_LOADED, END_FILE)
            if prop(mpv, handle, "eof-reached") == "yes":
                break
        else:
            raise AssertionError("Healthy natural EOF was not observable")
        assert snapshot(mpv, handle).phase == BOUND
        confirm_position(mpv, handle, 0, generation,
                         lambda: command(mpv, handle, "seek", "0", "absolute+exact"))
        command(mpv, handle, "set", "pause", "no")
        await_dual_decoder_ao(mpv, handle, "Same-instance EOF Replay")
        assert float(prop(mpv, handle, "volume")) == 37
        assert snapshot(mpv, handle).generation == generation
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        assert snapshot(mpv, handle).phase == STOPPED
        before = len(server.requests)
        argv = (ctypes.c_char_p * 4)(b"seek", b"0", b"absolute+exact", None)
        assert mpv.mpv_command(handle, argv) < 0
        assert len(server.requests) == before
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == UNSUPPORTED
    assert_headers(server)


def test_observed_seek_capability(mpv, server):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        audio = prop(mpv, handle, "aid")
        assert audio not in (None, "no")
        assert mpv.mpv_observe_property(handle, 701, b"seekable", 3) == 0

        def expect(expected):
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.02).contents
                if event.event_id == 22 and event.reply_userdata == 701:
                    value = ctypes.cast(event.data, ctypes.POINTER(EventProperty)).contents
                    if value.format == 3 and value.data:
                        if ctypes.cast(value.data, ctypes.POINTER(ctypes.c_int)).contents.value == expected:
                            assert prop(mpv, handle, "seekable") == ("yes" if expected else "no")
                            return
            raise AssertionError(f"seekable observer did not publish {expected}")

        expect(1)
        command(mpv, handle, "set", "aid", "no")
        expect(0)
        assert set_position(mpv, handle, "time-pos", 4) < 0
        command(mpv, handle, "set", "aid", audio)
        expect(1)
        generation = snapshot(mpv, handle).generation
        confirm_position(mpv, handle, 4, generation,
                         lambda: command(mpv, handle, "seek", "4", "absolute+exact"))


def test_repeated_zero_response(mpv, server):
    server.full_body_after_first_zero.add("audio")
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        generation = snapshot(mpv, handle).generation
        assert range_capability(mpv, handle).audio_validated_206 == 1
        assert server.requests_for("audio")[0][1:3] == (0, 206)
        command(mpv, handle, "audio-reload")
        command(mpv, handle, "set", "pause", "no")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.05).contents
            if event.event_id == END_FILE:
                raise AssertionError("Reloaded audio ended before dual-track output")
            if (any(r[1:3] == (0, 200)
                    for r in server.requests_for("audio")[1:]) and
                snapshot(mpv, handle).phase == BOUND and
                prop(mpv, handle, "audio-params/samplerate") == "48000" and
                prop(mpv, handle, "audio-out-params/samplerate") == "48000"):
                break
        else:
            state = snapshot(mpv, handle)
            responses = [(r[1], r[2]) for r in server.requests_for("audio")]
            raise AssertionError(
                "Repeated zero-offset audio 200 was not played: "
                f"phase={state.phase}, failure={state.failure}, "
                f"audio_http={state.audio_http_status}, "
                f"audio_responses={responses}")
        assert snapshot(mpv, handle).generation == generation
        assert snapshot(mpv, handle).audio_http_status in (200, 206)
        assert prop(mpv, handle, "audio-out-params/samplerate") == "48000"
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
    assert_headers(server)


def test_generic(mpv, server):
    with client(mpv) as handle:
        command(mpv, handle, "loadfile",
                f"http://127.0.0.1:{server.server_port}/video")
        await_event(mpv, handle, FILE_LOADED)
        assert snapshot(mpv, handle).phase == IDLE
        assert not frame_snapshot(mpv, handle).source_generation
        assert prop(mpv, handle, "vid") not in (None, "no")
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        assert snapshot(mpv, handle).generation == 0


def test_invalid_alias(mpv, server):
    for alias in ("dash://unknown", "DASH://unknown",
                  "dash://video?unexpected"):
        with client(mpv) as handle:
            command(mpv, handle, "loadfile", alias)
            event = await_event(mpv, handle, END_FILE)
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == END_ERROR
            assert snapshot(mpv, handle).phase == IDLE
            command(mpv, handle, "stop")
    assert not server.requests


def test_short_first_range(mpv, server, role):
    server.short_initial_roles.add(role)
    server.hold_followup_role = role
    server.release_followup_headers.clear()
    with client(mpv) as handle:
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 8
            while not server.followup_requested.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                if event.event_id == END_FILE:
                    raise AssertionError("First valid short 206 rejected before followup")
            assert server.followup_requested.is_set(), "No short-range continuation"
            state = snapshot(mpv, handle)
            assert (state.video_http_status if role == "video" else
                    state.audio_http_status) == 206
            time.sleep(0.1)
            assert snapshot(mpv, handle).failure == 0
        finally:
            server.release_followup_headers.set()
        await_event(mpv, handle, FILE_LOADED)
        state = snapshot(mpv, handle)
        assert state.phase == BOUND
        assert (state.video_responses if role == "video" else
                state.audio_responses) >= 2
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)


def test_stop_before_short_continuation(mpv, server, role):
    server.short_initial_roles.add(role)
    server.pause_first_body_role = role
    server.release_first_body.clear()
    with client(mpv) as handle:
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 10
            while not server.first_body_held.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                if event.event_id in (FILE_LOADED, END_FILE):
                    raise AssertionError("Playback advanced before short 206 body was held")
            assert server.first_body_held.is_set()
            initial = snapshot(mpv, handle)
            assert (initial.video_http_status if role == "video" else
                    initial.audio_http_status) == 206
            assert len(server.requests_for(role)) == 1
            command(mpv, handle, "stop")
            stopped = snapshot(mpv, handle)
            assert stopped.phase == STOPPED and stopped.failure == 0
            with server.lock:
                count_at_stop = len(server.requests)
            server.release_first_body.set()
            event = await_event(mpv, handle, END_FILE)
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == 2
            time.sleep(0.2)
            with server.lock:
                assert len(server.requests) == count_at_stop, \
                    "A new media GET began after STOPPED"
            after = snapshot(mpv, handle)
            assert after.phase == STOPPED and after.failure == 0
            assert after.video_http_status == stopped.video_http_status
            assert after.audio_http_status == stopped.audio_http_status
            assert after.video_responses == stopped.video_responses
            assert after.audio_responses == stopped.audio_responses
        finally:
            server.release_first_body.set()


def test_exact_on_done_stop(mpv, server, role):
    server.short_initial_roles.add(role)
    track = VIDEO if role == "video" else AUDIO
    with client(mpv) as handle:
        control = mpv.mpv_dash_test_control
        assert control(handle, TEST_ARM, track) == 0
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            mpv.mpv_wait_event(handle, 0)
            assert control(handle, TEST_WAIT, 8000) == 1, \
                "Clean short 206 did not reach on_done continuation branch"
            assert control(handle, TEST_SUCCESS_BRANCHES, 0) == 1
            initial = snapshot(mpv, handle)
            assert (initial.video_http_status if role == "video" else
                    initial.audio_http_status) == 206
            assert len(server.requests_for(role)) == 1

            command(mpv, handle, "stop")
            stopped = snapshot(mpv, handle)
            assert stopped.phase == STOPPED and stopped.failure == 0
            with server.lock:
                count_at_stop = len(server.requests)
            assert control(handle, TEST_RELEASE, 0) == 0
            deadline = time.monotonic() + 8
            while (control(handle, TEST_BLOCKED_CONTINUATIONS, 0) < 1 and
                   time.monotonic() < deadline):
                time.sleep(0.01)
            assert control(handle, TEST_BLOCKED_CONTINUATIONS, 0) >= 1, \
                "Stopped source did not reject on_done's next Range"
            assert control(handle, TEST_ADDED_AFTER_STOP, 0) == 0
            event = await_event(mpv, handle, END_FILE)
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == 2
            time.sleep(0.2)
            with server.lock:
                assert len(server.requests) == count_at_stop, \
                    "Curl issued a GET after on_done resumed against STOPPED"
            after = snapshot(mpv, handle)
            assert after.phase == STOPPED and after.failure == 0
            assert after.video_responses == stopped.video_responses
            assert after.audio_responses == stopped.audio_responses
        finally:
            control(handle, TEST_RELEASE, 0)


def test_stop_before_late_risk(mpv, server):
    server.short_initial_roles.add("video")
    server.hold_followup_role = "video"
    server.release_followup_headers.clear()
    with client(mpv) as handle:
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 10
            while not server.followup_requested.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                assert event.event_id not in (FILE_LOADED, END_FILE)
            assert server.followup_requested.is_set()
            command(mpv, handle, "stop")
            assert snapshot(mpv, handle).phase == STOPPED
            with server.lock:
                count_at_stop = len(server.requests)
            server.deny = "video"
            server.release_followup_headers.set()
            event = await_event(mpv, handle, END_FILE)
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == 2
            time.sleep(0.2)
            with server.lock:
                assert len(server.requests) <= count_at_stop + 1
            state = snapshot(mpv, handle)
            assert state.phase == STOPPED and state.failure == 0
            assert state.video_http_status == 206
            assert state.video_responses == 1
        finally:
            server.release_followup_headers.set()


def test_risk_latched_before_stop(mpv, server):
    server.deny = "video"
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        deadline = time.monotonic() + 10
        while snapshot(mpv, handle).failure != RISK and time.monotonic() < deadline:
            mpv.mpv_wait_event(handle, 0.05)
        assert snapshot(mpv, handle).failure == RISK
        command(mpv, handle, "stop")
        state = snapshot(mpv, handle)
        assert state.phase == FAILED and state.failure == RISK
        assert state.video_http_status == 412
    assert len(server.requests_for("video")) == 1


def test_stop_queued(mpv, server):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        assert snapshot(mpv, handle).phase == QUEUED
        command(mpv, handle, "stop")
        state = snapshot(mpv, handle)
        assert state.phase == STOPPED and state.failure == 0
        deadline = time.monotonic() + 0.3
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.03).contents
            assert event.event_id != FILE_LOADED
        assert snapshot(mpv, handle).phase == STOPPED
    assert not server.requests


def test_stop_while_opening(mpv, server, role):
    if role == "video":
        server.hold_video_body = True
        server.release_video_body.clear()
        blocked = server.video_body_blocked
        release = server.release_video_body
    else:
        server.hold_audio_body = True
        server.release_audio_body.clear()
        blocked = server.audio_body_blocked
        release = server.release_audio_body
    with client(mpv) as handle:
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 10
            while not blocked.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                assert event.event_id not in (FILE_LOADED, END_FILE)
            assert blocked.is_set(), f"{role} opener was not blocked on media"
            command(mpv, handle, "stop")
            event = await_event(mpv, handle, END_FILE)
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == 2
            state = snapshot(mpv, handle)
            assert state.phase == STOPPED and state.failure == 0
            assert state.failed_track == 0
            assert not frame_snapshot(mpv, handle).presented_frame_serial
        finally:
            release.set()


def test_tls_certificate_isolation(mpv, server, certificates, typed):
    options = {
        "tls-verify": "no",
        "tls-cert-file": str(certificates["client"][0]),
        "tls-key-file": str(certificates["client"][1]),
        "curl-enabled": "yes",
    }
    with client(mpv, options) as handle:
        if typed:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        else:
            command(mpv, handle, "loadfile",
                    f"https://127.0.0.1:{server.server_port}/video")
        await_event(mpv, handle, FILE_LOADED)
        if typed:
            assert snapshot(mpv, handle).phase == BOUND
            for role in ("video", "audio"):
                assert server.requests_for(role)
                assert all(not entry[6] for entry in server.requests_for(role))
        else:
            assert any(entry[6] for entry in server.requests_for("video")), \
                "Generic TLS control did not present its configured client certificate"
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)


def test_composition_first_frame(mpv, server, previous_generation,
                                 previous_serial, audio_status=206):
    options = {
        "vo": "gpu-next", "gpu-api": "d3d11", "d3d11-warp": "yes",
        "d3d11-output-mode": "composition",
        "d3d11-composition-size": "160x90",
    }
    with client(mpv, options) as handle:
        assert prop(mpv, handle, "options/d3d11-output-mode") == "composition"
        assert not frame_snapshot(mpv, handle).presented_frame_serial
        source = source_for(server)
        server.hold_video_body = True
        server.release_video_body.clear()
        saw_file_loaded = False
        try:
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            deadline = time.monotonic() + 10
            while not server.video_body_blocked.is_set() and time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                if event.event_id == LOG_MESSAGE:
                    message = ctypes.cast(
                        event.data, ctypes.POINTER(LogMessage)).contents
                    if MARKER in ctypes.string_at(message.text):
                        raise AssertionError("Private media URL appeared in a native log")
                if event.event_id == FILE_LOADED:
                    saw_file_loaded = True
                    assert not frame_snapshot(mpv, handle).presented_frame_serial
                if event.event_id == END_FILE:
                    raise AssertionError("Composition stopped before video media was released")
            if not server.video_body_blocked.is_set():
                raise AssertionError("Fixture did not isolate video header from first media fragment")
            time.sleep(0.1)
            before = frame_snapshot(mpv, handle)
            before_source = snapshot(mpv, handle)
            before_surface = CompositionSurface()
            assert mpv.mpv_acquire_d3d11_composition_surface(
                handle, ctypes.byref(before_surface)) == 0
            if os.getenv("DASH_TEST_DEBUG"):
                print("[dash] held-video pre-Present: " +
                      repr((saw_file_loaded, before_source.phase,
                            bool(before_surface.swapchain),
                            before_surface.epoch,
                            before.presented_frame_serial)), flush=True)
            mpv.mpv_release_d3d11_composition_surface(
                ctypes.byref(before_surface))
            assert before.source_generation > previous_generation
            assert not before.presented_generation
            assert not before.presented_frame_serial
            assert not before.presented_surface_epoch
        finally:
            server.release_video_body.set()
        if not saw_file_loaded:
            await_event(mpv, handle, FILE_LOADED, 20)
        assert server.requests_for("audio")[0][2] == audio_status
        started = time.monotonic()
        resumed = False
        ready = False
        while time.monotonic() - started < 20:
            event = mpv.mpv_wait_event(handle, 0.05).contents
            if event.event_id == LOG_MESSAGE:
                message = ctypes.cast(
                    event.data, ctypes.POINTER(LogMessage)).contents
                if MARKER in ctypes.string_at(message.text):
                    raise AssertionError("Private media URL appeared in a native log")
            if event.event_id == END_FILE:
                raise AssertionError("Native Composition ended before a real frame")
            state = snapshot(mpv, handle)
            frame = frame_snapshot(mpv, handle)
            surface = CompositionSurface()
            result = mpv.mpv_acquire_d3d11_composition_surface(
                handle, ctypes.byref(surface))
            if result != 0:
                raise AssertionError(f"Composition acquire failed: {result}")
            try:
                if (state.phase == BOUND and not state.failure and
                    frame.source_generation == state.generation and
                    frame.presented_generation == state.generation and
                    frame.presented_frame_serial and
                    frame.presented_surface_epoch == surface.epoch and
                    surface.swapchain and
                    prop(mpv, handle, "video-params/w") == "160" and
                    prop(mpv, handle, "audio-out-params/samplerate") == "48000"):
                    ready = True
                    break
            finally:
                mpv.mpv_release_d3d11_composition_surface(
                    ctypes.byref(surface))
                assert not surface.swapchain
            if not resumed and time.monotonic() - started > 2:
                command(mpv, handle, "set", "pause", "no")
                resumed = True
        if not ready:
            frame = frame_snapshot(mpv, handle)
            raise AssertionError(
                "No native Composition video Present with both tracks and "
                f"lease: generation={frame.source_generation}, "
                f"serial={frame.presented_frame_serial}, "
                f"epoch={frame.presented_surface_epoch}"
            )
        assert frame.source_generation > previous_generation
        assert frame.presented_frame_serial > previous_serial
        first_serial = frame.presented_frame_serial
        first_epoch = frame.presented_surface_epoch
        ranged = range_capability(mpv, handle)
        assert ranged.source_generation == frame.source_generation
        assert ranged.video_validated_206 == 1
        assert ranged.audio_validated_206 == any(
            r[2] == 206 for r in server.requests_for("audio"))
        if audio_status >= 200:
            command(mpv, handle, "set", "pause", "no")
            command(mpv, handle, "seek", "8", "absolute+exact")
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                event = mpv.mpv_wait_event(handle, 0.05).contents
                if event.event_id == LOG_MESSAGE:
                    message = ctypes.cast(
                        event.data, ctypes.POINTER(LogMessage)).contents
                    if MARKER in ctypes.string_at(message.text):
                        raise AssertionError("Private media URL appeared in a native log")
                if event.event_id == END_FILE:
                    raise AssertionError("Composition stopped during seek")
                next_frame = frame_snapshot(mpv, handle)
                position = prop(mpv, handle, "time-pos")
                if (next_frame.presented_frame_serial > first_serial and
                    next_frame.presented_surface_epoch == first_epoch and
                    next_frame.presented_generation == frame.source_generation and
                    prop(mpv, handle, "seeking") == "no" and
                    position is not None and abs(float(position) - 8) <= 0.5):
                    break
            else:
                raise AssertionError("Composition frame serial did not increase after seek")
        last_serial = frame_snapshot(mpv, handle).presented_frame_serial
        command(mpv, handle, "stop")
        await_event(mpv, handle, END_FILE)
        stopped = frame_snapshot(mpv, handle)
        assert stopped.source_generation == frame.source_generation
        assert not stopped.presented_frame_serial
        assert not stopped.presented_surface_epoch
        surface = CompositionSurface()
        assert mpv.mpv_acquire_d3d11_composition_surface(
            handle, ctypes.byref(surface)) == 0
        assert not surface.swapchain
        assert surface.epoch > first_epoch
        stopped_range = range_capability(mpv, handle)
        assert not stopped_range.video_validated_206
        assert not stopped_range.audio_validated_206
    assert_headers(server)
    return stopped.source_generation, last_serial


def test_failure(mpv, server, failed_role, expected, expected_http=None,
                 expected_origin=None):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        event = await_event(mpv, handle, END_FILE)
        result = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
        assert result.reason == END_ERROR, result.reason
        state = snapshot(mpv, handle)
        failures = expected if isinstance(expected, tuple) else (expected,)
        assert state.phase == FAILED and state.failure in failures, \
            (state.phase, state.failure, expected)
        detail = failure_detail(mpv, handle)
        assert detail.source_generation == state.generation
        assert detail.failed_track == state.failed_track
        assert detail.origin != ORIGIN_NONE
        if expected_origin is not None:
            assert detail.origin == expected_origin, \
                (detail.origin, expected_origin)
        if state.failed_track == VIDEO:
            assert detail.http_status == state.video_http_status
            assert detail.response_count == state.video_responses
        elif state.failed_track == AUDIO:
            assert detail.http_status == state.audio_http_status
            assert detail.response_count == state.audio_responses
        failed_range = range_capability(mpv, handle)
        assert failed_range.source_generation == state.generation
        assert not failed_range.video_validated_206
        assert not failed_range.audio_validated_206
        assert not frame_snapshot(mpv, handle).presented_frame_serial
        assert state.failed_track == (VIDEO if failed_role == "video" else AUDIO)
        if isinstance(expected, tuple):
            assert not prop(mpv, handle, "video-params/w" if failed_role == "video"
                            else "audio-out-params/samplerate")
        else:
            assert not prop(mpv, handle, "video-params/w")
        if expected_http is not None:
            assert (state.video_http_status if failed_role == "video" else
                    state.audio_http_status) == expected_http
        if expected in (RISK, AUTH) or (expected == HTTP_RANGE and
                                       expected_http == 416):
            assert server.error_headers.is_set()
            assert server.error_body_bytes == 0, "Error body was delivered"
    assert len(server.requests_for(failed_role)) == 1


def test_full_body_rejected(mpv, server, expected_failures, allow_file_loaded=False,
                            expected_origin=None):
    server.full_body_roles.add("audio")
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        deadline = time.monotonic() + 12
        resumed = False
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.05).contents
            if event.event_id == LOG_MESSAGE:
                message = ctypes.cast(
                    event.data, ctypes.POINTER(LogMessage)).contents
                if MARKER in ctypes.string_at(message.text):
                    raise AssertionError("Private media URL appeared in a native log")
            if event.event_id == FILE_LOADED and not allow_file_loaded:
                raise AssertionError("Invalid full-body media reached FILE_LOADED")
            if event.event_id == FILE_LOADED and allow_file_loaded and not resumed:
                command(mpv, handle, "set", "pause", "no")
                resumed = True
            if event.event_id == END_FILE:
                reason = ctypes.cast(
                    event.data, ctypes.POINTER(EndFile)).contents.reason
                assert reason == END_ERROR, reason
                break
        else:
            state = snapshot(mpv, handle)
            raise AssertionError(
                "Invalid full-body media did not terminate: "
                f"phase={state.phase}, failure={state.failure}, "
                f"audio_http={state.audio_http_status}, resumed={resumed}")
        state = snapshot(mpv, handle)
        assert state.phase == FAILED and state.failure in expected_failures, \
            (state.phase, state.failure, expected_failures)
        assert state.failure != HTTP_STATUS, \
            "An ordinary media response was rejected by HTTP status alone"
        detail = failure_detail(mpv, handle)
        assert detail.origin != ORIGIN_NONE
        if expected_origin is not None:
            assert detail.origin == expected_origin, \
                (detail.origin, expected_origin)
        assert state.failed_track == AUDIO
        assert not prop(mpv, handle, "audio-out-params/samplerate")
        ranged = range_capability(mpv, handle)
        assert ranged.source_generation == state.generation
        assert not ranged.video_validated_206
        assert not ranged.audio_validated_206
    assert len(server.requests_for("audio")) == 1


def test_status_syntax_control(mpv, server, expected_origin):
    server.full_body_roles.add("audio")
    server.full_body_payloads["audio"] = b""
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, END_FILE)
        state = snapshot(mpv, handle)
        detail = failure_detail(mpv, handle)
        assert state.phase == FAILED
        assert detail.source_generation == state.generation
        assert detail.failed_track == state.failed_track
        if state.failure == HTTP_STATUS:
            assert detail.origin == expected_origin, \
                (detail.origin, expected_origin)
            return True
        assert detail.origin == ORIGIN_OTHER, \
            (detail.origin, state.failure)
        return False


def test_consumer_window(mpv, tracks, end_offset, length_known=True,
                         code=200, playable=False):
    with serve(tracks) as server:
        server.full_body_roles.add("audio")
        server.full_body_status["audio"] = code
        if not length_known:
            server.full_body_lengths["audio"] = None
        with client(mpv) as handle:
            assert mpv.mpv_dash_test_control(
                handle, TEST_AUDIO_WINDOW, end_offset) == 0
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            if playable:
                await_event(mpv, handle, FILE_LOADED)
                assert snapshot(mpv, handle).phase == BOUND
                assert server.requests_for("audio")[0][2] == code
                if range_capability(mpv, handle).audio_validated_206:
                    assert any(request[2] == 206 for request in server.requests_for("audio"))
                command(mpv, handle, "stop")
                await_event(mpv, handle, END_FILE)
            else:
                await_event(mpv, handle, END_FILE)
                failed = snapshot(mpv, handle)
                assert failed.phase == FAILED
                assert failed.failure == HTTP_RANGE and failed.failed_track == AUDIO
                assert failure_detail(mpv, handle).origin == ORIGIN_WINDOW
                assert failed.audio_http_status == code
                assert not prop(mpv, handle, "audio-out-params/samplerate")
            assert server.requests_for("audio")[0][7] == \
                f"bytes=0-{end_offset - 1}"
        if playable:
            for request in server.requests_for("audio"):
                bounds = re.fullmatch(r"bytes=(\d+)-(\d+)", request[7])
                assert bounds and 0 <= int(bounds[1]) <= int(bounds[2]) < end_offset
        else:
            assert len(server.requests_for("audio")) == 1


def test_late_status(mpv, server, role, code, failure):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        before = len(server.requests_for(role))
        server.arm_failure = role
        if os.getenv("DASH_TEST_DEBUG"):
            print(f"[dash] armed late {role}, prior responses={before}", flush=True)
        command(mpv, handle, "seek", "15", "absolute+exact")
        try:
            event = await_event(mpv, handle, END_FILE, 12)
        except AssertionError:
            if os.getenv("DASH_TEST_DEBUG"):
                requests = [(item[1], item[2])
                            for item in server.requests_for(role)]
                print(f"[dash] late {role} response positions={requests}",
                      flush=True)
            raise
        result = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
        state = snapshot(mpv, handle)
        assert result.reason == END_ERROR
        assert state.phase == FAILED and state.failure == failure, \
            (state.phase, state.failure)
        assert state.failed_track == (VIDEO if role == "video" else AUDIO)
        detail = failure_detail(mpv, handle)
        assert detail.source_generation == state.generation
        assert detail.failed_track == state.failed_track
        assert detail.http_status == code
        assert detail.response_count == (
            state.video_responses if role == "video" else state.audio_responses)
        assert detail.origin == (ORIGIN_OFFSET if code == 200 else ORIGIN_OTHER)
        assert (state.video_http_status if role == "video" else
                state.audio_http_status) == code
        assert len(server.requests_for(role)) > before
        assert server.error_headers.is_set() and server.error_body_bytes == 0
        ranged = range_capability(mpv, handle)
        assert not ranged.video_validated_206
        assert not ranged.audio_validated_206
        count = len(server.requests_for(role))
        time.sleep(0.2)
        assert len(server.requests_for(role)) == count, "Terminal HTTP was retried"
        frozen = (detail.origin, detail.failed_track, detail.http_status,
                  detail.response_count)
        command(mpv, handle, "stop")
        after = failure_detail(mpv, handle)
        assert (after.origin, after.failed_track, after.http_status,
                after.response_count) == frozen, "First-failure reason was overwritten"


def test_truncated_seek_response(mpv, server, role):
    with client(mpv) as handle:
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        await_event(mpv, handle, FILE_LOADED)
        generation = snapshot(mpv, handle).generation
        before = len(server.requests_for(role))
        server.truncate_range_role = role
        command(mpv, handle, "seek", "15", "absolute+exact")
        event = await_event(mpv, handle, END_FILE)
        assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == END_ERROR
        state = snapshot(mpv, handle)
        detail = failure_detail(mpv, handle)
        assert state.generation == generation and state.phase == FAILED
        assert state.failed_track == (VIDEO if role == "video" else AUDIO)
        assert state.failure == TRANSPORT
        assert detail.origin == ORIGIN_BODY_LENGTH
        transfer = [int(value) for value in prop(
            mpv, handle, "rodel-dash-transfer").split(" ")]
        assert len(transfer) == 13 and transfer[0] == 1
        assert transfer[1] == generation and transfer[2] == state.failed_track
        assert transfer[3] == detail.response_count and transfer[4] == 206
        assert transfer[5] != 0 and transfer[6] == 1
        assert transfer[7] > transfer[8] and transfer[11:] == [1, 0]
        first_transfer = transfer[:]
        assert len(server.requests_for(role)) > before
        count = len(server.requests_for(role))
        time.sleep(0.2)
        assert len(server.requests_for(role)) == count, "Truncated response was retried"
        assert [int(value) for value in prop(
            mpv, handle, "rodel-dash-transfer").split(" ")] == first_transfer


def test_interrupted_transfer(mpv, server, role, full_body=True,
                              rejection=None, force_receive_error=False):
    server.disconnect_role = role
    server.disconnect_attempts = 1
    if rejection is not None and rejection not in ("compressed", "duplicate-encoding"):
        server.disconnect_bytes = 1
    if full_body:
        server.full_body_roles.add(role)
    expected_requests = 2
    if rejection == "budget":
        server.disconnect_attempts = 100
        server.disconnect_bytes = 1
        expected_requests = 3
    elif rejection == "unseekable":
        server.no_range_advertisement.add(role)
        expected_requests = 1
    elif rejection == "zero-progress":
        server.disconnect_bytes = 0
        expected_requests = 1
    elif rejection == "unknown-total":
        server.full_body_lengths[role] = None
        expected_requests = 1
    elif rejection in ("compressed", "duplicate-encoding"):
        server.full_body_payloads[role] = gzip.compress(server.tracks[role], mtime=0)
        server.full_body_encodings[role] = "gzip"
        if rejection == "duplicate-encoding":
            server.duplicate_identity_encoding_roles.add(role)
        server.disconnect_bytes = 128
        expected_requests = 1
    elif rejection == "duplicate-range-encoding":
        server.range_encodings[role] = "gzip"
    elif rejection == "wrong-range":
        server.bad_range = role
    elif rejection == "wrong-total":
        server.wrong_total_role = role
    elif isinstance(rejection, int):
        server.arm_failure = role
        server.arm_code = rejection
        server.release_error_body.clear()

    with client(mpv, {"pause": "no", "speed": "4", "keep-open": "no",
                      "curl-max-retries": "2",
                      "demuxer-lavf-o": "use_mfra_for=0"}) as handle:
        if force_receive_error:
            track = VIDEO if role == "video" else AUDIO
            assert mpv.mpv_dash_test_control(handle, TEST_COMPLETE_RECV_ERROR, track) == 0
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        generation = snapshot(mpv, handle).generation
        loaded = False
        max_position = 0.0
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            if event.event_id == LOG_MESSAGE:
                message = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
                assert MARKER not in ctypes.string_at(message.text)
            if event.event_id == FILE_LOADED:
                loaded = True
            if event.event_id == END_FILE:
                reason = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason
                state = snapshot(mpv, handle)
                assert state.generation == generation
                if rejection is None:
                    assert reason == 0 and not state.failure, \
                        ("Recoverable body interruption stopped playback", reason, state.failure)
                    assert loaded and max_position >= 16, max_position
                else:
                    assert reason == END_ERROR and state.phase == FAILED
                    if rejection == 412:
                        assert state.failure == RISK
                    elif rejection in (401, 403):
                        assert state.failure == AUTH
                    elif rejection in (200, "wrong-range", "wrong-total", "duplicate-range-encoding"):
                        assert state.failure == HTTP_RANGE
                    else:
                        assert state.failure == TRANSPORT
                break
            position = prop(mpv, handle, "time-pos")
            if position:
                max_position = max(max_position, float(position))
        else:
            raise AssertionError("Interrupted transfer did not settle within 15 seconds")
        requests = server.requests_for(role)
        if rejection is None:
            assert len(requests) >= expected_requests, \
                [(request[1], request[2]) for request in requests]
        else:
            assert len(requests) == expected_requests, \
                (rejection, [(request[1], request[2]) for request in requests])
        if expected_requests > 1:
            assert requests[1][1] == server.disconnect_bytes, \
                "Continuation did not start at the next received byte"
            assert requests[1][1] > 0, "Recovery reopened the source at zero"
        count = len(server.requests)
        time.sleep(0.2)
        assert len(server.requests) == count, "Requests continued after settlement"
        assert_private(mpv, handle)
    assert_headers(server, roles=tuple(role for role in ("video", "audio")
                                       if server.requests_for(role)))


def test_recovery_interleave(mpv, server, role, action):
    server.full_body_roles.update(("audio", "video"))
    server.disconnect_role = role
    server.disconnect_attempts = 0 if action == "seek" else 1
    server.disconnect_bytes = 8192
    track = VIDEO if role == "video" else AUDIO
    with client(mpv, {"pause": "yes" if action == "seek" else "no", "speed": "4",
                      "demuxer-lavf-o": "use_mfra_for=0"}) as handle:
        control = mpv.mpv_dash_test_control
        if action != "seek":
            assert control(handle, TEST_ARM, track) == 0
        errors = []
        seek = None
        try:
            source = source_for(server)
            assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
            if action == "seek":
                await_event(mpv, handle, FILE_LOADED)
                server.disconnect_attempts = 100
                assert control(handle, TEST_ARM, track) == 0
                command(mpv, handle, "seek", "12", "absolute+exact")
            mpv.mpv_wait_event(handle, 0)
            assert control(handle, TEST_WAIT, 8000) == 1, \
                ("Recovery did not reach its continuation fence",
                 snapshot(mpv, handle).phase, snapshot(mpv, handle).failure,
                 [(request[1], request[2]) for request in server.requests_for(role)])
            count = len(server.requests)
            role_count = len(server.requests_for(role))
            generation = snapshot(mpv, handle).generation
            if action == "stop":
                command(mpv, handle, "stop")
                assert snapshot(mpv, handle).phase == STOPPED
            elif action == "abort":
                assert control(handle, TEST_ABORT_WAITING, 0) == 0
            elif action == "other-risk":
                other = AUDIO if track == VIDEO else VIDEO
                assert control(handle, TEST_OTHER_TRACK_RISK, other) == 0
                assert snapshot(mpv, handle).failure == RISK
            elif action == "seek":
                server.disconnect_attempts = 0
                def request_seek():
                    try:
                        command(mpv, handle, "seek", "1", "absolute+exact")
                    except AssertionError as error:
                        errors.append(error)
                seek = threading.Thread(target=request_seek, daemon=True)
                seek.start()
                time.sleep(0.05)
            assert control(handle, TEST_RELEASE, 0) == 0
            if seek:
                seek.join(8)
                assert not seek.is_alive() and not errors, errors
                confirm_position(mpv, handle, 1, generation,
                                 lambda: command(mpv, handle, "seek", "1", "absolute+exact"))
                assert snapshot(mpv, handle).phase == BOUND
                command(mpv, handle, "stop")
            else:
                deadline = time.monotonic() + 5
                while control(handle, TEST_BLOCKED_CONTINUATIONS, 0) < 1 and time.monotonic() < deadline:
                    time.sleep(0.01)
                assert control(handle, TEST_BLOCKED_CONTINUATIONS, 0) >= 1
                assert control(handle, TEST_ADDED_AFTER_STOP, 0) == 0
                time.sleep(0.2)
                if action == "abort":
                    assert len(server.requests_for(role)) == role_count, \
                        "Aborted stream submitted a new GET"
                else:
                    assert len(server.requests) == count, "Retired recovery submitted a new GET"
                command(mpv, handle, "stop")
        finally:
            control(handle, TEST_RELEASE, 0)
            if seek:
                seek.join(8)


def test_complete_body_receive_error(mpv, server, role):
    server.full_body_roles.update(("audio", "video"))
    track = VIDEO if role == "video" else AUDIO
    with client(mpv, {"pause": "no", "speed": "4"}) as handle:
        assert mpv.mpv_dash_test_control(handle, TEST_COMPLETE_RECV_ERROR, track) == 0
        source = source_for(server)
        assert mpv.mpv_dash_source_load(handle, ctypes.byref(source)) == 0
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(handle, 0.02).contents
            if event.event_id != END_FILE:
                continue
            assert ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents.reason == END_ERROR
            state = snapshot(mpv, handle)
            assert state.phase == FAILED and state.failure == TRANSPORT
            transfer = [int(value) for value in prop(mpv, handle, "rodel-dash-transfer").split()]
            assert transfer[2] == track and transfer[5] == 56
            assert transfer[7] == transfer[8] > 0
            count = len(server.requests_for(role))
            assert count == failure_detail(mpv, handle).response_count
            time.sleep(0.2)
            assert len(server.requests_for(role)) == count, \
                "Complete non-OK response submitted another request"
            return
        raise AssertionError("Complete body with non-OK transfer became a successful EOF")


def main():
    scheduler = len(sys.argv) == 3 and sys.argv[2] == "--scheduler"
    controls = len(sys.argv) == 3 and sys.argv[2] == "--controls"
    recovery = len(sys.argv) == 3 and sys.argv[2] == "--recovery"
    if not (len(sys.argv) == 2 or scheduler or controls or recovery):
        raise SystemExit("usage: python test/libmpv_dash_source.py "
                         "build/local-libmpv/x86_64 [--scheduler|--controls|--recovery]")
    runtime = Path(sys.argv[1]).resolve()
    directory = runtime / f"dash-generated-{os.getpid()}"
    if not runtime.is_dir() or not (runtime / "libmpv-2.dll").is_file():
        raise AssertionError("Locally built x64 runtime directory required")
    directory.mkdir()
    try:
        if os.getenv("DASH_TEST_DEBUG"):
            faulthandler.dump_traceback_later(30)
        print("[dash] generate legal local fMP4 tracks", flush=True)
        tracks = generate_tracks(directory)
        with contextlib.ExitStack() as stack:
            stack.enter_context(os.add_dll_directory(str(runtime)))
            mpv = ctypes.CDLL(str(runtime / "libmpv-2.dll"))
            configure_library(mpv)
            if scheduler:
                if not hasattr(mpv, "mpv_dash_test_control"):
                    raise AssertionError("Test-only scheduler symbol is missing")
                mpv.mpv_dash_test_control.argtypes = [
                    ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
                mpv.mpv_dash_test_control.restype = ctypes.c_int
                for role in ("video", "audio"):
                    print(f"[dash] exact on_done(CURLE_OK) STOPPED {role}",
                          flush=True)
                    with serve(tracks) as server:
                        test_exact_on_done_stop(mpv, server, role)
                    for action in ("stop", "abort", "other-risk", "seek"):
                        print(f"[dash] recovery continuation {role}: {action}", flush=True)
                        with serve(tracks) as server:
                            test_recovery_interleave(mpv, server, role, action)
                    with serve(tracks) as server:
                        test_complete_body_receive_error(mpv, server, role)
                    with serve(tracks) as server:
                        test_interrupted_transfer(mpv, server, role, force_receive_error=True)
                    with serve(tracks) as server:
                        test_interrupted_transfer(mpv, server, role, rejection="unknown-total",
                                                  force_receive_error=True)
                with serve(tracks) as server:
                    test_risk_latched_before_stop(mpv, server)
                    print("[dash] consumer byte-window, no body past end", flush=True)
                    test_consumer_window(mpv, tracks, len(tracks["audio"]) + 16,
                                         playable=True)
                    test_consumer_window(mpv, tracks, 64)
                    test_consumer_window(mpv, tracks, 64, length_known=False)
                    test_consumer_window(mpv, tracks, 64, code=416)
                    print("DASH_TEST_ONLY_EXACT_ON_DONE_INTERLEAVE_PASS")
                    return
            if hasattr(mpv, "mpv_dash_test_control"):
                raise AssertionError("Production DLL exposes a test-only hook")
            assert mpv.mpv_client_api_version() == (2 << 16) | 11
            assert ctypes.sizeof(FailureDetail) == 32
            assert FailureDetail.source_generation.offset == 8
            assert ctypes.sizeof(RangeCapability) == 24
            assert RangeCapability.source_generation.offset == 8
            assert RangeCapability.video_validated_206.offset == 16
            assert RangeCapability.audio_validated_206.offset == 20
            if recovery:
                for role in ("audio", "video"):
                    for full_body in (True, False):
                        print(f"[dash] interrupted {role}, full-body={full_body}", flush=True)
                        with serve(tracks) as server:
                            test_interrupted_transfer(mpv, server, role, full_body)
                    for rejection in ("budget", "unseekable", "zero-progress",
                                      "compressed", "duplicate-encoding", "duplicate-range-encoding", "wrong-range",
                                      "wrong-total", 200, 401, 403, 412):
                        print(f"[dash] recovery rejection {role}: {rejection}", flush=True)
                        with serve(tracks) as server:
                            test_interrupted_transfer(mpv, server, role, rejection=rejection)
                print("DASH_NATIVE_RECOVERY_PASS", flush=True)
                return
            if controls:
                print("[dash] complete same-instance native controls", flush=True)
                with serve(tracks) as server:
                    test_control_lifecycle(mpv, server)
                with serve(tracks) as server:
                    test_observed_seek_capability(mpv, server)
                for role in ("video", "audio"):
                    print(f"[dash] {role} 200 advertisement and direct seek", flush=True)
                    with serve(tracks) as server:
                        test_full_body_playback(mpv, server, role)
                    print(f"[dash] {role} genuinely nonseekable admission", flush=True)
                    with serve(tracks) as server:
                        test_unseekable_track(mpv, server, role)
                    with serve(tracks) as server:
                        test_truncated_seek_response(mpv, server, role)
                for role, code, failure in (
                    ("video", 412, RISK), ("audio", 412, RISK),
                    ("video", 401, AUTH), ("audio", 403, AUTH),
                    ("video", 200, HTTP_RANGE), ("audio", 200, HTTP_RANGE),
                ):
                    with serve(tracks) as server:
                        server.arm_code = code
                        test_late_status(mpv, server, role, code, failure)
                print("DASH_NATIVE_CONTROL_LIFECYCLE_PASS", flush=True)
                return
            print("[dash] invalid descriptor", flush=True)
            with serve(tracks) as server:
                test_invalid(mpv, server)
            print("[dash] invalid DASH alias fails without native abort", flush=True)
            with serve(tracks) as server:
                test_invalid_alias(mpv, server)
            print("[dash] dual track and seek", flush=True)
            with serve(tracks) as server:
                previous_generation = test_playback(mpv, server)
            for role in ("audio", "video"):
                print(f"[dash] first full-body MP4 200 {role}, direct seek", flush=True)
                with serve(tracks) as server:
                    test_full_body_playback(mpv, server, role)
                with serve(tracks) as server:
                    test_unseekable_track(mpv, server, role)
            with serve(tracks) as server:
                test_control_lifecycle(mpv, server)
            with serve(tracks) as server:
                test_observed_seek_capability(mpv, server)
            print("[dash] ordinary response media, absent length, finite hint", flush=True)
            with serve(tracks) as server:
                server.full_body_lengths["audio"] = None
                test_full_body_playback(mpv, server, "audio")
            with serve(tracks) as server:
                test_full_body_playback(mpv, server, "audio",
                                        options={"curl-max-request-size": "16KiB"},
                                        expected_range="bytes=0-16383")
            with serve(tracks) as server:
                server.full_body_lengths["audio"] = None
                test_full_body_playback(mpv, server, "audio",
                                        options={"curl-max-request-size": "16KiB"},
                                        expected_range="bytes=0-16383")
            with serve(tracks) as server:
                server.full_body_with_range.add("audio")
                test_full_body_playback(mpv, server, "audio")
            with serve(tracks) as server:
                server.full_body_types["audio"] = "text/html"
                test_full_body_playback(mpv, server, "audio")
            for code in (404, 500, 416):
                with serve(tracks) as server:
                    test_full_body_playback(mpv, server, "audio", http_status=code)
            print("[dash] repeated zero-offset media response", flush=True)
            with serve(tracks) as server:
                test_repeated_zero_response(mpv, server)
            print("[dash] existing generic HTTP path", flush=True)
            with serve(tracks) as server:
                test_generic(mpv, server)
            for role in ("video", "audio"):
                print(f"[dash] short first 206 {role} still opens", flush=True)
                with serve(tracks) as server:
                    test_short_first_range(mpv, server, role)
            for role in ("video", "audio"):
                print(f"[dash] stop before short 206 {role} continuation", flush=True)
                with serve(tracks) as server:
                    test_stop_before_short_continuation(mpv, server, role)
            print("[dash] stop before late 412 keeps STOPPED", flush=True)
            with serve(tracks) as server:
                test_stop_before_late_risk(mpv, server)
            print("[dash] 412 observed before stop stays latched", flush=True)
            with serve(tracks) as server:
                test_risk_latched_before_stop(mpv, server)
            print("[dash] queued stop remains stopped", flush=True)
            with serve(tracks) as server:
                test_stop_queued(mpv, server)
            for role in ("video", "audio"):
                print(f"[dash] stop during required {role} open", flush=True)
                with serve(tracks) as server:
                    test_stop_while_opening(mpv, server, role)
            print("[dash] 103 before two final 206 responses", flush=True)
            with serve(tracks) as server:
                server.early_hints_roles.update(("video", "audio"))
                test_playback(mpv, server)
            print("[dash] 103 before terminal 412 still stops", flush=True)
            with serve(tracks) as server:
                server.early_hints_roles.add("video")
                server.deny = "video"
                test_failure(mpv, server, "video", RISK)
            print("[dash] numeric first-failure status branches", flush=True)
            with serve(tracks) as server:
                server.interim_count_roles["audio"] = 9
                test_failure(mpv, server, "audio", HTTP_STATUS,
                             expected_origin=ORIGIN_INTERIM)
            for name, expected in (
                ("malformed_status_roles", ORIGIN_MALFORMED),
                ("duplicate_final_roles", ORIGIN_DUPLICATE),
            ):
                with serve(tracks) as server:
                    getattr(server, name).add("audio")
                    observed = test_status_syntax_control(mpv, server, expected)
                    print(f"[dash] {name} exposed={int(observed)}", flush=True)
            print("[dash] TLS client certificate isolation", flush=True)
            certificates = generate_tls_certificates(directory)
            with serve(tracks, certificates) as server:
                test_tls_certificate_isolation(mpv, server, certificates, True)
            with serve(tracks, certificates) as server:
                test_tls_certificate_isolation(mpv, server, certificates, False)
            print("[dash] D3D11 Composition real Present/lease", flush=True)
            with serve(tracks) as server:
                source_generation, presented_serial = test_composition_first_frame(
                    mpv, server, previous_generation, 0)
            print("[dash] new source cannot reuse prior Present serial", flush=True)
            with serve(tracks) as server:
                test_composition_first_frame(
                    mpv, server, source_generation, presented_serial)
            print("[dash] full-body audio 200 with D3D11 Present and AO", flush=True)
            with serve(tracks) as server:
                server.full_body_roles.add("audio")
                test_composition_first_frame(
                    mpv, server, source_generation, presented_serial,
                    audio_status=200)
            print("[dash] ordinary-code media with D3D11 Present and AO", flush=True)
            for code in (404, 500):
                with serve(tracks) as server:
                    server.full_body_roles.add("audio")
                    server.full_body_status["audio"] = code
                    test_composition_first_frame(
                        mpv, server, source_generation, presented_serial,
                        audio_status=code)
            print("[dash] full-body 200 HTML rejected before Ready", flush=True)
            for code in (200, 404, 500):
                for mime in ("text/html", "audio/mp4"):
                    with serve(tracks) as server:
                        server.full_body_status["audio"] = code
                        server.full_body_types["audio"] = mime
                        server.full_body_payloads["audio"] = \
                            b"<!doctype html><html>no media</html>"
                        test_full_body_rejected(
                            mpv, server, (2, 8, 9, TRANSPORT))
                with serve(tracks) as server:
                    server.full_body_status["audio"] = code
                    server.full_body_lengths["audio"] = None
                    server.full_body_payloads["audio"] = b""
                    test_full_body_rejected(mpv, server, (2, 8, 9, TRANSPORT))
            print("[dash] full-body 200 invalid Content-Length", flush=True)
            with serve(tracks) as server:
                server.content_length_conflict_roles.add("audio")
                test_full_body_rejected(mpv, server, (TRANSPORT,),
                                        expected_origin=ORIGIN_LENGTH)
            for length in ("0", "-1", "invalid", str(1 << 70)):
                with serve(tracks) as server:
                    server.full_body_lengths["audio"] = length
                    test_full_body_rejected(mpv, server, (2, 8, 9, TRANSPORT))
            with serve(tracks) as server:
                server.full_body_lengths["audio"] = None
                server.full_body_payloads["audio"] = b""
                test_full_body_rejected(mpv, server, (2, 8, 9, TRANSPORT))
            with serve(tracks) as server:
                server.full_body_encodings["audio"] = "gzip"
                test_full_body_rejected(mpv, server, (2, 8, 9, TRANSPORT))
            print("[dash] full-body 200 short body fails visibly", flush=True)
            with serve(tracks) as server:
                # Consume the short response instead of intentionally abandoning
                # it for a healthy index range before its EOF is observable.
                server.no_range_advertisement.add("audio")
                server.full_body_payloads["audio"] = tracks["audio"][:len(tracks["audio"]) // 2]
                server.full_body_lengths["audio"] = len(tracks["audio"])
                test_full_body_rejected(
                    mpv, server, (7, 2, 8, 9), allow_file_loaded=True)
            for role in ("video", "audio"):
                print(f"[dash] active {role} seek response truncation is terminal", flush=True)
                with serve(tracks) as server:
                    test_truncated_seek_response(mpv, server, role)
            print("[dash] required audio 412", flush=True)
            with serve(tracks) as server:
                server.deny = "audio"
                test_failure(mpv, server, "audio", RISK)
            print("[dash] required audio 404", flush=True)
            with serve(tracks) as server:
                server.deny = "audio"
                server.deny_code = 404
                test_failure(mpv, server, "audio", (2, 8, 9, TRANSPORT),
                             expected_http=404)
            print("[dash] video auth 401", flush=True)
            with serve(tracks) as server:
                server.deny = "video"
                server.deny_code = 401
                test_failure(mpv, server, "video", AUTH)
            print("[dash] malformed Content-Range", flush=True)
            with serve(tracks) as server:
                server.bad_range = "video"
                test_failure(mpv, server, "video", HTTP_RANGE,
                             expected_origin=ORIGIN_PARTIAL)
            print("[dash] malformed audio Content-Range", flush=True)
            with serve(tracks) as server:
                server.bad_range = "audio"
                test_failure(mpv, server, "audio", HTTP_RANGE,
                             expected_origin=ORIGIN_PARTIAL)
            for role in ("video", "audio"):
                print(f"[dash] {role} unbounded HTTP 416 is media candidate",
                      flush=True)
                with serve(tracks) as server:
                    server.deny = role
                    server.deny_code = 416
                    test_failure(mpv, server, role, (1, 2, 8, 9, TRANSPORT),
                                 expected_http=416)
            print("[dash] no redirect", flush=True)
            with serve(tracks) as server:
                server.redirect = "video"
                test_failure(mpv, server, "video", HTTP_STATUS,
                             expected_origin=ORIGIN_REDIRECT)
                assert server.redirect_hits == 0
            for role, code, failure in (
                ("video", 412, RISK), ("audio", 412, RISK),
                ("audio", 403, AUTH),
                ("audio", 200, HTTP_RANGE),
                ("video", 200, HTTP_RANGE),
            ):
                print(f"[dash] late {role} {code}", flush=True)
                with serve(tracks) as server:
                    server.arm_code = code
                    test_late_status(mpv, server, role, code, failure)
        print("DASH ABI: validation, dual track, seek, required audio, "
              "range, redirect, repeated 412, native Composition first "
              "Present/lease, stop/lifetime PASS")
    finally:
        shutil.rmtree(directory)


if __name__ == "__main__":
    main()
