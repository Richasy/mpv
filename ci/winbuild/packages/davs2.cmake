set(davs2_patches
    ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0001-enable-10bit-build-and-propagate-frame-packet-position.patch
    ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0010-export-sequence-display-color-description.patch
)
set(davs2_configure_args)

if(TARGET_CPU STREQUAL "aarch64")
    set(davs2_patches
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0001-enable-10bit-build-and-propagate-frame-packet-position.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0002-enable-arm64-neon-detect-and-keep-vectorization.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0003-add-aarch64-neon-primitives-for-copy-add-avg.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0004-add-aarch64-neon-mc-interpolation.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0005-add-aarch64-neon-mc-ext-primitives.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0006-add-aarch64-neon-deblock-luma.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0007-add-aarch64-neon-deblock-chroma.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0008-add-aarch64-neon-intra-basic-10bit.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0009-add-aarch64-neon-intra-bilinear-10bit.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0010-export-sequence-display-color-description.patch
        ${CMAKE_CURRENT_SOURCE_DIR}/davs2-0012-use-standard-setjmp-on-windows-arm64.patch
    )
    list(APPEND davs2_configure_args --disable-asm)
endif()

ExternalProject_Add(davs2
    GIT_REPOSITORY https://github.com/xatabhk/davs2-10bit.git
    GIT_TAG ${WINBUILD_DAVS2_COMMIT}
    SOURCE_DIR ${SOURCE_LOCATION}
    GIT_CLONE_FLAGS "--filter=tree:0"
    UPDATE_COMMAND ""
    PATCH_COMMAND ${EXEC} git apply ${davs2_patches}
    CONFIGURE_COMMAND ${EXEC} cd <SOURCE_DIR>/build/linux &&
        CONF=1 ./configure
        --host=${TARGET_ARCH}
        --cross-prefix=${TARGET_ARCH}-
        --prefix=${MINGW_INSTALL_PREFIX}
        --disable-cli
        --bit-depth=10
        --enable-pic
        ${davs2_configure_args}
    BUILD_COMMAND ${MAKE} -C <SOURCE_DIR>/build/linux
    INSTALL_COMMAND ${MAKE} -C <SOURCE_DIR>/build/linux install
    BUILD_IN_SOURCE 1
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

force_rebuild_git(davs2)
cleanup(davs2 install)
