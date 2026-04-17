#!/bin/bash
set -e

# =============================================================================
# Build libmpv-2.dll for Windows using MinGW cross-compilation from Linux
# Uses mpv-winbuild-cmake with Clang toolchain
#
# Required environment variables:
#   BUILD_DIR     - path to build directory (per-arch)
#   CLANG_ROOT    - path to clang/LLVM install prefix
#   SRC_PACKAGES  - path to shared source package cache
#   RUSTUP_LOC    - path to rustup installation
#   OUTPUT_DIR    - path to output directory for artifacts
#   TARGET_ARCH   - x86_64 or aarch64
#
# Optional environment variables:
#   MPV_REPO      - mpv git repository URL (default: https://github.com/Richasy/mpv.git)
#   MPV_COMMIT    - mpv git commit/branch/tag (default: master)
#   MPV_SRC_DIR   - path to mpv source (for copying headers)
# =============================================================================

export PATH="$HOME/.local/bin:$PATH"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[BUILD]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
err() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Validate required env vars
: "${BUILD_DIR:?BUILD_DIR is required}"
: "${CLANG_ROOT:?CLANG_ROOT is required}"
: "${SRC_PACKAGES:?SRC_PACKAGES is required}"
: "${RUSTUP_LOC:?RUSTUP_LOC is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"
: "${TARGET_ARCH:?TARGET_ARCH is required (x86_64 or aarch64)}"

# Auto-detect WINBUILD_DIR from script location
WINBUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/winbuild"

MPV_REPO="${MPV_REPO:-https://github.com/Richasy/mpv.git}"
MPV_COMMIT="${MPV_COMMIT:-master}"

# =============================================================================
# Install prerequisites (run with --install-deps)
# =============================================================================
install_deps() {
    log "Installing build dependencies..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential checkinstall bison flex gettext git mercurial subversion \
        ninja-build gyp cmake yasm nasm automake pkgconf libtool libtool-bin \
        gcc-multilib g++-multilib clang lld libc++1 libc++abi1 \
        libc++-dev libc++abi-dev \
        libgmp-dev libmpfr-dev libmpc-dev libgcrypt-dev \
        gperf ragel texinfo autopoint re2c asciidoc \
        python3-pip docbook2x unzip p7zip-full curl ccache

    pip3 install --break-system-packages --user rst2pdf meson mako jsonschema 2>/dev/null || \
    pip3 install --user rst2pdf meson mako jsonschema

    MESON_VER=$(meson --version 2>/dev/null)
    if [ -z "$MESON_VER" ]; then
        err "meson not found after installation"
    fi
    log "Meson version: $MESON_VER"
}

# =============================================================================
# Patch mpv.cmake to use specified repo and commit
# =============================================================================
patch_mpv_cmake() {
    log "Patching mpv.cmake (repo: $MPV_REPO, commit: $MPV_COMMIT)..."
    local mpv_cmake="$WINBUILD_DIR/packages/mpv.cmake"

    if [ ! -f "$mpv_cmake.orig" ]; then
        cp "$mpv_cmake" "$mpv_cmake.orig"
    fi

    cp "$mpv_cmake.orig" "$mpv_cmake"

    sed -i "s|GIT_REPOSITORY https://github.com/Richasy/mpv.git|GIT_REPOSITORY ${MPV_REPO}|" "$mpv_cmake"
    sed -i "s|GIT_TAG master|GIT_TAG ${MPV_COMMIT}|" "$mpv_cmake"

    log "mpv.cmake patched successfully"
}

# =============================================================================
# Build for the target architecture
# =============================================================================
build() {
    local ARCH="$TARGET_ARCH"
    local MINGW_PREFIX="$BUILD_DIR/${ARCH}-w64-mingw32"

    log "=========================================="
    log "Building libmpv for $ARCH"
    log "=========================================="

    # IMPORTANT: Remove mpv source cache BEFORE cmake configure.
    # custom_steps.cmake's force_rebuild_git() checks if(EXISTS source_dir/.git)
    # at configure time. If the source dir exists, it generates a check-git step
    # that fakes the download stamp, causing ExternalProject to skip git clone.
    # By removing the source dir first, cmake sees no .git and generates a proper
    # clone step instead.
    log "Removing mpv source cache and stamp files to force re-clone..."
    rm -rf "$SRC_PACKAGES/mpv" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/mpv-prefix/src/mpv-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/mpv-prefix/src/mpv-build" 2>/dev/null || true
    rm -rf "$BUILD_DIR/mpv-dev-"* 2>/dev/null || true

    # Force libplacebo re-patch/rebuild by clearing its stamp and build dirs
    log "Removing libplacebo cache to force re-patch and rebuild..."
    rm -rf "$SRC_PACKAGES/libplacebo" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/libplacebo-prefix/src/libplacebo-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/libplacebo-prefix/src/libplacebo-build" 2>/dev/null || true

    # Force ffmpeg re-clone to pick up new configure options (e.g. --enable-whisper)
    log "Removing ffmpeg source cache to force re-clone..."
    rm -rf "$SRC_PACKAGES/ffmpeg" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/ffmpeg-prefix/src/ffmpeg-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/ffmpeg-prefix/src/ffmpeg-build" 2>/dev/null || true

    # Force whisper re-build
    log "Removing whisper cache to force rebuild..."
    rm -rf "$SRC_PACKAGES/whisper" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/whisper-prefix/src/whisper-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/whisper-prefix/src/whisper-build" 2>/dev/null || true

    # Configure CMake
    log "Configuring CMake for $ARCH..."
    cmake \
        -DTARGET_ARCH=${ARCH}-w64-mingw32 \
        -DCOMPILER_TOOLCHAIN=clang \
        -DCMAKE_INSTALL_PREFIX="$CLANG_ROOT" \
        -DMINGW_INSTALL_PREFIX="$MINGW_PREFIX" \
        -DSINGLE_SOURCE_LOCATION="$SRC_PACKAGES" \
        -DRUSTUP_LOCATION="$RUSTUP_LOC" \
        -DENABLE_CCACHE=ON \
        -G Ninja --fresh \
        -B "$BUILD_DIR" \
        -S "$WINBUILD_DIR"

    # Download sources
    log "Downloading sources..."
    ninja -C "$BUILD_DIR" download || true

    # Build LLVM (only needed once, shared across architectures)
    if [ ! -f "$CLANG_ROOT/.llvm_built" ]; then
        log "Building LLVM toolchain..."
        ninja -C "$BUILD_DIR" llvm
        touch "$CLANG_ROOT/.llvm_built"
    else
        log "LLVM already built, skipping..."
    fi

    # Build Rust toolchain
    log "Building Rust toolchain..."
    ninja -C "$BUILD_DIR" rustup || true

    # Build clang for this target
    log "Building clang for $ARCH target..."
    ninja -C "$BUILD_DIR" llvm-clang

    # Build mpv (and all dependencies)
    log "Building mpv and all dependencies for $ARCH..."
    ninja -C "$BUILD_DIR" mpv

    log "$ARCH build complete!"
}

# =============================================================================
# Collect build artifacts
# =============================================================================
collect() {
    local ARCH="$TARGET_ARCH"
    local ARCH_OUTPUT="$OUTPUT_DIR/$ARCH"
    mkdir -p "$ARCH_OUTPUT/include/mpv"

    log "Collecting build artifacts for $ARCH..."

    # Find and copy libmpv-2.dll
    local MPV_DEV_DIR=$(find "$BUILD_DIR" -path "*/mpv-dev*" -type d 2>/dev/null | head -1)
    if [ -n "$MPV_DEV_DIR" ] && [ -f "$MPV_DEV_DIR/libmpv-2.dll" ]; then
        cp "$MPV_DEV_DIR/libmpv-2.dll" "$ARCH_OUTPUT/"
        cp "$MPV_DEV_DIR/libmpv.dll.a" "$ARCH_OUTPUT/" 2>/dev/null || true
        log "libmpv-2.dll copied from $MPV_DEV_DIR"
    else
        local DLL_PATH=$(find "$BUILD_DIR" -name "libmpv-2.dll" -type f 2>/dev/null | head -1)
        if [ -n "$DLL_PATH" ]; then
            cp "$DLL_PATH" "$ARCH_OUTPUT/"
            cp "$(dirname "$DLL_PATH")/libmpv.dll.a" "$ARCH_OUTPUT/" 2>/dev/null || true
            log "libmpv-2.dll copied from $(dirname "$DLL_PATH")"
        else
            err "libmpv-2.dll not found for $ARCH"
        fi
    fi

    # Copy headers
    local HEADERS_SRC="${MPV_SRC_DIR:-}"
    if [ -z "$HEADERS_SRC" ] || [ ! -d "$HEADERS_SRC/include/mpv" ]; then
        HEADERS_SRC=$(find "$BUILD_DIR" -path "*/mpv-prefix/src/mpv" -maxdepth 5 -type d 2>/dev/null | head -1)
    fi
    if [ -n "$HEADERS_SRC" ] && [ -d "$HEADERS_SRC/include/mpv" ]; then
        cp "$HEADERS_SRC/include/mpv/client.h" "$ARCH_OUTPUT/include/mpv/"
        cp "$HEADERS_SRC/include/mpv/stream_cb.h" "$ARCH_OUTPUT/include/mpv/"
        cp "$HEADERS_SRC/include/mpv/render.h" "$ARCH_OUTPUT/include/mpv/"
        cp "$HEADERS_SRC/include/mpv/render_gl.h" "$ARCH_OUTPUT/include/mpv/"
        log "Headers copied to $ARCH_OUTPUT/include/mpv/"
    else
        warn "Headers not found, skipping header copy"
    fi

    log "Artifacts for $ARCH:"
    ls -lh "$ARCH_OUTPUT/"
    ls -lh "$ARCH_OUTPUT/include/mpv/" 2>/dev/null || true
}

# =============================================================================
# Main
# =============================================================================
main() {
    local COMMAND="${1:-all}"

    case "$COMMAND" in
        --install-deps)
            install_deps
            ;;
        --patch)
            patch_mpv_cmake
            ;;
        --build)
            patch_mpv_cmake
            build
            ;;
        --collect)
            collect
            ;;
        all)
            patch_mpv_cmake
            build
            collect
            ;;
        *)
            echo "Usage: $0 [--install-deps|--patch|--build|--collect|all]"
            echo ""
            echo "Commands:"
            echo "  --install-deps  Install build prerequisites"
            echo "  --patch         Patch mpv.cmake only"
            echo "  --build         Patch and build"
            echo "  --collect       Collect artifacts to OUTPUT_DIR"
            echo "  all             Patch, build, and collect (default)"
            echo ""
            echo "Required env vars: BUILD_DIR, CLANG_ROOT,"
            echo "  SRC_PACKAGES, RUSTUP_LOC, OUTPUT_DIR, TARGET_ARCH"
            exit 1
            ;;
    esac
}

main "$@"
