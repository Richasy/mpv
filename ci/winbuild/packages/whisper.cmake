ExternalProject_Add(whisper
    DEPENDS
        vulkan
        vulkan-header
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
        # Build as shared libs so each backend (ggml-cpu, ggml-vulkan, ggml-cuda)
        # can be discovered at runtime by ggml_backend_load_all().  This lets us
        # ship Vulkan as the default GPU path and lets callers (Rodel.Player)
        # drop in ggml-cuda.dll alongside libmpv-2.dll for CUDA acceleration
        # without any rebuild of mpv.
        -DBUILD_SHARED_LIBS=ON
        -DGGML_BACKEND_DL=ON
        -DGGML_NATIVE=OFF
        # Vulkan backend.  Cross-compiled to Windows; the Vulkan loader/headers
        # come from our own vulkan/vulkan-header packages.  glslc must be
        # available on the BUILD HOST (Linux) — install via:
        #   sudo apt-get install -y glslc       (Ubuntu 22.04+)
        # or by extracting glslc from a LunarG Vulkan SDK / shaderc release.
        -DGGML_VULKAN=ON
        -DGGML_VULKAN_RUN_TESTS=OFF
        -DGGML_VULKAN_CHECK_RESULTS=OFF
        -DWHISPER_BUILD_TESTS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
    BUILD_COMMAND ${EXEC} ninja -C <BINARY_DIR>
    INSTALL_COMMAND ${EXEC} ninja -C <BINARY_DIR> install
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

# With BUILD_SHARED_LIBS=ON the install layout is:
#   bin/  whisper.dll, ggml.dll, ggml-base.dll, ggml-cpu*.dll, ggml-vulkan.dll
#   lib/  libwhisper.dll.a, libggml*.dll.a (proper mingw import libs, no fixup
#         needed)
#   lib/pkgconfig/whisper.pc  (generated correctly by upstream cmake for shared
#                              builds — Libs: -L${libdir} -lwhisper)
#
# We don't need the static-build pc fixup / lib*.a symlinks anymore.

force_rebuild_git(whisper)
cleanup(whisper install)
