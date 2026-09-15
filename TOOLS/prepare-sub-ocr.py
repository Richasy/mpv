#!/usr/bin/env python3
"""Prepare the pinned PP-OCRv6 medium model and its CTC dictionary.

This build-time helper requires PyYAML; playback has no Python dependency.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import urllib.request

import yaml


REPOSITORY = "PaddlePaddle/PP-OCRv6_medium_rec_onnx"
REVISION = "50c7eacafc52fa7bcf4194e8cd08e46f8558504b"
FILES = {
    "inference.onnx": "9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba",
    "inference.yml": "991b700facf5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129",
}
LICENSE_URL = "https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/v3.7.0/LICENSE"
LICENSE_SHA256 = "3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e"


def fingerprint(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(directory, name, expected, uri=None):
    destination = directory / name
    if destination.exists():
        if fingerprint(destination) != expected:
            raise RuntimeError(f"Existing file does not match the pinned model: {destination}")
        return destination
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=directory, delete=False) as output:
            temporary = Path(output.name)
            uri = uri or f"https://huggingface.co/{REPOSITORY}/resolve/{REVISION}/{name}"
            with urllib.request.urlopen(uri, timeout=120) as response:
                while block := response.read(1024 * 1024):
                    output.write(block)
        if fingerprint(temporary) != expected:
            raise RuntimeError(f"Downloaded model hash mismatch: {name}")
        os.replace(temporary, destination)
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    for name, expected in FILES.items():
        download(directory, name, expected)
    license_path = download(
        directory, "LICENSE-PaddleOCR.txt", LICENSE_SHA256, LICENSE_URL)
    config = yaml.safe_load((directory / "inference.yml").read_text(encoding="utf-8"))
    if config["Global"]["model_name"] != "PP-OCRv6_medium_rec":
        raise RuntimeError("Unexpected OCR model configuration")
    characters = config["PostProcess"]["character_dict"]
    if len(characters) != 18708 or any(not isinstance(c, str) or not c for c in characters):
        raise RuntimeError("Unexpected OCR character dictionary")
    dictionary = directory / "dictionary.json"
    payload = json.dumps([""] + characters + [" "], ensure_ascii=False, indent=2) + "\n"
    if dictionary.exists() and dictionary.read_text(encoding="utf-8") != payload:
        raise RuntimeError(f"Refusing to replace a different dictionary: {dictionary}")
    dictionary.write_text(payload, encoding="utf-8")
    provenance = {
        "repository": REPOSITORY, "revision": REVISION, "license": "Apache-2.0",
        "model_sha256": FILES["inference.onnx"],
        "dictionary_sha256": fingerprint(dictionary),
        "license_sha256": fingerprint(license_path),
        "ctc_classes": 18710,
    }
    (directory / "provenance.json").write_text(
        json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"model": str(directory / "inference.onnx"),
                      "dictionary": str(dictionary), "mode": "auto"}))


if __name__ == "__main__":
    main()
