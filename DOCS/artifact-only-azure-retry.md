# Artifact-only Azure retry

The `libmpv` workflow supports retrying Azure publication from an existing
GitHub Actions artifact without rebuilding native code. The helper is
restricted to `Richasy/mpv`, workflow ID `235091400`, and
`.github/workflows/libmpv.yml`.

Set `source_run_id` to the exact prior workflow run and provide its full
40-character `expected_source_sha` and `expected_libplacebo_sha`. In retry
mode, `build_x64` and `build_arm64` select which existing artifacts are
published. `upload_target` must be `azure` or `both`; both values perform only
the Azure retry because the source GitHub artifact is reused in place rather
than repackaged.

For an x64 retry from a build using the current Player FFmpeg pin:

```sh
gh workflow run libmpv.yml \
  --repo Richasy/mpv \
  --ref SOURCE_REF \
  -f source_run_id=SOURCE_RUN_ID \
  -f expected_source_sha=FULL_MPV_SHA \
  -f expected_libplacebo_sha=FULL_LIBPLACEBO_SHA \
  -f build_x64=true \
  -f build_arm64=false \
  -f upload_target=azure \
  -f build_type=release \
  -f ffmpeg_ref=026ddd08e6f80db6251cbb32d011c23c49470713
```

The helper discovers the artifact by exact name within the explicit run and
verifies the downloaded archive against its GitHub metadata digest before
extraction.

The retry helper verifies:

- repository, workflow ID, run ID, event, completion state, and exact source
  SHA;
- a successful GitHub-only source may have its Azure publication step
  explicitly skipped;
- any failed source run must contain actual Azure publication failures only;
  a skipped Azure step never makes a failed run eligible;
- successful architecture build and GitHub artifact-upload steps;
- exactly one non-expired artifact with the expected architecture/build name;
- the downloaded archive SHA-256 against GitHub artifact metadata;
- exact `build-info.txt` values for mpv, FFmpeg, libplacebo, target
  architecture, build type, pinned AVS sources, patch provenance, architecture
  paths, and registration proof, plus a Clang compiler identity;
- required `libmpv-2.dll`, `libmpv-2.pdb`, and `build-info.txt` files, plus
  exact SHA-256 values for all AVS license and source notices.

Uploads use the existing `AZURE_STORAGE_CONNECTION_STRING` secret implicitly
through the Azure CLI environment. Each upload is limited to 180 seconds and
at most three attempts, with bounded backoff only for recognized transient
failures; the publication job is limited to 45 minutes. Staging is scoped by
retry run, attempt, and architecture and is removed only when its ownership
marker matches.

Successful uploads retain the existing paths:

- `native/<arch>/` for all top-level DLL, PDB, and TXT files;
- `symbols/<source-sha>/<arch>/` for `libmpv-2.dll`, `libmpv-2.pdb`, and
  `build-info.txt`.

Each architecture emits a nonsecret receipt artifact containing the source
run and source Azure-step conclusions, GitHub artifact ID and archive digest,
build provenance, per-file SHA-256 values, destinations, retry counts, and the
separate commit that supplied the publication tooling. A failed source workflow
is never relabeled as successful; it is accepted only when all failures are
confined to Azure publication and the reusable artifact passed every
verification above.
