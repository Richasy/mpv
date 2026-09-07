![mpv logo](https://raw.githubusercontent.com/mpv-player/mpv.io/master/source/images/mpv-logo-128.png)

# mpv (Richasy Fork)

This is a fork of [mpv](https://github.com/mpv-player/mpv) with modifications for native Windows application integration.

The primary integration uses `vo=gpu-next`, `gpu-api=d3d11`, and
`d3d11-output-mode=composition`. A host such as Rodel.Player's Sprout application
embeds the D3D11 swapchain through the retained composition-surface API. The
host supplies its window with `d3d11-composition-hwnd`; mpv remains responsible
for rendering, presentation, resizing, and HDR metadata.

Additionally, this fork adds support for **Blu-ray ISO playback over HTTP**. By leveraging libbluray's `bd_open_stream()` API with a custom block-read callback backed by mpv's HTTP stream layer, Blu-ray ISO files hosted on remote servers (e.g. cloud storage, NAS with HTTP access) can be played directly:

```bash
mpv bd:// --bluray-device=https://example.com/movie.iso
mpv bd://0 --bluray-device=https://example.com/movie.iso
```

The HTTP server must support `Range` requests for seeking to work (most servers do: nginx, Apache, S3, cloud storage).

Local Blu-ray, DVD-Video, and DVD-Audio ISO files can be opened directly. mpv
probes the enabled disc readers and falls back to regular file handling if none
recognizes the image. HTTP ISO playback retains the fork's block cache and
filename detection inside URL query strings:

```bash
mpv /path/to/movie.iso
```

The fork is maintained as a `libmpv` build target — it produces `libmpv-2.dll` for Windows x64 and arm64.

* [Compilation](#compilation)
* [Upstream](#upstream)


## Upstream

This fork is based on [mpv-player/mpv](https://github.com/mpv-player/mpv). For general documentation, please refer to:

* [Wiki](https://github.com/mpv-player/mpv/wiki)
* [FAQ](https://github.com/mpv-player/mpv/wiki/FAQ)
* [Manual](https://mpv.io/manual/master/)

## Compilation

This fork uses a bundled copy of [mpv-winbuild-cmake](https://github.com/shinchiro/mpv-winbuild-cmake) (under `ci/winbuild/`) for cross-compiling `libmpv-2.dll` from Linux using the Clang/LLVM MinGW toolchain.

### Prerequisites

A Linux host (Ubuntu 24.04 recommended) with the build script's dependencies installed:

```bash
bash ci/build-libmpv.sh --install-deps
```

### Building

The build is driven by `ci/build-libmpv.sh`, which takes its configuration via environment variables:

```bash
export BUILD_DIR=/path/to/build_x86_64
export CLANG_ROOT=/path/to/clang_root
export SRC_PACKAGES=/path/to/src_packages
export RUSTUP_LOC=$CLANG_ROOT/install_rustup
export OUTPUT_DIR=/path/to/output
export TARGET_ARCH=x86_64   # or aarch64

bash ci/build-libmpv.sh all
```

The script will patch `mpv.cmake` to point to this fork, build the LLVM toolchain (first run only), compile all dependencies, and produce `libmpv-2.dll` in `$OUTPUT_DIR/$TARGET_ARCH/`.

### CI

The GitHub Actions workflow (`.github/workflows/libmpv.yml`) is configured for manual dispatch only (`workflow_dispatch`). It runs on a self-hosted Linux runner and outputs artifacts to a local directory.

Use the workflow's `ffmpeg_ref` input (or `FFMPEG_COMMIT` for the build script)
to build an exact companion FFmpeg revision without changing the default branch.
It defaults to `master`. For a coordinated upgrade, select the committed FFmpeg
SHA and use `upload_target=github` until the artifact set is ready to publish.
GitHub-only runs omit ARM64. The `build_arm64` input is honored only when
`upload_target` is `azure` or `both`; ordinary artifact builds use x64.

## Upstream integration: 2026-09-07

This integration merges mpv-player/mpv through `989d32716e` and coordinates
with Richasy/FFmpeg's upstream merge through `6d87581efa`. The current rendering
dependency is libplacebo 7.371.0 (`3330a515d6`); CI records its exact source
commit alongside the mpv and FFmpeg commits in `build-info.txt`.

| Area | Update and fork compatibility |
| --- | --- |
| Subtitle changes | Subtitle-option updates now rebuild from cached subtitle packets instead of refreshing the demuxer. This is distinct from switching a real audio/subtitle track. |
| Track switching | Keep `demuxer-cache-preserve-on-track-switch=yes` as an opt-in for uninterrupted bystander queues, plus the always-on Whisper/CC virtual-track bypass. A real track switch can still seek the source to backfill the new track; upstream's subtitle-option improvement does not replace this workaround. |
| Dolby Vision | Adopt Profile 7 base/enhancement-layer splitting, pairing, hardware-surface budgeting, and `gpu-next` composition for supported containers. Enhancement-layer rendering needs libplacebo API 367 or newer. Output is mapped to the configured HDR/SDR target, not native HDMI Dolby Vision passthrough. |
| FFmpeg | Include the Dolby Vision RPU padding fix, propagation of HDR side data across decoder threads after flushes, HTTP/container fixes, DVD-Audio PCM decoding, and raw DSD support. Preserve the fork's H.264/AVBufferRef hardening, HTTP redirect/header handling, and asynchronous Whisper pipeline. |
| D3D11/HDR | Use upstream's unified high-bit-depth and 4:4:4 format mapping, which also covers the fork's BGRA/10-bit RGB/float RGB fixes. System reference-white detection also uses the composition host window. Keep the richer RTX HDR source/display guards and `vf-metadata` status contract. |
| Disc playback | Adopt DVD/HDMV/BD-J menu support and local ISO probing; remote DVD ISO playback shares the menu-aware initialization path. Keep public angle properties 1-based after upstream makes Blu-ray stream controls 1-based too. DVD-Audio requires libdvdread 7.1.1 or newer; BD-J additionally requires a usable Java runtime and libbluray.jar. |
| Network playback | Include stricter HTTP range-response handling, encrypted nested-stream support, and cancellation of obsolete blocking demux reads. Keep byte-range LRU caching, expiring-redirect recovery, and fatal stream-error reporting. |

The Rodel.Player contract remains `libmpv-2.dll` with client API 2.7, retained
`mpv_acquire_d3d11_composition_surface` /
`mpv_release_d3d11_composition_surface`, `MPV_EVENT_TRACK_FAILED = 26`, and the
existing track IDs, GPU-adapter identity, RIFE, Whisper, thumbnail, subtitle
layout, and statistics interfaces. The consumer already represents optional
`ff-index` values as nullable, as required by upstream.

The integration also keeps remote ISO backends in the new navigation lifecycle,
excludes tiled-image filter graphs from enhancement-layer pairing, and gives
blended-subtitle overlay arrays and parts frame-local ownership.

Do not remove the consumer's external-player IPC, `sub-add`, playlist, or
`--start` sequencing workarounds on account of the subtitle-option change.
Source merges also do not update an installed player: publish the matching
native artifact set before refreshing the consumer's size/SHA-256-pinned
`eng/native-dependencies.json`. Never change only the recorded commit IDs while
leaving the old artifact hashes in place.


## License

GPLv2 "or later" by default, LGPLv2.1 "or later" with `-Dgpl=false`.
See [details](https://github.com/mpv-player/mpv/blob/master/Copyright).
