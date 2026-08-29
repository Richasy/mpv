![mpv logo](https://raw.githubusercontent.com/mpv-player/mpv.io/master/source/images/mpv-logo-128.png)

# mpv (Richasy Fork)

This is a fork of [mpv](https://github.com/mpv-player/mpv) with modifications to support XAML-based application integration on Windows.

The primary change is adding a custom `vo=d3d11` video output that renders to a shared D3D11 texture, enabling mpv to be embedded into XAML (WinUI 3 / UWP) applications as a render source. This allows building media player applications with native Windows UI frameworks while leveraging mpv's powerful decoding and playback capabilities.

Additionally, this fork adds support for **Blu-ray ISO playback over HTTP**. By leveraging libbluray's `bd_open_stream()` API with a custom block-read callback backed by mpv's HTTP stream layer, Blu-ray ISO files hosted on remote servers (e.g. cloud storage, NAS with HTTP access) can be played directly:

```bash
mpv bd:// --bluray-device=https://example.com/movie.iso
mpv bd://0 --bluray-device=https://example.com/movie.iso
```

The HTTP server must support `Range` requests for seeking to work (most servers do: nginx, Apache, S3, cloud storage).

Local Blu-ray and DVD ISO files can be opened directly. mpv probes Blu-ray
first, then DVD, and falls back to regular file handling if neither recognizes
the image:

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


## License

GPLv2 "or later" by default, LGPLv2.1 "or later" with `-Dgpl=false`.
See [details](https://github.com/mpv-player/mpv/blob/master/Copyright).
