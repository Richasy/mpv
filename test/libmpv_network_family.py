#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Prove that prefer_family orders dual-stack HTTP/TLS connections per file."""

import contextlib
import ctypes
import http.server
import io
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import wave

BASE_LAVF_OPTIONS = "reconnect=0,reconnect_on_network_error=0"


class Event(ctypes.Structure):
    _fields_ = [
        ("event_id", ctypes.c_int),
        ("error", ctypes.c_int),
        ("reply_userdata", ctypes.c_uint64),
        ("data", ctypes.c_void_p),
    ]


class EndFile(ctypes.Structure):
    _fields_ = [("reason", ctypes.c_int), ("error", ctypes.c_int)]


class LogMessage(ctypes.Structure):
    _fields_ = [
        ("prefix", ctypes.c_char_p),
        ("level", ctypes.c_char_p),
        ("text", ctypes.c_char_p),
        ("log_level", ctypes.c_int),
    ]


def wait_for(mpv, client, wanted, messages, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        event = mpv.mpv_wait_event(client, 0.25).contents
        if event.event_id == 2:  # MPV_EVENT_LOG_MESSAGE
            message = ctypes.cast(event.data, ctypes.POINTER(LogMessage)).contents
            messages.append(message.text.decode("utf-8", errors="replace")[:500])
            del messages[:-12]
        elif event.event_id == 8:  # MPV_EVENT_FILE_LOADED
            if wanted == "loaded":
                return None
            raise RuntimeError("The file loaded although the failure was expected")
        elif event.event_id == 7:  # MPV_EVENT_END_FILE
            end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
            if wanted == "loaded":
                raise RuntimeError(
                    f"Unexpected end-file: reason={end.reason}, error={end.error}\n"
                    + "".join(messages))
            return end
    raise TimeoutError(f"Native media did not reach {wanted}\n" + "".join(messages))


def run_client(library, url, preference, expected):
    with contextlib.ExitStack() as stack:
        if os.name == "nt":
            stack.enter_context(os.add_dll_directory(str(Path(library).parent)))
        mpv = ctypes.CDLL(library)
        mpv.mpv_create.restype = ctypes.c_void_p
        mpv.mpv_set_option_string.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ]
        mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
        mpv.mpv_request_log_messages.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_command.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ]
        mpv.mpv_get_property_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        mpv.mpv_get_property_string.restype = ctypes.c_void_p
        mpv.mpv_free.argtypes = [ctypes.c_void_p]
        mpv.mpv_free.restype = None
        mpv.mpv_wait_event.argtypes = [ctypes.c_void_p, ctypes.c_double]
        mpv.mpv_wait_event.restype = ctypes.POINTER(Event)
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        client = mpv.mpv_create()
        if not client:
            raise RuntimeError("Could not create the native client")
        stack.callback(mpv.mpv_terminate_destroy, client)
        if mpv.mpv_request_log_messages(client, b"warn") < 0:
            raise RuntimeError("Native diagnostic subscription failed")
        options = {
            "config": "no",
            "load-scripts": "no",
            "terminal": "no",
            "vo": "null",
            "ao": "null",
            "pause": "yes",
            "ytdl": "no",
            "tls-verify": "no",
            "network-timeout": "5",
            "curl-enabled": "no",
            "stream-lavf-o": BASE_LAVF_OPTIONS,
        }
        for name, value in options.items():
            result = mpv.mpv_set_option_string(client, name.encode(), value.encode())
            if result < 0 and not (name == "curl-enabled" and result == -5):
                raise RuntimeError(f"Native option rejected: {name}")
        if mpv.mpv_initialize(client) < 0:
            raise RuntimeError("Native initialization failed")

        # The same per-file form the player uses: loadfile <url> <flags> <index> <options>.
        per_file = f"stream-lavf-o-append=prefer_family={preference}".encode()
        command = (ctypes.c_char_p * 6)(b"loadfile", url.encode(), b"replace", b"-1", per_file, None)
        if mpv.mpv_command(client, command) < 0:
            raise RuntimeError("Native load command failed")
        messages = []
        end = wait_for(mpv, client, expected, messages)
        if expected == "failed" and (end.reason != 4 or end.error != -13):
            raise RuntimeError(
                f"Unexpected failure: reason={end.reason}, error={end.error}\n"
                + "".join(messages))
        if expected == "loaded":
            stop = (ctypes.c_char_p * 2)(b"stop", None)
            if mpv.mpv_command(client, stop) < 0:
                raise RuntimeError("Native stop command failed")
            wait_for(mpv, client, "ended", messages)

        value = mpv.mpv_get_property_string(client, b"stream-lavf-o")
        if not value:
            raise RuntimeError("stream-lavf-o could not be read after the file ended")
        try:
            restored = ctypes.string_at(value).decode()
        finally:
            mpv.mpv_free(value)
        if "prefer_family" in restored:
            raise RuntimeError(f"The per-file preference leaked past its file: {restored}")


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests += 1
        media = self.server.media
        if media is None:
            self.send_response(403)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        start, end = 0, len(media) - 1
        requested = self.headers.get("Range")
        if requested:
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", requested)
            if not match:
                self.send_error(416)
                return
            start = int(match[1])
            if match[2]:
                end = min(end, int(match[2]))
            if start > end:
                self.send_error(416)
                return
        self.send_response(206 if requested else 200)
        self.send_header("Content-Type", "audio/wav")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        if requested:
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(media)}")
        self.end_headers()
        try:
            self.wfile.write(media[start:end + 1])
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self, family, host, port, media):
        self.address_family = family
        super().__init__((host, port), Handler)
        self.media = media
        self.requests = 0


@contextlib.contextmanager
def serve(family, host, port, media, context):
    server = Server(family, host, port, media)
    if context:
        server.socket = context.wrap_socket(server.socket, server_side=True)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)
        if thread.is_alive():
            raise RuntimeError("Fixture server did not stop")


@contextlib.contextmanager
def dual_stack(v4_media, v6_media, context):
    """Serve different answers on 127.0.0.1 and ::1 behind one port."""
    for _ in range(20):
        with contextlib.ExitStack() as stack:
            v4 = stack.enter_context(serve(socket.AF_INET, "127.0.0.1", 0, v4_media, context))
            try:
                v6 = stack.enter_context(
                    serve(socket.AF_INET6, "::1", v4.server_port, v6_media, context))
            except OSError:
                continue
            yield v4, v6
            return
    raise RuntimeError("Could not bind one port on both loopback families")


@contextlib.contextmanager
def ipv6_only(media, context):
    """Serve ::1 while the same IPv4 port is reserved without a listener."""
    for _ in range(20):
        reserved = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        with reserved, contextlib.ExitStack() as stack:
            v6 = stack.enter_context(serve(socket.AF_INET6, "::1", 0, media, context))
            try:
                reserved.bind(("127.0.0.1", v6.server_port))
            except OSError:
                continue
            yield v6
            return
    raise RuntimeError("Could not reserve the IPv4 twin of the IPv6 fixture port")


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    for candidate in (r"C:\Program Files\Git\usr\bin\openssl.exe",
                      r"C:\Program Files\Git\mingw64\bin\openssl.exe"):
        if Path(candidate).exists():
            return candidate
    raise RuntimeError("The TLS cases need an openssl executable")


def run_case(library, name, url, preference, expected, servers, hit):
    before = [server.requests for server in servers]
    result = subprocess.run(
        [sys.executable, str(Path(__file__).resolve()), "--child",
         library, url, preference, expected],
        capture_output=True, text=True, timeout=30,
    )
    counts = [server.requests - count for server, count in zip(servers, before)]
    if result.returncode:
        raise RuntimeError(f"{name}: {result.stdout}{result.stderr}\nRequests: {counts}")
    observed = [count > 0 for count in counts]
    if observed != hit:
        raise RuntimeError(f"{name}: requests {counts} did not match {hit}")
    print(f"PASS {name}", flush=True)


def main(library):
    families = {info[0] for info in socket.getaddrinfo("localhost", 80, type=socket.SOCK_STREAM)}
    if not {socket.AF_INET, socket.AF_INET6} <= families:
        raise RuntimeError("localhost must resolve to both IPv4 and IPv6 for this test")
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(16000))
    media = buffer.getvalue()
    with tempfile.TemporaryDirectory(prefix="mpv-network-family-") as directory:
        cert, key = (str(Path(directory, name)) for name in ("cert.pem", "key.pem"))
        subprocess.run(
            [find_openssl(), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost"],
            check=True, capture_output=True, timeout=30,
        )
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        for scheme, context in (("http", None), ("https", tls)):
            with dual_stack(media, None, context) as (v4, v6):
                url = f"{scheme}://localhost:{v4.server_port}/media.wav"
                run_case(library, f"{scheme}/ipv4-first", url, "ipv4", "loaded",
                         (v4, v6), [True, False])
                run_case(library, f"{scheme}/ipv6-first", url, "ipv6", "failed",
                         (v4, v6), [False, True])
            with ipv6_only(media, context) as v6:
                url = f"{scheme}://localhost:{v6.server_port}/media.wav"
                run_case(library, f"{scheme}/ipv4-first-fallback", url, "ipv4", "loaded",
                         (v6,), [True])


if __name__ == "__main__":
    if len(sys.argv) == 6 and sys.argv[1] == "--child":
        run_client(*sys.argv[2:])
    elif len(sys.argv) == 2:
        main(str(Path(sys.argv[1]).resolve(strict=True)))
    else:
        raise SystemExit("usage: libmpv_network_family.py <libmpv shared library>")
