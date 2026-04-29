# Locate host glslc (Google shaderc). ggml-vulkan invokes it during the build
# to compile shaders, so it must be a HOST executable runnable on the build
# machine. The mingw shaderc package built for the Windows target is useless
# here. Install /usr/bin/glslc on the host (Ubuntu: `apt install glslc`).
find_program(HOST_GLSLC NAMES glslc NO_CMAKE_FIND_ROOT_PATH)
if (NOT HOST_GLSLC)
    message(FATAL_ERROR "Host glslc not found. Install Ubuntu package 'glslc'.")
endif()

ExternalProject_Add(whisper
    DEPENDS vulkan vulkan-header
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    SOURCE_DIR ${SOURCE_LOCATION}
    GIT_CLONE_FLAGS "--filter=tree:0"
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ${EXEC} CONF=1 cmake -H<SOURCE_DIR> -B<BINARY_DIR>
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}
        -DCMAKE_INSTALL_PREFIX=${MINGW_INSTALL_PREFIX}
        -DCMAKE_FIND_ROOT_PATH=${MINGW_INSTALL_PREFIX}
        -DBUILD_SHARED_LIBS=ON
        -DGGML_BACKEND_DL=ON
        -DGGML_VULKAN=ON
        -DGGML_NATIVE=OFF
        -DVulkan_GLSLC_EXECUTABLE=${HOST_GLSLC}
        # mpv's vulkan package installs libvulkan.a (BUILD_STATIC_LOADER=ON) but
        # cmake's FindVulkan can't always discover it under mingw cross-compile.
        # Pin the loader and headers explicitly so ggml-vulkan finds Vulkan::Vulkan.
        -DVulkan_LIBRARY=${MINGW_INSTALL_PREFIX}/lib/libvulkan.a
        -DVulkan_INCLUDE_DIR=${MINGW_INSTALL_PREFIX}/include
        -DWHISPER_BUILD_TESTS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
    BUILD_COMMAND ${EXEC} ninja -C <BINARY_DIR>
    INSTALL_COMMAND ${EXEC} ninja -C <BINARY_DIR> install
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

# whisper.cpp + ggml are now built as shared libraries so that:
#   1. libavfilter/af_whisper (static-linked into libmpv-2.dll) and any
#      externally loaded ggml backend dll share ONE process-wide ggml backend
#      registry living inside ggml-base.dll;
#   2. GGML_BACKEND_DL=ON makes every backend (CPU, Vulkan, ...) build as an
#      independent MODULE library (ggml-cpu.dll, ggml-vulkan.dll) that exports
#      `ggml_backend_init` and is loaded at runtime via ggml_backend_load() /
#      ggml_backend_load_all_from_path(). Without DL=ON, the previously-shipped
#      ggml-vulkan.dll exported only `ggml_backend_vk_reg` (old static API)
#      and ggml_backend_load() failed silently with "Failed to load ggml backend".
#   3. The vulkan backend is built here (instead of being shipped from a separate
#      release) so its ggml-base ABI matches the one libmpv links against.
#
# Required runtime layout (deployed alongside libmpv-2.dll):
#   libmpv-2.dll, ggml.dll, ggml-base.dll, libwhisper.dll  -> root
#   ggml-cpu.dll, ggml-vulkan.dll                          -> any directory
#       passed to af_whisper as `backend_path` (the directory of the dll path)
#       — both CPU and the GPU backend get loaded by load_all_from_path.

force_rebuild_git(whisper)
cleanup(whisper install)
