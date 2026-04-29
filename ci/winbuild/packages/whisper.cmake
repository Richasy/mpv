ExternalProject_Add(whisper
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
        -DWHISPER_BUILD_TESTS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
    BUILD_COMMAND ${EXEC} ninja -C <BINARY_DIR>
    INSTALL_COMMAND ${EXEC} ninja -C <BINARY_DIR> install
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

# whisper.cpp + ggml are now built as shared libraries so that:
#   1. libavfilter/af_whisper (static-linked into libmpv-2.dll) and any externally
#      loaded ggml backend (ggml-vulkan.dll / ggml-cuda.dll) share ONE process-wide
#      ggml backend registry living inside ggml-base.dll;
#   2. Otherwise (BUILD_SHARED_LIBS=OFF), libmpv-2.dll would carry a private static
#      copy of ggml core, and any ggml_backend_load() of an external GPU backend
#      would register into the new dll's own registry, invisible to af_whisper —
#      forcing CPU inference even when a GPU backend dll is provided.
#
# With shared linkage the installed pkg-config file already references the correct
# import libraries, no post-install fix-up is needed.

force_rebuild_git(whisper)
cleanup(whisper install)
