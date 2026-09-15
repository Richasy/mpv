#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

import http.server
import json
import subprocess
import sys
import threading
import urllib.parse
from pathlib import Path
from tempfile import TemporaryDirectory


class TranslationServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), TranslationHandler)
        self.lock = threading.Lock()
        self.requests = []
        self.hold_started = threading.Event()
        self.hold_release = threading.Event()
        self.hold_completed = threading.Event()


class TranslationHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        return

    def do_POST(self):
        if urllib.parse.urlsplit(self.path).path != "/v1/chat/completions":
            self.send_error(404)
            return
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size))
        messages = request.get("messages", [])
        text = messages[-1].get("content", "") if messages else ""
        with self.server.lock:
            self.server.requests.append(text)
        mode = urllib.parse.parse_qs(
            urllib.parse.urlsplit(self.path).query
        ).get("mode", ["normal"])[0]
        if mode == "partial" and text == "world":
            self.send_error(500, "fixture span failure")
            return
        if mode == "hold":
            self.server.hold_started.set()
            if not self.server.hold_release.wait(15):
                self.send_error(500, "fixture hold timed out")
                return
            self.server.hold_completed.set()
        if mode == "after-hold" and not self.server.hold_completed.wait(3):
            self.send_error(500, "stale request did not finish")
            return
        translated = ("STALE:" if mode == "hold" else "translated:") + text
        if mode == "escape" and text == "Escaping":
            translated = "{\\p1}\\N\n你好"
        payload = json.dumps(
            {"choices": [{"message": {"content": translated}}]}
        ).encode("utf-8")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass  # Cancellation is expected for deliberately held old work.


def run_test(dense_path, ssa_path, bilingual_path):
    server = TranslationServer()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    originals = {Path(path): Path(path).read_bytes()
                 for path in [*sys.argv[2:], dense_path, ssa_path, bilingual_path]}
    try:
        process = subprocess.Popen(
            [
                sys.argv[1],
                f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                *sys.argv[2:],
                str(dense_path),
                str(ssa_path),
                str(bilingual_path),
            ],
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.PIPE,
        )
        output = []
        failures = []

        def read_output():
            for line in process.stdout:
                output.append(line)
                if line.startswith("fixture:"):
                    name = line.strip().removeprefix("fixture:")
                    if name == "hold-started":
                        passed = server.hold_started.wait(3)
                    elif name == "hold-release":
                        server.hold_release.set()
                        passed = server.hold_completed.wait(3)
                    else:
                        passed = False
                    if not passed:
                        failures.append(f"barrier failed: {name}")
                    try:
                        process.stdin.write("ok\n" if passed else "failed\n")
                        process.stdin.flush()
                    except BrokenPipeError:
                        break

        reader = threading.Thread(target=read_output, daemon=True)
        reader.start()
        try:
            process.wait(timeout=90)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
            server.hold_release.set()
            reader.join(timeout=5)
            process.stdin.close()
            process.stdout.close()
            sys.stdout.buffer.write("".join(output).encode("utf-8"))
        if process.returncode or failures or reader.is_alive():
            with server.lock:
                sys.stdout.write(f"fixture requests: {server.requests!r}\n")
            raise RuntimeError(
                f"native subtitle translation test exited with "
                f"{process.returncode}: {failures}"
            )
        with server.lock:
            requests = list(server.requests)
        expected = {"foo", "bar", "Hello", "world", "I", "Repeat",
                    "Positioned", "Caption", "ka", "ra", "oke", "Escaping",
                    "Upper one", "Upper two", "Upper speaker", "Lower speaker",
                    "Plain first", "Plain second", "Normal", "emphasis"}
        expected.update(f"Upper block {index}" for index in range(1, 5))
        expected.update(f"dense-{index:03d}" for index in range(160))
        if not expected.issubset(requests) or any(
            text not in expected for text in requests
        ):
            raise RuntimeError(
                f"unexpected translated request set: {requests!r}"
            )
        if any("{\\i" in text for text in requests):
            raise RuntimeError("ASS override tags reached the translator")
        if any("http://" in text or "fixture-secret" in text
               for text in requests):
            raise RuntimeError("private configuration leaked into translation text")
        for path, content in originals.items():
            if path.read_bytes() != content:
                raise RuntimeError(f"source fixture was modified: {path}")
    finally:
        server.shutdown()
        worker.join(timeout=3)
        server.server_close()
        if worker.is_alive():
            raise RuntimeError("translation fixture server did not stop")


def main():
    if len(sys.argv) != 8:
        raise SystemExit(
            "expected executable, embedded, video, external, bitmap, dual, "
            "and ASS fixtures"
        )
    # Keep all generated fixture files in the selected build working directory.
    with TemporaryDirectory(prefix="mpv-sub-translation-", dir=Path.cwd()) as directory:
        dense_path = Path(directory) / "dense.srt"
        dense_path.write_text(
            "".join(
                f"{index + 1}\n"
                "00:00:05,000 --> 00:00:20,000\n"
                f"dense-{index:03d}\n\n"
                for index in range(160)
            ),
            encoding="utf-8",
        )
        ssa_path = Path(directory) / "styled.ssa"
        ssa_path.write_text(
            "[Script Info]\nScriptType: v4.00\nPlayResX: 640\nPlayResY: 360\n"
            "[V4 Styles]\n"
            "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "TertiaryColour, BackColour, Bold, Italic, BorderStyle, Outline, "
            "Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
            "Style: Sign,Arial,32,65535,255,0,0,-1,-1,1,2,1,6,12,13,14,0,1\n"
            "[Events]\n"
            "Format: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, "
            "Effect, Text\n"
            "Dialogue: Marked=0,0:00:00.00,0:00:03.00,Sign,actor,12,13,14,,"
            "{\\a6\\pos(20,30)}Hello\n",
            encoding="utf-8",
        )
        bilingual_path = Path(directory) / "bilingual-blocks.ass"
        style_format = (
            "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,"
            "OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,ScaleX,"
            "ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,"
            "MarginL,MarginR,MarginV,Encoding\n"
        )
        style_fields = (
            ",Arial,18,&H00FFFFFF,&H0000FFFF,&H00000000,&H00000000,"
            "0,0,0,0,100,100,0,0,1,1,1,2,5,5,2,1\n"
        )
        bilingual_path.write_text(
            "[Script Info]\nScriptType: v4.00+\nPlayResX: 384\nPlayResY: 288\n"
            "[V4+ Styles]\n" + style_format
            + "".join("Style: " + name + style_fields
                      for name in ("Body", "Speaker", "Plain"))
            + "[Events]\n"
            "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
            + "".join(
                f"Dialogue: 0,0:00:{index * 3:02d}.00,"
                f"0:00:{index * 3 + 3:02d}.00,Body,,0,0,0,,"
                + ("Upper one\\NUpper two" if index == 0
                   else f"Upper block {index}")
                + "\\N{\\fs12\\Arial\\i1}Lower unchanged\n"
                for index in range(5)
            )
            + "Dialogue: 0,0:00:00.00,0:00:03.00,Speaker,,0,0,0,,"
            "{\\pos(160,160)}Lower speaker\n"
            + "Dialogue: 0,0:00:00.00,0:00:03.00,Speaker,,0,0,0,,"
            "{\\pos(160,40)}Upper speaker\n"
            + "Dialogue: 0,0:00:00.00,0:00:03.00,Plain,,0,0,0,,"
            "Plain first\\NPlain second\n"
            + "Dialogue: 0,0:00:00.00,0:00:03.00,Plain,,0,0,0,,"
            "Normal\\N{\\i1}emphasis{\\i0}\n",
            encoding="utf-8",
        )
        run_test(dense_path, ssa_path, bilingual_path)


if __name__ == "__main__":
    main()
