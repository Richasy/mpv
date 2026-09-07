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
#   FFMPEG_COMMIT - Richasy/FFmpeg git commit/branch/tag (default: master)
#   MPV_SRC_DIR   - path to mpv source (for copying headers)
#   BUILD_TYPE    - 'release' (default) or 'debug'. Debug switches mpv meson
#                   options to -Doptimization=0 -Db_lto=false -Db_ndebug=false
#                   so PDBs are stepable and asserts are kept on. PDB=1 is
#                   already always passed to ninja so PDBs are produced for
#                   release builds too (handy for crash-dump symbolication).
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
BUILD_TYPE="${BUILD_TYPE:-release}"
case "$BUILD_TYPE" in
    release|debug) ;;
    *) err "BUILD_TYPE must be 'release' or 'debug', got: $BUILD_TYPE" ;;
esac

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
        python3-pip docbook2x unzip p7zip-full curl ccache \
        glslc

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
    log "Patching mpv.cmake (repo: $MPV_REPO, commit: $MPV_COMMIT, build: $BUILD_TYPE)..."
    local mpv_cmake="$WINBUILD_DIR/packages/mpv.cmake"

    if [ ! -f "$mpv_cmake.orig" ]; then
        cp "$mpv_cmake" "$mpv_cmake.orig"
    fi

    cp "$mpv_cmake.orig" "$mpv_cmake"

    sed -i "s|GIT_REPOSITORY https://github.com/Richasy/mpv.git|GIT_REPOSITORY ${MPV_REPO}|" "$mpv_cmake"
    sed -i "s|GIT_TAG master|GIT_TAG ${MPV_COMMIT}|" "$mpv_cmake"

    if [ "$BUILD_TYPE" = "debug" ]; then
        log "Switching meson options to debug build (no optimization, no LTO, asserts on)..."
        sed -i "s|-Db_ndebug=true|-Db_ndebug=false|" "$mpv_cmake"
        sed -i "s|-Doptimization=3|-Doptimization=0|" "$mpv_cmake"
        sed -i "s|-Db_lto=true|-Db_lto=false|" "$mpv_cmake"
    fi

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

    # Preflight: ggml-vulkan needs a HOST glslc (Google shaderc) to compile shaders.
    # The mingw-cross shaderc dll built for the Windows target is useless here.
    # Self-hosted runners need /usr/bin/glslc installed (Ubuntu pkg: glslc).
    if ! command -v glslc >/dev/null 2>&1; then
        err "host glslc not found in PATH (required by ggml-vulkan). Install Ubuntu pkg 'glslc' on the runner."
    fi
    log "Host glslc: $(command -v glslc) ($(glslc --version 2>&1 | head -n1))"

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

    # Force amf-headers re-clone + re-install so the freshly-cloned ffmpeg's
    # newer AMF code builds against up-to-date AMF SDK headers. ffmpeg is
    # re-cloned every build (above), but the AMF headers were previously left
    # cached/stale; when upstream ffmpeg added AMFSurface1 host-memory mapping
    # (libavutil/hwcontext_amf.c, 2026-06) the stale headers broke the build:
    #   error: unknown type name 'AMFSurface1'; did you mean 'AMFSurface'?
    # Removing the source dir before cmake configure makes force_rebuild_git
    # generate a proper fresh clone; wiping the installed AMF dir forces re-copy.
    log "Removing amf-headers cache to force re-clone..."
    rm -rf "$SRC_PACKAGES/amf-headers" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/amf-headers-prefix/src/amf-headers-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/${ARCH}-w64-mingw32/include/AMF" 2>/dev/null || true

    # Force curl re-clone (newly enabled mpv dep for the stream_curl backend).
    # Wiping the whole curl-prefix avoids stale ExternalProject step definitions
    # lingering from earlier runs (e.g. a removed PATCH_COMMAND) and tracks
    # curl.git master cleanly. mbedtls (curl's TLS backend) is left cached; it
    # is pinned to a fixed tag and builds deterministically.
    log "Removing curl cache to force re-clone..."
    rm -rf "$SRC_PACKAGES/curl" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/curl-prefix" 2>/dev/null || true

    # Force whisper re-build
    log "Removing whisper cache to force rebuild..."
    rm -rf "$SRC_PACKAGES/whisper" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/whisper-prefix/src/whisper-stamp" 2>/dev/null || true
    rm -rf "$BUILD_DIR/packages/whisper-prefix/src/whisper-build" 2>/dev/null || true
    # Wipe stale ggml/whisper install artifacts. Switching whisper.cpp between
    # BUILD_SHARED_LIBS ON/OFF leaves both libggml.a (static archive) and
    # libggml.dll.a (import library) in the prefix; the linker may then pick
    # the wrong one (e.g. linking against libggml.a's ggml-backend-reg.cpp
    # symbols when ggml-cpu has been moved into ggml-cpu.dll, producing
    # "undefined symbol: ggml_backend_cpu_reg").
    log "Wiping stale ggml/whisper install artifacts..."
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/lib/"libwhisper.{a,dll.a} 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/lib/"whisper.{a,dll.a} 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/lib/"libggml*.{a,dll.a} 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/lib/"ggml*.{a,dll.a} 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/bin/"libwhisper.dll 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/bin/"whisper.dll 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/bin/"libggml*.dll 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/bin/"ggml*.dll 2>/dev/null || true
    rm -f "$BUILD_DIR/${ARCH}-w64-mingw32/lib/pkgconfig/whisper.pc" 2>/dev/null || true

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

    # Wipe any stale artifacts from previous runs so removed dependencies
    # (e.g. nvngx_vsr.dll, NvOFFRUC.dll, onnxruntime.dll, DirectML.dll)
    # do not leak back into the uploaded archive.
    log "Wiping previous artifacts at $ARCH_OUTPUT..."
    rm -rf "$ARCH_OUTPUT"
    mkdir -p "$ARCH_OUTPUT/include/mpv"

    log "Collecting build artifacts for $ARCH..."

    # Copy ONNX Runtime + DirectML DLLs for vf_rife (silently skip if SDK
    # is not provisioned on this runner).
    local ORT_PKG="/home/richasy/programs/microsoft.ml.onnxruntime.directml"
    local DML_PKG="/home/richasy/programs/microsoft.ai.directml"
    local ORT_ARCH_DIR DML_ARCH_DIR
    if [ "$TARGET_ARCH" = "aarch64" ]; then
        ORT_ARCH_DIR="win-arm64"
        DML_ARCH_DIR="arm64-win"
    else
        ORT_ARCH_DIR="win-x64"
        DML_ARCH_DIR="x64-win"
    fi
    local ORT_DLL="$ORT_PKG/runtimes/$ORT_ARCH_DIR/native/onnxruntime.dll"
    local ORT_SHARED_DLL="$ORT_PKG/runtimes/$ORT_ARCH_DIR/native/onnxruntime_providers_shared.dll"
    local DML_DLL="$DML_PKG/bin/$DML_ARCH_DIR/DirectML.dll"
    if [ -f "$ORT_DLL" ]; then
        cp "$ORT_DLL" "$ARCH_OUTPUT/"
        log "onnxruntime.dll copied ($ORT_ARCH_DIR)"
    else
        warn "onnxruntime.dll not found at $ORT_DLL, skipping"
    fi
    if [ -f "$ORT_SHARED_DLL" ]; then
        cp "$ORT_SHARED_DLL" "$ARCH_OUTPUT/"
        log "onnxruntime_providers_shared.dll copied ($ORT_ARCH_DIR)"
    fi
    if [ -f "$DML_DLL" ]; then
        cp "$DML_DLL" "$ARCH_OUTPUT/"
        log "DirectML.dll copied ($DML_ARCH_DIR)"
    else
        warn "DirectML.dll not found at $DML_DLL, skipping"
    fi

    # Find and copy libmpv-2.dll (and libmpv-2.pdb when produced - PDB=1 is
    # always passed to the linker so PDBs exist for both release and debug
    # builds; debug builds simply produce more useful PDBs).
    local MPV_DEV_DIR=$(find "$BUILD_DIR" -path "*/mpv-dev*" -type d 2>/dev/null | head -1)
    local DLL_DIR=""
    if [ -n "$MPV_DEV_DIR" ] && [ -f "$MPV_DEV_DIR/libmpv-2.dll" ]; then
        cp "$MPV_DEV_DIR/libmpv-2.dll" "$ARCH_OUTPUT/"
        cp "$MPV_DEV_DIR/libmpv.dll.a" "$ARCH_OUTPUT/" 2>/dev/null || true
        DLL_DIR="$MPV_DEV_DIR"
        log "libmpv-2.dll copied from $MPV_DEV_DIR"
    else
        local DLL_PATH=$(find "$BUILD_DIR" -name "libmpv-2.dll" -type f 2>/dev/null | head -1)
        if [ -n "$DLL_PATH" ]; then
            cp "$DLL_PATH" "$ARCH_OUTPUT/"
            DLL_DIR=$(dirname "$DLL_PATH")
            cp "$DLL_DIR/libmpv.dll.a" "$ARCH_OUTPUT/" 2>/dev/null || true
            log "libmpv-2.dll copied from $DLL_DIR"
        else
            err "libmpv-2.dll not found for $ARCH"
        fi
    fi

    # copy-binary preserves the PDB in mpv-dev before winbuild cleanup removes
    # the linker BINARY_DIR. Keep a fallback search for older build trees.
    local PDB_PATH=""
    if [ -n "$DLL_DIR" ] && [ -f "$DLL_DIR/libmpv-2.pdb" ]; then
        PDB_PATH="$DLL_DIR/libmpv-2.pdb"
    else
        PDB_PATH=$(find "$BUILD_DIR" -name "libmpv-2.pdb" -type f 2>/dev/null | head -1)
    fi
    if [ -n "$PDB_PATH" ] && [ -f "$PDB_PATH" ]; then
        cp "$PDB_PATH" "$ARCH_OUTPUT/"
        log "libmpv-2.pdb copied from $(dirname "$PDB_PATH") ($(du -h "$PDB_PATH" | cut -f1))"
    else
        err "libmpv-2.pdb not found - refusing to publish an unsymbolizable build"
    fi

    local MPV_BUILD_COMMIT="${MPV_COMMIT:-unknown}"
    local FFMPEG_BUILD_COMMIT="unknown"
    local LIBPLACEBO_BUILD_COMMIT="unknown"
    local COMPILER_VERSION="unknown"
    local PDB_GUID="unknown"
    if [ -n "${MPV_SRC_DIR:-}" ] && git -C "$MPV_SRC_DIR" rev-parse HEAD >/dev/null 2>&1; then
        MPV_BUILD_COMMIT=$(git -C "$MPV_SRC_DIR" rev-parse HEAD)
    fi
    if git -C "$SRC_PACKAGES/ffmpeg" rev-parse HEAD >/dev/null 2>&1; then
        FFMPEG_BUILD_COMMIT=$(git -C "$SRC_PACKAGES/ffmpeg" rev-parse HEAD)
    fi
    if git -C "$SRC_PACKAGES/libplacebo" rev-parse HEAD >/dev/null 2>&1; then
        LIBPLACEBO_BUILD_COMMIT=$(git -C "$SRC_PACKAGES/libplacebo" rev-parse HEAD)
    fi
    if [ -x "$CLANG_ROOT/bin/clang" ]; then
        COMPILER_VERSION=$("$CLANG_ROOT/bin/clang" --version | head -n1)
    fi
    if [ -x "$CLANG_ROOT/bin/llvm-readobj" ]; then
        PDB_GUID=$("$CLANG_ROOT/bin/llvm-readobj" --coff-debug-directory \
            "$ARCH_OUTPUT/libmpv-2.dll" |
            sed -n 's/.*PDBGUID: {\(.*\)}/\1/p' | head -n1)
        PDB_GUID="${PDB_GUID:-unknown}"
    fi
    cat > "$ARCH_OUTPUT/build-info.txt" <<EOF
mpv_commit=$MPV_BUILD_COMMIT
ffmpeg_commit=$FFMPEG_BUILD_COMMIT
libplacebo_commit=$LIBPLACEBO_BUILD_COMMIT
target_arch=$TARGET_ARCH
build_type=$BUILD_TYPE
compiler=$COMPILER_VERSION
pdb_guid=$PDB_GUID
EOF
    log "Build metadata written to $ARCH_OUTPUT/build-info.txt"

    # Copy ggml/whisper shared libraries. whisper.cpp is now built as
    # BUILD_SHARED_LIBS=ON so af_whisper (static-linked into libmpv-2.dll)
    # and any externally loaded ggml backend dll (ggml-vulkan.dll /
    # ggml-cuda.dll) share a single ggml backend registry living inside
    # ggml-base.dll. These four DLLs MUST be deployed alongside libmpv-2.dll.
    local MINGW_PREFIX_DIR="$BUILD_DIR/${ARCH}-w64-mingw32"
    local GGML_BIN_DIR="$MINGW_PREFIX_DIR/bin"
    local ggml_dll_count=0
    for dll in libwhisper.dll libggml.dll libggml-base.dll libggml-cpu.dll libggml-vulkan.dll \
               whisper.dll ggml.dll ggml-base.dll ggml-cpu.dll ggml-vulkan.dll; do
        if [ -f "$GGML_BIN_DIR/$dll" ]; then
            cp "$GGML_BIN_DIR/$dll" "$ARCH_OUTPUT/"
            ggml_dll_count=$((ggml_dll_count + 1))
            log "  $dll copied"
        fi
    done
    if [ "$ggml_dll_count" -lt 4 ]; then
        err "Only $ggml_dll_count ggml/whisper DLL(s) copied — expected at least 4 (whisper, ggml, ggml-base, ggml-cpu). Check whisper.cmake BUILD_SHARED_LIBS setting."
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
