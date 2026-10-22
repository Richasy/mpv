#!/usr/bin/env python3
"""Publish immutable, paired Windows runtimes to a runner-owned local feed."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile


ARCHITECTURES = {"x64": "x86_64", "arm64": "aarch64"}
REQUIRED = (
    "libmpv-2.dll", "libmpv-2.pdb", "build-info.txt",
    "ggml-base.dll", "ggml.dll", "ggml-cpu.dll", "ggml-vulkan.dll",
    "libwhisper.dll", "DirectML.dll", "onnxruntime.dll",
    "onnxruntime_providers_shared.dll", "AVS-THIRD-PARTY-NOTICES.txt",
    "GPL-2.0.txt", "GPL-3.0.txt", "UAVS3D-BSD-3-Clause.txt",
)
POINTER_SCHEMA = "libmpv.runtime-set.pointer.v1"
BUILD_SCHEMA = "libmpv.runtime-set.v1"
ARCH_SCHEMA = "libmpv.runtime-architecture.v1"


def sha256(path):
    with path.open("rb") as stream:
        digest = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_directory(path):
    path = Path(path)
    if not path.is_absolute():
        raise ValueError("Feed and output directories must be absolute.")
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink():
            raise ValueError("Native publication does not follow symbolic links.")
    return path


def required_files(architecture):
    return (*REQUIRED, "mpv-nvngx.dll") if architecture == "x64" else REQUIRED


def check_build_info(path, commit, architecture):
    facts = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            if key in facts:
                raise ValueError("Duplicate native build metadata.")
            facts[key] = value
    if (facts.get("mpv_commit") != commit
            or facts.get("target_arch") != ARCHITECTURES[architecture]
            or facts.get("build_type") != "release"):
        raise ValueError("Native build metadata differs from the selected release source.")


def write_document(path, document):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, sort_keys=True, indent=2)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())


def publish_document(path, document, immutable=False):
    payload = (json.dumps(document, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if path.is_symlink():
        raise ValueError("Native publication does not replace symbolic links.")
    if immutable and path.exists():
        if path.read_bytes() != payload:
            raise ValueError("The existing immutable native manifest differs.")
        return
    descriptor, name = tempfile.mkstemp(prefix=".publication-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


@contextmanager
def publication_lock(root):
    path = root / ".native-publication.lock"
    if path.is_symlink():
        raise ValueError("The publication lock must not be a symbolic link.")
    with path.open("a+b") as stream:
        if os.name == "nt":
            import msvcrt
            if path.stat().st_size == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def build_root(root, run_id, attempt, commit):
    root = safe_directory(root)
    if (not re.fullmatch(r"[1-9][0-9]*", str(run_id))
            or not re.fullmatch(r"[1-9][0-9]*", str(attempt))
            or not re.fullmatch(r"[0-9a-f]{40}", commit)):
        raise ValueError("Publication requires an exact commit, run ID and attempt.")
    return safe_directory(root / "native" / "builds" / f"{run_id}-{attempt}")


def stage(root, source, architecture, run_id, attempt, commit):
    destination = build_root(root, run_id, attempt, commit) / architecture
    source = safe_directory(source)
    if architecture not in ARCHITECTURES:
        raise ValueError("Unsupported architecture.")
    if destination.exists():
        raise ValueError("An immutable architecture output already exists.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    files = sorted(path for path in source.iterdir()
                   if path.suffix.lower() in (".dll", ".pdb", ".txt"))
    if not set(required_files(architecture)).issubset(path.name for path in files):
        raise ValueError("The architecture output is incomplete.")
    if any(not path.is_file() or path.is_symlink() for path in files):
        raise ValueError("Runtime artifacts must be ordinary files.")
    check_build_info(source / "build-info.txt", commit, architecture)
    staging = Path(tempfile.mkdtemp(prefix=f".{architecture}-", dir=destination.parent))
    try:
        records = []
        for path in files:
            target = staging / path.name
            before = path.stat()
            shutil.copyfile(path, target)
            after = path.stat()
            digest = sha256(target)
            if (before.st_size != after.st_size or before.st_mtime_ns != after.st_mtime_ns
                    or sha256(path) != digest or target.stat().st_size <= 0):
                raise ValueError("A runtime artifact changed during publication.")
            records.append({"path": path.name, "size": target.stat().st_size, "sha256": digest})
        check_build_info(staging / "build-info.txt", commit, architecture)
        write_document(staging / "manifest.json", {
            "schema": ARCH_SCHEMA, "mpvCommit": commit, "runId": str(run_id),
            "runAttempt": int(attempt), "platform": architecture, "files": records,
        })
        staging.rename(destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination


def read_architecture(root, architecture, run_id, attempt, commit):
    directory = safe_directory(root / architecture)
    manifest_path = directory / "manifest.json"
    if manifest_path.is_symlink():
        raise ValueError("Architecture manifests must be ordinary files.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("schema") != ARCH_SCHEMA or manifest.get("mpvCommit") != commit
            or manifest.get("runId") != str(run_id) or manifest.get("runAttempt") != int(attempt)
            or manifest.get("platform") != architecture):
        raise ValueError("Native architectures do not share one source and workflow attempt.")
    records = []
    names = set()
    for item in manifest["files"]:
        name = item["path"]
        if not re.fullmatch(r"[a-zA-Z0-9._-]+", name) or name.lower() in names:
            raise ValueError("Unsafe or duplicate native artifact path.")
        names.add(name.lower())
        path = directory / name
        if (not path.is_file() or path.is_symlink() or item["size"] <= 0
                or path.stat().st_size != item["size"] or sha256(path) != item["sha256"]):
            raise ValueError("Native artifact differs from its architecture manifest.")
        records.append({**item, "path": f"{architecture}/{name}", "platform": architecture})
    if not {name.lower() for name in required_files(architecture)}.issubset(names):
        raise ValueError("The architecture manifest is incomplete.")
    check_build_info(directory / "build-info.txt", commit, architecture)
    return records


def complete(root, run_id, attempt, commit):
    root = safe_directory(root)
    directory = build_root(root, run_id, attempt, commit)
    # The OS releases the writer lock even if publication is terminated before its finally block.
    with publication_lock(root):
        manifest_path = directory / "manifest.json"
        files = []
        for architecture in ARCHITECTURES:
            files.extend(read_architecture(directory, architecture, run_id, attempt, commit))
        manifest = {
            "schema": BUILD_SCHEMA, "mpvCommit": commit, "runId": str(run_id),
            "runAttempt": int(attempt), "platforms": list(ARCHITECTURES), "files": files,
        }
        publish_document(manifest_path, manifest, immutable=True)
        pointer = {
            "schema": POINTER_SCHEMA,
            "path": f"native/builds/{run_id}-{attempt}/manifest.json",
            "sha256": sha256(manifest_path),
        }
        latest = root / "native" / "latest.json"
        if latest.exists():
            current = json.loads(latest.read_text(encoding="utf-8"))
            match = re.fullmatch(r"native/builds/([1-9][0-9]*)-([1-9][0-9]*)/manifest.json", current["path"])
            if current["schema"] != POINTER_SCHEMA or not match:
                raise ValueError("The current native pointer is invalid.")
            if (int(match[1]), int(match[2])) > (int(run_id), int(attempt)):
                raise ValueError("A newer native workflow attempt is already published.")
        publish_document(latest, pointer)
        return manifest_path


def select_matrix(build_x64, build_arm64, target):
    if target == "local" and not (build_x64 and build_arm64):
        raise ValueError("Local publication requires both x64 and ARM64 in the same workflow attempt.")
    entries = []
    if build_x64:
        entries.append({"arch": "x86_64", "name": "x64"})
    if build_arm64 and target in ("local", "azure", "both"):
        entries.append({"arch": "aarch64", "name": "arm64"})
    return {"include": entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--root", type=Path, required=True)
    common.add_argument("--run-id", required=True)
    common.add_argument("--attempt", type=int, required=True)
    common.add_argument("--commit", required=True)
    staging = commands.add_parser("stage", parents=[common])
    staging.add_argument("--source", type=Path, required=True)
    staging.add_argument("--architecture", choices=tuple(ARCHITECTURES), required=True)
    commands.add_parser("complete", parents=[common])
    matrix = commands.add_parser("matrix")
    matrix.add_argument("--build-x64", choices=("true", "false"), required=True)
    matrix.add_argument("--build-arm64", choices=("true", "false"), required=True)
    matrix.add_argument("--upload-target", choices=("local", "azure", "github", "both"), required=True)
    args = parser.parse_args()
    if args.command == "matrix":
        selection = select_matrix(args.build_x64 == "true", args.build_arm64 == "true", args.upload_target)
        print("matrix=" + json.dumps(selection, separators=(",", ":")))
        return
    if args.command == "stage":
        result = stage(args.root, args.source, args.architecture, args.run_id, args.attempt, args.commit)
    else:
        result = complete(args.root, args.run_id, args.attempt, args.commit)
    print(f"Published verified native {args.command}: {result}")


if __name__ == "__main__":
    main()
