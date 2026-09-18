#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


REPOSITORY = "Richasy/mpv"
RUN_ID = 34752654492
SOURCE_SHA = "8b898bb5fac0d2df8de1451be13e57a130a100cd"
FFMPEG_SHA = "026ddd08e6f80db6251cbb32d011c23c49470713"
LIBPLACEBO_SHA = "3330a515d62139259c26239014f286e233bd3a5c"
DAVS2_SHA = "21d64c8f8e36af71fc7a488cd6f789c86cdd1200"
UAVS3D_SHA = "0e20d2c291853f196c68922a264bcd8471d75b68"
AVS_PATCH_SHA = "6788d317a3a67c44f799d02c4ff83f95d6b10165"
TOOL_SHA = "a" * 40
ARTIFACT_ID = 10316414903
ARM64_ARTIFACT_ID = 10316392721
NOTICE_ROOT = Path(__file__).parents[1] / "ci" / "winbuild" / "notices"
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
AVS_NOTICES = tuple(EXPECTED_AVS_NOTICES)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_info_text(
    ffmpeg=FFMPEG_SHA,
    libplacebo=LIBPLACEBO_SHA,
    target_arch="x86_64",
    include_libplacebo=True,
):
    davs2_cpu_path = (
        "aarch64-neon-intrinsics-no-asm"
        if target_arch == "aarch64"
        else "x86_64-nasm"
    )
    uavs3d_cpu_path = (
        "portable-c" if target_arch == "aarch64" else "x86_64-simd"
    )
    lines = [
        f"mpv_commit={SOURCE_SHA}\n",
        f"ffmpeg_commit={ffmpeg}\n",
    ]
    if include_libplacebo:
        lines.append(f"libplacebo_commit={libplacebo}\n")
    lines.extend(
        [
            f"davs2_commit={DAVS2_SHA}\n",
            f"uavs3d_commit={UAVS3D_SHA}\n",
            f"avs_patch_commit={AVS_PATCH_SHA}\n",
            "davs2_build=static-bit-depth-10\n",
            "uavs3d_build=static-8-and-10-bit\n",
            f"davs2_cpu_path={davs2_cpu_path}\n",
            f"uavs3d_cpu_path={uavs3d_cpu_path}\n",
            "avs_registration_proof=config-and-static-archive-symbols\n",
            f"target_arch={target_arch}\n",
            "build_type=release\n",
            "compiler=clang version 21.1.8\n",
            "pdb_guid=fixture\n",
        ],
    )
    return "".join(lines)


def write_zip(
    path,
    build_info=None,
    target_arch="x86_64",
    omit_notice=None,
    tamper_notice=None,
):
    info = build_info or build_info_text(target_arch=target_arch)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("libmpv-2.dll", b"dll-bytes")
        archive.writestr("libmpv-2.pdb", b"pdb-bytes")
        archive.writestr("ggml-base.dll", b"ggml-bytes")
        archive.writestr("DirectML.dll", b"directml-bytes")
        archive.writestr("build-info.txt", info.encode("utf-8"))
        for name in AVS_NOTICES:
            if name == omit_notice:
                continue
            data = (NOTICE_ROOT / name).read_bytes()
            if name == tamper_notice:
                data += b"\ntampered\n"
            archive.writestr(name, data)


def base_state(root, archive, architecture="x64"):
    digest = sha256(archive)
    artifact_id = ARTIFACT_ID if architecture == "x64" else ARM64_ARTIFACT_ID
    return {
        "archive": str(archive),
        "azure_root": str(root / "azure"),
        "transient_blob": "native/x64/ggml-base.dll",
        "upload_attempts": {},
        "run": {
            "id": RUN_ID,
            "workflow_id": 235091400,
            "path": ".github/workflows/libmpv.yml",
            "head_sha": SOURCE_SHA,
            "event": "workflow_dispatch",
            "status": "completed",
            "conclusion": "failure",
            "repository": {"full_name": REPOSITORY},
        },
        "jobs": {
            "total_count": 2,
            "jobs": [
                {
                    "name": "build (x86_64, x64)",
                    "conclusion": "failure",
                    "steps": [
                        {
                            "name": "Test subtitle translation policy",
                            "conclusion": "success",
                        },
                        {
                            "name": "Build libmpv for x64",
                            "conclusion": "success",
                        },
                        {
                            "name": "Upload to GitHub Artifacts",
                            "conclusion": "success",
                        },
                        {
                            "name": "Upload libmpv-2.dll to Azure Blob",
                            "conclusion": "failure",
                        },
                    ],
                },
                {
                    "name": "build (aarch64, arm64)",
                    "conclusion": "success",
                    "steps": [
                        {
                            "name": "Build libmpv for arm64",
                            "conclusion": "success",
                        },
                        {
                            "name": "Upload to GitHub Artifacts",
                            "conclusion": "success",
                        },
                        {
                            "name": "Upload libmpv-2.dll to Azure Blob",
                            "conclusion": "success",
                        },
                    ],
                },
            ],
        },
        "artifacts": {
            "total_count": 1,
            "artifacts": [
                {
                    "id": artifact_id,
                    "name": f"libmpv-{architecture}",
                    "digest": f"sha256:{digest}",
                    "expired": False,
                    "expires_at": "2099-01-01T00:00:00Z",
                    "workflow_run": {
                        "id": RUN_ID,
                        "head_sha": SOURCE_SHA,
                    },
                }
            ],
        },
    }


def run_helper(root, state, case, expect_success, architecture="x64"):
    state_path = root / f"{case}-state.json"
    state_path.write_text(json.dumps(state), encoding="utf-8")
    stage = root / ".artifact-retry" / "9001" / "2" / architecture
    receipt = root / "receipts" / f"{case}.json"
    environment = os.environ.copy()
    fake = Path(__file__).with_name("fake_artifact_cli.py").resolve()
    environment.update({
        "GITHUB_REPOSITORY": REPOSITORY,
        "GITHUB_RUN_ID": "9001",
        "GITHUB_RUN_ATTEMPT": "2",
        "GITHUB_SHA": TOOL_SHA,
        "AZURE_STORAGE_CONNECTION_STRING": "fixture-secret",
        "FAKE_ARTIFACT_STATE": str(state_path),
        "LIBMPV_RETRY_GH_COMMAND": json.dumps(
            [sys.executable, str(fake), "gh"]),
        "LIBMPV_RETRY_AZ_COMMAND": json.dumps(
            [sys.executable, str(fake), "az"]),
    })
    helper = Path(__file__).parents[1] / "ci" / "retry_azure_artifact.py"
    result = subprocess.run(
        [
            sys.executable, str(helper),
            "--repository", REPOSITORY,
            "--source-run-id", str(RUN_ID),
            "--expected-source-sha", SOURCE_SHA,
            "--expected-ffmpeg-sha", FFMPEG_SHA,
            "--expected-libplacebo-sha", LIBPLACEBO_SHA,
            "--architecture", architecture,
            "--build-type", "release",
            "--artifact-name", f"libmpv-{architecture}",
            "--workspace", str(root),
            "--staging-directory", str(stage),
            "--receipt", str(receipt),
            "--upload-attempts", "2",
            "--upload-timeout-seconds", "30",
            "--retry-delay-seconds", "0",
        ],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=20,
    )
    if "fixture-secret" in result.stdout or "fixture-secret" in result.stderr:
        raise AssertionError(f"{case} leaked the connection string")
    if (expect_success and result.returncode != 0):
        raise AssertionError(result.stderr)
    if not expect_success and result.returncode == 0:
        raise AssertionError(f"{case} unexpectedly succeeded")
    if stage.exists():
        raise AssertionError(f"{case} staging directory was not cleaned")
    return receipt, json.loads(state_path.read_text(encoding="utf-8"))


def verify_success(root, archive):
    state = base_state(root, archive)
    receipt_path, final_state = run_helper(root, state, "success", True)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    assert receipt["status"] == "success"
    assert receipt["source_run_conclusion"] == "failure"
    assert receipt["publication"]["tool_sha"] == TOOL_SHA
    assert receipt["artifact"]["id"] == ARTIFACT_ID
    assert receipt["artifact"]["archive_sha256"] == sha256(archive)
    assert receipt["build_info"]["mpv_commit"] == SOURCE_SHA
    assert receipt["build_info"]["ffmpeg_commit"] == FFMPEG_SHA
    assert receipt["build_info"]["libplacebo_commit"] == LIBPLACEBO_SHA
    assert receipt["build_info"]["davs2_commit"] == DAVS2_SHA
    assert receipt["build_info"]["uavs3d_commit"] == UAVS3D_SHA
    assert receipt["build_info"]["avs_patch_commit"] == AVS_PATCH_SHA
    assert (
        receipt["provenance_policy"]["expected_libplacebo_commit"] ==
        LIBPLACEBO_SHA
    )
    assert final_state["upload_attempts"]["native/x64/ggml-base.dll"] == 2

    for name in (
        "libmpv-2.dll", "libmpv-2.pdb",
        "ggml-base.dll", "DirectML.dll", "build-info.txt",
    ):
        uploaded = root / "azure" / "native" / "x64" / name
        assert uploaded.is_file()
        assert receipt["files"][name]["sha256"] == sha256(uploaded)
    for name, expected_sha256 in EXPECTED_AVS_NOTICES.items():
        uploaded = root / "azure" / "native" / "x64" / name
        assert uploaded.is_file()
        assert sha256(uploaded) == expected_sha256
        assert receipt["files"][name]["sha256"] == expected_sha256
        upload = next(
            item for item in receipt["uploads"]
            if item["blob"] == f"native/x64/{name}"
        )
        assert upload["attempts"] == 1
    for name in ("libmpv-2.dll", "libmpv-2.pdb", "build-info.txt"):
        uploaded = root / "azure" / "symbols" / SOURCE_SHA / "x64" / name
        assert uploaded.is_file()
        assert receipt["files"][name]["sha256"] == sha256(uploaded)


def verify_arm64_success(root, archive):
    state = base_state(root, archive, "arm64")
    state["transient_blob"] = ""
    receipt_path, final_state = run_helper(
        root, state, "arm64-success", True, "arm64")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    assert receipt["status"] == "success"
    assert receipt["source_run_conclusion"] == "failure"
    assert receipt["artifact"]["id"] == ARM64_ARTIFACT_ID
    assert receipt["build_info"]["target_arch"] == "aarch64"
    assert final_state["upload_attempts"]["native/arm64/libmpv-2.dll"] == 1
    for name, expected_sha256 in EXPECTED_AVS_NOTICES.items():
        stable = root / "azure" / "native" / "arm64" / name
        assert stable.is_file()
        assert sha256(stable) == expected_sha256
        assert receipt["files"][name]["sha256"] == expected_sha256
        upload = next(
            item for item in receipt["uploads"]
            if item["blob"] == f"native/arm64/{name}"
        )
        assert upload["attempts"] == 1
    for name in ("libmpv-2.dll", "libmpv-2.pdb", "build-info.txt"):
        stable = root / "azure" / "native" / "arm64" / name
        symbol = root / "azure" / "symbols" / SOURCE_SHA / "arm64" / name
        assert stable.is_file()
        assert symbol.is_file()
        assert receipt["files"][name]["sha256"] == sha256(stable)
        assert receipt["files"][name]["sha256"] == sha256(symbol)


def verify_failure(root, archive, case, mutate):
    state = base_state(root, archive)
    mutate(state)
    receipt, final_state = run_helper(root, state, case, False)
    data = None
    if receipt.exists():
        data = json.loads(receipt.read_text(encoding="utf-8"))
        assert data["status"] == "failed"
        assert "fixture-secret" not in json.dumps(data)
    assert not final_state.get("upload_attempts")
    return data


def verify_nontransient_upload_failure(root, archive):
    state = base_state(root, archive)
    state["transient_blob"] = ""
    state["fatal_blob"] = "native/x64/DirectML.dll"
    receipt_path, final_state = run_helper(
        root, state, "nontransient-upload", False)
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    assert receipt["status"] == "failed"
    assert "fixture-secret" not in json.dumps(receipt)
    assert receipt["artifact"]["archive_sha256"] == sha256(archive)
    assert receipt["files"]["libmpv-2.dll"]["sha256"]
    assert receipt["current_upload"] == "native/x64/DirectML.dll"
    assert final_state["upload_attempts"]["native/x64/DirectML.dll"] == 1


def artifact_digest_mutation(archive):
    def mutate(state):
        state["artifacts"]["artifacts"][0].update({
            "digest": f"sha256:{sha256(archive)}",
        })

    return mutate


def main():
    root = Path.cwd() / f"build-artifact-retry-test-{os.getpid()}"
    if root.exists():
        raise RuntimeError("owned test directory already exists")
    root.mkdir()
    archive = root / "artifact.zip"
    arm64_archive = root / "artifact-arm64.zip"
    try:
        for name, expected_sha256 in EXPECTED_AVS_NOTICES.items():
            assert sha256(NOTICE_ROOT / name) == expected_sha256
        write_zip(archive)
        write_zip(arm64_archive, target_arch="aarch64")
        verify_success(root, archive)
        verify_arm64_success(root, arm64_archive)
        verify_nontransient_upload_failure(root, archive)
        verify_failure(
            root, archive, "duplicate",
            lambda state: state["artifacts"]["artifacts"].append(
                dict(state["artifacts"]["artifacts"][0])),
        )
        verify_failure(
            root, archive, "expired",
            lambda state: state["artifacts"]["artifacts"][0].update(
                {"expired": True}),
        )
        verify_failure(
            root, archive, "digest",
            lambda state: state["artifacts"]["artifacts"][0].update(
                {"digest": "sha256:" + "0" * 64}),
        )
        verify_failure(
            root, archive, "head",
            lambda state: state["run"].update({"head_sha": "1" * 40}),
        )
        verify_failure(
            root, archive, "artifact-head",
            lambda state: state["artifacts"]["artifacts"][0][
                "workflow_run"].update({"head_sha": "2" * 40}),
        )
        verify_failure(
            root, archive, "run-id",
            lambda state: state["run"].update({"id": RUN_ID + 1}),
        )
        verify_failure(
            root, archive, "workflow-id",
            lambda state: state["run"].update({"workflow_id": 1}),
        )
        verify_failure(
            root, archive, "repository",
            lambda state: state["run"]["repository"].update(
                {"full_name": "Other/mpv"}),
        )
        verify_failure(
            root, archive, "job",
            lambda state: state["jobs"]["jobs"][0]["steps"][1].update(
                {"conclusion": "failure"}),
        )
        verify_failure(
            root, archive, "unexplained-job",
            lambda state: state["jobs"]["jobs"][1].update(
                {"conclusion": "failure"}),
        )

        malformed = root / "malformed.zip"
        write_zip(
            malformed,
            build_info_text(include_libplacebo=False),
        )
        verify_failure(
            root, malformed, "provenance",
            lambda state: state["artifacts"]["artifacts"][0].update({
                "digest": f"sha256:{sha256(malformed)}",
            }),
        )

        wrong_ffmpeg = root / "wrong-ffmpeg.zip"
        write_zip(
            wrong_ffmpeg,
            build_info_text(ffmpeg="1" * 40),
        )
        verify_failure(
            root, wrong_ffmpeg, "wrong-ffmpeg",
            lambda state: state["artifacts"]["artifacts"][0].update({
                "digest": f"sha256:{sha256(wrong_ffmpeg)}",
            }),
        )

        wrong_libplacebo = root / "wrong-libplacebo.zip"
        write_zip(
            wrong_libplacebo,
            build_info_text(libplacebo="2" * 40),
        )
        wrong_libplacebo_receipt = verify_failure(
            root, wrong_libplacebo, "wrong-libplacebo",
            lambda state: state["artifacts"]["artifacts"][0].update({
                "digest": f"sha256:{sha256(wrong_libplacebo)}",
            }),
        )
        assert (
            wrong_libplacebo_receipt["error"] ==
            "build-info libplacebo_commit does not match"
        )

        for index, name in enumerate(AVS_NOTICES):
            missing_notice = root / f"missing-notice-{index}.zip"
            write_zip(missing_notice, omit_notice=name)
            missing_receipt = verify_failure(
                root, missing_notice, f"missing-notice-{index}",
                artifact_digest_mutation(missing_notice),
            )
            assert (
                missing_receipt["error"] ==
                f"artifact is missing required file {name}"
            )

            tampered_notice = root / f"tampered-notice-{index}.zip"
            write_zip(tampered_notice, tamper_notice=name)
            tampered_receipt = verify_failure(
                root, tampered_notice, f"tampered-notice-{index}",
                artifact_digest_mutation(tampered_notice),
            )
            assert (
                tampered_receipt["error"] ==
                f"artifact notice SHA256 does not match: {name}"
            )

        wrong_arch = root / "wrong-arch.zip"
        write_zip(wrong_arch, target_arch="aarch64")
        verify_failure(
            root, wrong_arch, "wrong-arch",
            lambda state: state["artifacts"]["artifacts"][0].update({
                "digest": f"sha256:{sha256(wrong_arch)}",
            }),
        )

        occupied_stage = root / ".artifact-retry" / "9001" / "2" / "x64"
        occupied_stage.mkdir(parents=True)
        marker = occupied_stage / "foreign"
        marker.write_text("keep", encoding="utf-8")
        state = base_state(root, archive)
        state["transient_blob"] = ""
        state_path = root / "ownership-state.json"
        state_path.write_text(json.dumps(state), encoding="utf-8")
        environment = os.environ.copy()
        fake = Path(__file__).with_name("fake_artifact_cli.py").resolve()
        environment.update({
            "GITHUB_REPOSITORY": REPOSITORY,
            "GITHUB_RUN_ID": "9001",
            "GITHUB_RUN_ATTEMPT": "2",
            "GITHUB_SHA": TOOL_SHA,
            "AZURE_STORAGE_CONNECTION_STRING": "fixture-secret",
            "FAKE_ARTIFACT_STATE": str(state_path),
            "LIBMPV_RETRY_GH_COMMAND": json.dumps(
                [sys.executable, str(fake), "gh"]),
            "LIBMPV_RETRY_AZ_COMMAND": json.dumps(
                [sys.executable, str(fake), "az"]),
        })
        helper = Path(__file__).parents[1] / "ci" / "retry_azure_artifact.py"
        result = subprocess.run(
            [
                sys.executable, str(helper),
                "--repository", REPOSITORY,
                "--source-run-id", str(RUN_ID),
                "--expected-source-sha", SOURCE_SHA,
                "--expected-ffmpeg-sha", FFMPEG_SHA,
                "--expected-libplacebo-sha", LIBPLACEBO_SHA,
                "--architecture", "x64",
                "--build-type", "release",
                "--artifact-name", "libmpv-x64",
                "--workspace", str(root),
                "--staging-directory", str(occupied_stage),
                "--receipt", str(root / "receipts" / "ownership.json"),
            ],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
        assert result.returncode != 0
        assert marker.read_text(encoding="utf-8") == "keep"
    finally:
        shutil.rmtree(root)
    print("artifact retry helper tests passed")


if __name__ == "__main__":
    main()
