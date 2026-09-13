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

For the x64 retry from run `34752654492`:

```sh
gh workflow run libmpv.yml \
  --repo Richasy/mpv \
  --ref richasy/subtitle-translation \
  -f source_run_id=34752654492 \
  -f expected_source_sha=8b898bb5fac0d2df8de1451be13e57a130a100cd \
  -f expected_libplacebo_sha=3330a515d62139259c26239014f286e233bd3a5c \
  -f build_x64=true \
  -f build_arm64=false \
  -f upload_target=azure \
  -f build_type=release \
  -f ffmpeg_ref=df21143bf252528f45d7ae56cc1d317ff00d4449
```

That source run's x64 artifact is ID `10316414903` with metadata digest
`sha256:03c9cfa6728323197db16eb371c9b87c56b5bd97ecf3c8e235c4458c90fc20ab`.
The helper discovers it by exact name within the explicit run and verifies the
downloaded archive against that digest before extraction.

The retry helper verifies:

- repository, workflow ID, run ID, event, completion state, and exact source
  SHA;
- that any source-run failure came only from the Azure publication step;
- successful architecture build and GitHub artifact-upload steps;
- exactly one non-expired artifact with the expected architecture/build name;
- the downloaded archive SHA-256 against GitHub artifact metadata;
- exact `build-info.txt` values for mpv, FFmpeg, libplacebo, target
  architecture, and build type, plus a Clang compiler identity;
- required `libmpv-2.dll`, `libmpv-2.pdb`, and `build-info.txt` files.

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
run conclusion, GitHub artifact ID and archive digest, build provenance,
per-file SHA-256 values, destinations, retry counts, and the separate commit
that supplied the publication tooling. A failed source workflow is never
relabeled as successful; it is accepted only when all failures are confined
to Azure publication and the reusable artifact passed every verification
above.
