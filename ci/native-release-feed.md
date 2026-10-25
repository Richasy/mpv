# Windows runtime publication

## Azure CI

The `libmpv` workflow supports GitHub artifacts, Azure Blob Storage, or both.
For a Player release, select `upload_target=azure` and enable both x64 and
ARM64. The workflow uploads complete run-specific runtime sets and updates
`native/latest.json` only after validating both architectures and their source
commit. Runner selection retains its explicit or time-of-day policy.
No shared native directory or `NATIVE_RELEASE_ROOT` repository variable is
required. Player consumes the completed pair through its canonical Azure
downloader and verifies its run-owned receipt against both Store packages.

## Explicit offline publication

`ci/publish_native_release.py` remains available for separate offline
publication; it is not a workflow upload target. Select an existing writable
directory explicitly with `--root`. Its Linux/WSL path and a Windows consumer's path must
refer to the same persistent storage. Keep it outside Actions checkouts,
`RUNNER_TEMP`, and mutable compiler output. No machine-specific path belongs
in the workflow. Only the trusted native publisher should have write access;
consumers need read access.

Each architecture is copied from the completed build into
`native/builds/<run-id>-<attempt>/<x64|arm64>`. The publisher rejects missing
runtime dependencies, non-release output, wrong `build-info.txt` commit or
architecture, and changed bytes. Its architecture manifest records every DLL,
PDB, and notice with its size and SHA-256. Existing architecture directories
are never overwritten.

After both architecture `stage` commands succeed, `complete` verifies their source,
workflow attempt, and bytes again, writes `manifest.json`, and atomically
replaces `native/latest.json`. The completion pointer has this shape:

```json
{
  "schema": "libmpv.runtime-set.pointer.v1",
  "path": "native/builds/123-1/manifest.json",
  "sha256": "<manifest-sha256>"
}
```

The selected manifest uses `libmpv.runtime-set.v1` and contains `mpvCommit`,
`runId`, `runAttempt`, `platforms: ["x64", "arm64"]`, and `files` with
`platform`, `path` (relative to the manifest directory), `size`, and `sha256`.
Consumers freeze the pointer and manifest once and validate copied bytes
against that document; a newer build never retargets an in-flight consumer.
They must not consume the mutable `OUTPUT_DIR` directly.

An incomplete local set has no completion pointer. Stage both completed
architectures under a new paired attempt; do not combine outputs from different attempts
or overwrite an existing set. An interrupted completion retains its immutable
files. Repeating `complete` for that exact attempt verifies identical bytes
and resumes pointer publication, but cannot roll back a newer pointer. The
`.native-publication.lock` file carries an OS-owned lock that is released on
process exit, including termination; keep the lock file itself in place.
Consumers never remove native sets, and pruning must exclude
versions retained by release jobs.

Product-specific static assets are separate from the libmpv runtime set.
The Player publisher can seed its `static/` set from a previous verified
acquisition receipt; this helper never relabels arbitrary models or vendor
DLLs as outputs of the mpv build.

Run the publication contract tests without native compilation:

```text
python3 -B -m unittest discover -s ci/tests -p test_native_release.py -v
```
