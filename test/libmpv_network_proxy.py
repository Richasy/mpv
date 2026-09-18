#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Prove proxy inheritance, explicit direct access, and overrides on HTTP/TLS."""

import contextlib
import ctypes
import http.server
import io
import os
from pathlib import Path
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import wave


class Event(ctypes.Structure):
    _fields_ = [
        ("event_id", ctypes.c_int),
        ("error", ctypes.c_int),
        ("reply_userdata", ctypes.c_uint64),
        ("data", ctypes.c_void_p),
    ]


class EndFile(ctypes.Structure):
    _fields_ = [("reason", ctypes.c_int), ("error", ctypes.c_int)]


def run_client(library, url, backend, mode, proxy):
    with contextlib.ExitStack() as stack:
        if os.name == "nt":
            stack.enter_context(os.add_dll_directory(str(Path(library).parent)))
        mpv = ctypes.CDLL(library)
        mpv.mpv_create.restype = ctypes.c_void_p
        mpv.mpv_set_option_string.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ]
        mpv.mpv_initialize.argtypes = [ctypes.c_void_p]
        mpv.mpv_command.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p),
        ]
        mpv.mpv_wait_event.argtypes = [ctypes.c_void_p, ctypes.c_double]
        mpv.mpv_wait_event.restype = ctypes.POINTER(Event)
        mpv.mpv_terminate_destroy.argtypes = [ctypes.c_void_p]
        mpv.mpv_terminate_destroy.restype = None
        client = mpv.mpv_create()
        if not client:
            raise RuntimeError("Could not create the native client")
        stack.callback(mpv.mpv_terminate_destroy, client)
        options = {
            "config": "no",
            "load-scripts": "no",
            "terminal": "no",
            "vo": "null",
            "ao": "null",
            "pause": "yes",
            "tls-verify": "no",
            "network-timeout": "5",
            "curl-enabled": "yes" if backend == "curl" else "no",
            "stream-lavf-o": "reconnect=0,reconnect_on_network_error=0",
        }
        if mode != "inherit":
            options["http-proxy"] = "" if mode == "direct" else proxy
        for name, value in options.items():
            if mpv.mpv_set_option_string(client, name.encode(), value.encode()) < 0:
                raise RuntimeError(f"Native option rejected: {name}")
        if mpv.mpv_initialize(client) < 0:
            raise RuntimeError("Native initialization failed")
        command = (ctypes.c_char_p * 3)(b"loadfile", url.encode(), None)
        if mpv.mpv_command(client, command) < 0:
            raise RuntimeError("Native load command failed")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            event = mpv.mpv_wait_event(client, 0.25).contents
            if event.event_id == 8:  # MPV_EVENT_FILE_LOADED
                if mode != "direct":
                    raise RuntimeError("The rejecting proxy was bypassed")
                return
            if event.event_id == 7:  # MPV_EVENT_END_FILE
                end = ctypes.cast(event.data, ctypes.POINTER(EndFile)).contents
                if mode == "direct" or end.reason != 4 or end.error != -13:
                    raise RuntimeError(
                        f"Unexpected end-file: reason={end.reason}, error={end.error}"
                    )
                return
        raise TimeoutError("Native media opening did not finish")


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = False

    def __init__(self, media=None):
        super().__init__(("127.0.0.1", 0), Handler)
        self.media = media
        self.requests = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def log_message(self, *args):
        pass

    def do_CONNECT(self):
        self.do_GET()

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


@contextlib.contextmanager
def serve(media=None, context=None):
    server = Server(media)
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


def main(library):
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as audio:
        audio.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        audio.writeframes(bytes(16000))
    with tempfile.TemporaryDirectory(prefix="mpv-network-proxy-") as directory:
        cert, key = (str(Path(directory, name)) for name in ("cert.pem", "key.pem"))
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
             "-keyout", key, "-out", cert, "-days", "1", "-subj", "/CN=localhost"],
            check=True, capture_output=True, timeout=15,
        )
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        with (serve() as inherited, serve() as explicit,
              serve(buffer.getvalue()) as http,
              serve(buffer.getvalue(), tls) as https):
            environment = {
                name: value for name, value in os.environ.items()
                if name.lower() not in {
                    "http_proxy", "https_proxy", "all_proxy", "no_proxy",
                }
            }
            inherited_url = f"http://127.0.0.1:{inherited.server_port}"
            explicit_url = f"http://127.0.0.1:{explicit.server_port}"
            for name in ("http_proxy", "https_proxy", "all_proxy"):
                environment[name] = inherited_url
            for backend in ("lavf", "curl"):
                for scheme, server in (("http", http), ("https", https)):
                    for mode in ("inherit", "direct", "explicit"):
                        before = (inherited.requests, explicit.requests, server.requests)
                        result = subprocess.run(
                            [sys.executable, str(Path(__file__).resolve()), "--child",
                             library, f"{scheme}://127.0.0.1:{server.server_port}/media.wav",
                             backend, mode, explicit_url],
                            env=environment, capture_output=True, text=True, timeout=20,
                        )
                        if result.returncode:
                            raise RuntimeError(
                                f"{backend}/{scheme}/{mode}: {result.stdout}{result.stderr}"
                            )
                        observed = (
                            inherited.requests > before[0],
                            explicit.requests > before[1],
                            server.requests > before[2],
                        )
                        expected = (mode == "inherit", mode == "explicit", mode == "direct")
                        if observed != expected:
                            raise RuntimeError(
                                f"{backend}/{scheme}/{mode}: {observed} != {expected}"
                            )
                        print(f"PASS {backend}/{scheme}/{mode}", flush=True)


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "--child":
        run_client(*sys.argv[2:])
    elif len(sys.argv) == 2:
        main(str(Path(sys.argv[1]).resolve(strict=True)))
    else:
        raise SystemExit("usage: libmpv_network_proxy.py <libmpv shared library>")
