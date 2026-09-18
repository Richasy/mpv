#!/usr/bin/env python3
"""Republish a verified libmpv GitHub artifact to the existing Azure paths."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import time
import uuid
import zipfile


WORKFLOW_ID = 235091400
EXPECTED_REPOSITORY = "Richasy/mpv"
EXPECTED_DAVS2_COMMIT = "21d64c8f8e36af71fc7a488cd6f789c86cdd1200"
EXPECTED_UAVS3D_COMMIT = "0e20d2c291853f196c68922a264bcd8471d75b68"
EXPECTED_AVS_PATCH_COMMIT = "6788d317a3a67c44f799d02c4ff83f95d6b10165"
EXPECTED_AVS_NOTICES = {
    "AVS-THIRD-PARTY-NOTICES.txt": (
        "21bfffd34ee6644dd7acbeffc65a68afd449e3ec4427f1a67afe46c0fb38517d"
    ),
    "GPL-2.0.txt": (
        "edaef632cbb643e4e7a221717a6c441a4c1a7c918e6e4d56debc3d8739b233f6"
    ),
    "GPL-3.0.txt": (
        "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903"
    ),
    "UAVS3D-BSD-3-Clause.txt": (
        "5a8dcb7da222df8a81b6e334000f859248196335e2d28e1db9f3c552827d7cdf"
    ),
}
REQUIRED_AVS_NOTICES = tuple(EXPECTED_AVS_NOTICES)
TRANSIENT_UPLOAD_ERRORS = (
    "unexpected_eof",
    "eof occurred in violation",
    "connection reset",
    "connection aborted",
    "connection closed",
    "temporarily unavailable",
    "timed out",
    "timeout",
    "too many requests",
    "internal server error",
    "bad gateway",
    "service unavailable",
    "gateway timeout",
    "server busy",
    "status code: 429",
    "status code: 500",
    "status code: 502",
    "status code: 503",
    "status code: 504",
)


class RetryError(RuntimeError):
    pass


def command_from_env(name, fallback):
    value = os.environ.get(name)
    if not value:
        return [fallback]
    command = json.loads(value)
    if not isinstance(command, list) or not command or not all(
        isinstance(item, str) and item for item in command
    ):
        raise RetryError(f"{name} must be a JSON string array")
    return command


def run_command(command, timeout, stdout=None):
    try:
        result = subprocess.run(
            command,
            stdout=stdout if stdout is not None else subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise RetryError(f"command timed out after {timeout}s") from error
    if result.returncode:
        raise RetryError(f"command failed with exit code {result.returncode}")
    return result.stdout


def gh_json(gh, endpoint):
    payload = run_command(
        [
            *gh, "api",
            "-H", "Accept: application/vnd.github+json",
            "-H", "X-GitHub-Api-Version: 2022-11-28",
            endpoint,
        ],
        60,
    )
    try:
        return json.loads(payload)
    except (TypeError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RetryError("GitHub API returned invalid JSON") from error


def list_run_items(gh, repository, run_id, collection):
    items = []
    page = 1
    while page <= 10:
        data = gh_json(
            gh,
            f"repos/{repository}/actions/runs/{run_id}/{collection}"
            f"?per_page=100&page={page}",
        )
        page_items = data.get(collection)
        total = data.get("total_count")
        if not isinstance(page_items, list) or not isinstance(total, int):
            raise RetryError(f"GitHub {collection} response is malformed")
        items.extend(page_items)
        if len(items) >= total:
            return items
        if not page_items:
            break
        page += 1
    raise RetryError(f"GitHub {collection} response exceeded pagination bound")


def validate_run(gh, repository, run_id, expected_sha, architecture):
    run = gh_json(gh, f"repos/{repository}/actions/runs/{run_id}")
    if run.get("id") != run_id:
        raise RetryError("source workflow run ID does not match")
    if (run.get("repository") or {}).get("full_name") != repository:
        raise RetryError("source workflow repository does not match")
    if run.get("workflow_id") != WORKFLOW_ID:
        raise RetryError("source workflow ID does not match libmpv")
    if run.get("path") != ".github/workflows/libmpv.yml":
        raise RetryError("source workflow path does not match libmpv.yml")
    if run.get("head_sha", "").lower() != expected_sha:
        raise RetryError("source workflow head SHA does not match")
    if run.get("event") != "workflow_dispatch" or run.get("status") != "completed":
        raise RetryError("source workflow is not a completed manual dispatch")
    if run.get("conclusion") not in ("success", "failure"):
        raise RetryError("source workflow conclusion is not publishable")

    jobs = list_run_items(gh, repository, run_id, "jobs")
    target_arch = "x86_64" if architecture == "x64" else "aarch64"
    job_name = f"build ({target_arch}, {architecture})"
    selected = [job for job in jobs if job.get("name") == job_name]
    if len(selected) != 1:
        raise RetryError(f"expected exactly one {job_name} job")
    steps = selected[0].get("steps") or []
    required = [
        f"Build libmpv for {architecture}",
        "Upload to GitHub Artifacts",
    ]
    if architecture == "x64":
        required.append("Test subtitle translation policy")
    for name in required:
        matches = [step for step in steps if step.get("name") == name]
        if len(matches) != 1 or matches[0].get("conclusion") != "success":
            raise RetryError(f"required source step did not succeed: {name}")
    azure_steps = [
        step for step in steps
        if step.get("name") == "Upload libmpv-2.dll to Azure Blob"
    ]
    if len(azure_steps) != 1:
        raise RetryError("source Azure publication step was not requested")
    azure_conclusion = azure_steps[0].get("conclusion")
    if azure_conclusion not in ("success", "failure", "skipped"):
        raise RetryError("source Azure publication step was not requested")
    if azure_conclusion == "skipped" and (
        run.get("conclusion") != "success"
        or selected[0].get("conclusion") != "success"
    ):
        raise RetryError(
            "skipped source Azure publication requires a successful workflow")

    failed_steps = []
    for job in jobs:
        job_conclusion = job.get("conclusion")
        if job_conclusion not in ("success", "failure", "skipped"):
            raise RetryError("source workflow contains an invalid job conclusion")
        job_failed_steps = []
        for step in job.get("steps") or []:
            if step.get("conclusion") in (
                "failure", "cancelled", "timed_out",
                "action_required", "startup_failure",
            ):
                job_failed_steps.append(step.get("name"))
        failed_steps.extend(job_failed_steps)
        if job_conclusion == "failure" and not job_failed_steps:
            raise RetryError("source workflow contains an unexplained job failure")
    if any(name != "Upload libmpv-2.dll to Azure Blob"
           for name in failed_steps):
        raise RetryError("source workflow failed outside Azure publication")
    if run.get("conclusion") == "failure" and not failed_steps:
        raise RetryError("failed source workflow has no Azure publication failure")
    if run.get("conclusion") == "success" and failed_steps:
        raise RetryError("successful source workflow contains a failed step")
    return run, azure_conclusion


def find_artifact(gh, repository, run_id, artifact_name, expected_sha):
    artifacts = list_run_items(gh, repository, run_id, "artifacts")
    matches = [item for item in artifacts if item.get("name") == artifact_name]
    if len(matches) != 1:
        raise RetryError(
            f"expected exactly one artifact named {artifact_name}")
    artifact = matches[0]
    if artifact.get("expired") is not False:
        raise RetryError("source artifact is expired")
    expires_at = artifact.get("expires_at")
    if not isinstance(expires_at, str):
        raise RetryError("source artifact expiration is missing")
    try:
        expires = datetime.datetime.fromisoformat(
            expires_at.replace("Z", "+00:00"))
    except ValueError as error:
        raise RetryError("source artifact expiration is invalid") from error
    if expires <= datetime.datetime.now(datetime.timezone.utc):
        raise RetryError("source artifact expiration has passed")
    source = artifact.get("workflow_run")
    if not isinstance(source, dict):
        raise RetryError("artifact workflow provenance is missing")
    if source.get("id") != run_id:
        raise RetryError("artifact belongs to a different workflow run")
    if source.get("head_sha", "").lower() != expected_sha:
        raise RetryError("artifact head SHA does not match")
    digest = artifact.get("digest")
    if not isinstance(digest, str) or not re.fullmatch(
        r"sha256:[0-9a-fA-F]{64}", digest):
        raise RetryError("artifact metadata has no valid SHA256 digest")
    if not isinstance(artifact.get("id"), int):
        raise RetryError("artifact metadata has no numeric ID")
    return artifact


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_artifact(gh, repository, artifact, archive):
    with archive.open("xb") as output:
        run_command(
            [
                *gh, "api", "-H",
                "Accept: application/vnd.github+json",
                "-H", "X-GitHub-Api-Version: 2022-11-28",
                f"repos/{repository}/actions/artifacts/{artifact['id']}/zip",
            ],
            180,
            stdout=output,
        )
    actual = sha256_file(archive)
    expected = artifact["digest"].split(":", 1)[1].lower()
    if actual != expected:
        raise RetryError(
            f"artifact archive SHA256 mismatch: expected {expected}, "
            f"got {actual}")
    return actual


def safe_extract(archive, destination):
    destination.mkdir()
    seen = set()
    total_size = 0
    with zipfile.ZipFile(archive) as package:
        for entry in package.infolist():
            path = PurePosixPath(entry.filename)
            if path.is_absolute() or ".." in path.parts or "\\" in entry.filename:
                raise RetryError("artifact contains an unsafe path")
            if entry.filename in seen:
                raise RetryError("artifact contains a duplicate path")
            seen.add(entry.filename)
            total_size += entry.file_size
            if total_size > 4 * 1024 * 1024 * 1024:
                raise RetryError("artifact expands beyond the 4 GiB safety limit")
            mode = entry.external_attr >> 16
            if mode & 0o170000 == 0o120000:
                raise RetryError("artifact contains a symbolic link")
        package.extractall(destination)


def parse_build_info(path):
    if path.stat().st_size > 64 * 1024:
        raise RetryError("build-info exceeds the 64 KiB safety limit")
    values = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if not raw_line or raw_line.startswith("#"):
            continue
        if "=" not in raw_line:
            raise RetryError("build-info contains a malformed line")
        key, value = raw_line.split("=", 1)
        if not key or key in values:
            raise RetryError("build-info contains a duplicate or empty key")
        values[key] = value
    return values


def validate_payload(payload, expected_sha, expected_ffmpeg,
                     expected_libplacebo, target_arch, build_type):
    build_infos = list(payload.rglob("build-info.txt"))
    if len(build_infos) != 1:
        raise RetryError("artifact must contain exactly one build-info.txt")
    root = build_infos[0].parent
    required = [
        "libmpv-2.dll",
        "libmpv-2.pdb",
        "build-info.txt",
        *REQUIRED_AVS_NOTICES,
    ]
    for name in required:
        if not (root / name).is_file():
            raise RetryError(f"artifact is missing required file {name}")
    for name, expected_sha256 in EXPECTED_AVS_NOTICES.items():
        if sha256_file(root / name) != expected_sha256:
            raise RetryError(
                f"artifact notice SHA256 does not match: {name}")

    info = parse_build_info(root / "build-info.txt")
    required_keys = (
        "mpv_commit", "ffmpeg_commit", "libplacebo_commit",
        "davs2_commit", "uavs3d_commit", "avs_patch_commit",
        "davs2_build", "uavs3d_build", "davs2_cpu_path",
        "uavs3d_cpu_path", "avs_registration_proof",
        "target_arch", "build_type", "compiler",
    )
    if any(not info.get(key) for key in required_keys):
        raise RetryError("build-info is missing required provenance")
    if info["mpv_commit"].lower() != expected_sha:
        raise RetryError("build-info mpv_commit does not match")
    if info["ffmpeg_commit"].lower() != expected_ffmpeg:
        raise RetryError("build-info ffmpeg_commit does not match")
    if not re.fullmatch(r"[0-9a-fA-F]{40}", info["libplacebo_commit"]):
        raise RetryError("build-info libplacebo_commit is not a full SHA")
    if info["libplacebo_commit"].lower() != expected_libplacebo:
        raise RetryError("build-info libplacebo_commit does not match")
    expected_avs = {
        "davs2_commit": EXPECTED_DAVS2_COMMIT,
        "uavs3d_commit": EXPECTED_UAVS3D_COMMIT,
        "avs_patch_commit": EXPECTED_AVS_PATCH_COMMIT,
        "davs2_build": "static-bit-depth-10",
        "uavs3d_build": "static-8-and-10-bit",
        "avs_registration_proof": "config-and-static-archive-symbols",
    }
    for key, expected in expected_avs.items():
        if info[key].lower() != expected.lower():
            raise RetryError(f"build-info {key} does not match")
    if info["target_arch"] != target_arch:
        raise RetryError("build-info target_arch does not match")
    expected_cpu_paths = {
        "x86_64": ("x86_64-nasm", "x86_64-simd"),
        "aarch64": ("aarch64-neon-intrinsics-no-asm", "portable-c"),
    }
    davs2_cpu_path, uavs3d_cpu_path = expected_cpu_paths[target_arch]
    if info["davs2_cpu_path"] != davs2_cpu_path:
        raise RetryError("build-info davs2_cpu_path does not match")
    if info["uavs3d_cpu_path"] != uavs3d_cpu_path:
        raise RetryError("build-info uavs3d_cpu_path does not match")
    if info["build_type"] != build_type:
        raise RetryError("build-info build_type does not match")
    if "clang" not in info.get("compiler", "").lower():
        raise RetryError("build-info compiler is not clang")
    receipt_info = {key: info[key] for key in required_keys}
    if info.get("pdb_guid"):
        receipt_info["pdb_guid"] = info["pdb_guid"]

    files = sorted(
        path for path in root.iterdir()
        if path.is_file() and path.suffix in (".dll", ".pdb", ".txt")
    )
    if not files:
        raise RetryError("artifact contains no publishable files")
    hashes = {
        path.name: {
            "size": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        for path in files
    }
    return root, files, receipt_info, hashes


def is_transient_upload_failure(stderr):
    value = stderr.decode("utf-8", errors="replace").lower()
    return any(marker in value for marker in TRANSIENT_UPLOAD_ERRORS)


def upload_file(az, source, blob, attempts, timeout, retry_delay):
    for attempt in range(1, attempts + 1):
        transient = False
        try:
            result = subprocess.run(
                [
                    *az, "storage", "blob", "upload",
                    "--container-name", "rodel-player-deps",
                    "--name", blob,
                    "--file", str(source),
                    "--overwrite", "true",
                    "--only-show-errors",
                    "--no-progress",
                    "--output", "none",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            transient = True
        else:
            if result.returncode == 0:
                return attempt
            transient = is_transient_upload_failure(result.stderr)
        if not transient or attempt == attempts:
            raise RetryError(
                f"Azure upload failed for {source.name} after {attempt} "
                "attempt(s)")
        if retry_delay:
            time.sleep(retry_delay * attempt)
    raise RetryError(f"Azure upload failed for {source.name}")


def write_receipt(path, receipt):
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(path.suffix + ".partial")
    partial.write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    partial.replace(path)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repository", required=True)
    parser.add_argument("--source-run-id", required=True, type=int)
    parser.add_argument("--expected-source-sha", required=True)
    parser.add_argument("--expected-ffmpeg-sha", required=True)
    parser.add_argument("--expected-libplacebo-sha", required=True)
    parser.add_argument("--architecture", required=True,
                        choices=("x64", "arm64"))
    parser.add_argument("--build-type", required=True,
                        choices=("release", "debug"))
    parser.add_argument("--artifact-name", required=True)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--staging-directory", required=True, type=Path)
    parser.add_argument("--receipt", required=True, type=Path)
    parser.add_argument("--upload-attempts", type=int, default=3)
    parser.add_argument("--upload-timeout-seconds", type=int, default=180)
    parser.add_argument("--retry-delay-seconds", type=int, default=5)
    return parser.parse_args()


def main():
    args = parse_args()
    publication_run_id = os.environ.get("GITHUB_RUN_ID", "")
    publication_run_attempt = os.environ.get("GITHUB_RUN_ATTEMPT", "")
    publication_tool_sha = os.environ.get("GITHUB_SHA", "")
    receipt = {
        "mode": "artifact-only-azure-retry",
        "status": "failed",
        "repository": args.repository,
        "workflow_id": WORKFLOW_ID,
        "source_run_id": args.source_run_id,
        "source_sha": args.expected_source_sha.lower(),
        "architecture": args.architecture,
        "build_type": args.build_type,
        "retry_policy": {
            "max_attempts": args.upload_attempts,
            "operation_timeout_seconds": args.upload_timeout_seconds,
            "retry_delay_seconds": args.retry_delay_seconds,
        },
        "provenance_policy": {
            "expected_mpv_commit": args.expected_source_sha.lower(),
            "expected_ffmpeg_commit": args.expected_ffmpeg_sha.lower(),
            "expected_libplacebo_commit":
                args.expected_libplacebo_sha.lower(),
            "expected_target_arch":
                "x86_64" if args.architecture == "x64" else "aarch64",
            "expected_build_type": args.build_type,
        },
        "publication": {
            "run_id": publication_run_id,
            "run_attempt": publication_run_attempt,
            "tool_sha": publication_tool_sha.lower(),
        },
    }
    token = uuid.uuid4().hex
    stage = args.staging_directory.absolute()
    workspace = args.workspace.absolute()
    expected_stage = (
        workspace / ".artifact-retry" /
        publication_run_id /
        publication_run_attempt /
        args.architecture
    )
    if not workspace.is_dir() or workspace.is_symlink():
        raise RetryError("workspace must be an existing ordinary directory")
    if stage != expected_stage:
        raise RetryError("staging directory is not run/attempt/architecture scoped")
    stage_relative = stage.relative_to(workspace)
    current = workspace
    for part in stage_relative.parts:
        current = current / part
        if current.is_symlink():
            raise RetryError("staging path contains a symbolic link")
    if stage.exists() or stage.is_symlink():
        raise RetryError("staging directory already exists")
    stage.mkdir(parents=True)
    marker = stage / ".owner.json"
    marker.write_text(json.dumps({"token": token}), encoding="utf-8")

    try:
        if os.environ.get("GITHUB_REPOSITORY") != args.repository:
            raise RetryError("repository does not match GITHUB_REPOSITORY")
        if args.repository != EXPECTED_REPOSITORY:
            raise RetryError("artifact retry is restricted to Richasy/mpv")
        if args.source_run_id <= 0:
            raise RetryError("source run ID must be positive")
        if not publication_run_id.isdigit() or not publication_run_attempt.isdigit():
            raise RetryError("publication run identity is unavailable")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", publication_tool_sha):
            raise RetryError("publication tool SHA is unavailable")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", args.expected_source_sha):
            raise RetryError("expected source SHA must be a full commit SHA")
        if not re.fullmatch(r"[0-9a-fA-F]{40}", args.expected_ffmpeg_sha):
            raise RetryError("expected FFmpeg SHA must be a full commit SHA")
        if not re.fullmatch(
            r"[0-9a-fA-F]{40}", args.expected_libplacebo_sha
        ):
            raise RetryError(
                "expected libplacebo SHA must be a full commit SHA")
        if not os.environ.get("AZURE_STORAGE_CONNECTION_STRING"):
            raise RetryError("AZURE_STORAGE_CONNECTION_STRING is unavailable")
        if args.upload_attempts < 1 or args.upload_attempts > 5:
            raise RetryError("upload attempts must be between 1 and 5")
        if args.upload_timeout_seconds < 30 or args.upload_timeout_seconds > 600:
            raise RetryError("upload timeout must be between 30 and 600 seconds")
        if args.retry_delay_seconds < 0 or args.retry_delay_seconds > 60:
            raise RetryError("retry delay must be between 0 and 60 seconds")

        gh = command_from_env("LIBMPV_RETRY_GH_COMMAND", "gh")
        az = command_from_env("LIBMPV_RETRY_AZ_COMMAND", "az")
        run, source_azure_conclusion = validate_run(
            gh, args.repository, args.source_run_id,
            args.expected_source_sha.lower(), args.architecture)

        suffix = "-debug" if args.build_type == "debug" else ""
        expected_name = f"libmpv-{args.architecture}{suffix}"
        if args.artifact_name != expected_name:
            raise RetryError("artifact name does not match architecture/build type")
        artifact = find_artifact(
            gh, args.repository, args.source_run_id, expected_name,
            args.expected_source_sha.lower())
        archive = stage / "artifact.zip"
        archive_sha256 = download_artifact(
            gh, args.repository, artifact, archive)
        payload = stage / "payload"
        safe_extract(archive, payload)
        root, files, build_info, hashes = validate_payload(
            payload, args.expected_source_sha.lower(),
            args.expected_ffmpeg_sha.lower(),
            args.expected_libplacebo_sha.lower(),
            "x86_64" if args.architecture == "x64" else "aarch64",
            args.build_type)

        azure_prefix = f"native/{args.architecture}{suffix}"
        symbol_prefix = (
            f"symbols/{args.expected_source_sha.lower()}/"
            f"{args.architecture}{suffix}"
        )
        receipt.update({
            "source_run_conclusion": run["conclusion"],
            "source_azure_publication_conclusion":
                source_azure_conclusion,
            "artifact": {
                "id": artifact["id"],
                "name": artifact["name"],
                "metadata_digest": artifact["digest"].lower(),
                "archive_sha256": archive_sha256,
                "expires_at": artifact["expires_at"],
            },
            "build_info": build_info,
            "files": hashes,
            "uploads": [],
        })
        for source in files:
            blob = f"{azure_prefix}/{source.name}"
            receipt["current_upload"] = blob
            attempts = upload_file(
                az, source, blob, args.upload_attempts,
                args.upload_timeout_seconds, args.retry_delay_seconds)
            receipt["uploads"].append({"blob": blob, "attempts": attempts})
        for name in ("libmpv-2.dll", "libmpv-2.pdb", "build-info.txt"):
            source = root / name
            blob = f"{symbol_prefix}/{name}"
            receipt["current_upload"] = blob
            attempts = upload_file(
                az, source, blob, args.upload_attempts,
                args.upload_timeout_seconds, args.retry_delay_seconds)
            receipt["uploads"].append({"blob": blob, "attempts": attempts})
        receipt.pop("current_upload", None)
        receipt["status"] = "success"
        write_receipt(args.receipt, receipt)
    except Exception as error:
        receipt["error"] = str(error)
        write_receipt(args.receipt, receipt)
        raise
    finally:
        if stage.is_symlink():
            receipt["status"] = "failed"
            receipt["cleanup_error"] = (
                "staging path became a symbolic link and was retained")
            write_receipt(args.receipt, receipt)
            raise RetryError(receipt["cleanup_error"])
        if stage.exists():
            try:
                owner = json.loads(marker.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                owner = {}
            if owner.get("token") != token:
                receipt["status"] = "failed"
                receipt["cleanup_error"] = (
                    "staging ownership marker changed; directory was retained")
                write_receipt(args.receipt, receipt)
                raise RetryError(receipt["cleanup_error"])
            try:
                shutil.rmtree(stage)
            except OSError as error:
                receipt["status"] = "failed"
                receipt["cleanup_error"] = (
                    "owned staging directory cleanup failed")
                write_receipt(args.receipt, receipt)
                raise RetryError(receipt["cleanup_error"]) from error


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
