set_property(DIRECTORY PROPERTY EP_STEP_TARGETS download)
include(${PROJECT_SOURCE_DIR}/cmake/packages_check.cmake)
list(APPEND ep
    gmp
    xz
    lzo
    uchardet
    libsamplerate
    rubberband
    zlib
    zstd
    bzip2
    libjpeg
    lcms2
    ogg
    vorbis
    opus-dnn
    opus
    speex
    libvpx
    libpng
    libdvdcss
    libdvdread
    libdvdnav
    libiconv
    luajit
    fribidi
    expat
    nettle
    mbedtls
    libxml2
    brotli
    freetype2
    libudfread
    libbluray
    harfbuzz
    libsoxr
    graphengine
    libzimg
    fontconfig
    libunibreak
    libass
    subrandr
    libvpl
    amf-headers
    nvcodec-headers
    libmysofa
    xxhash
    dav1d
    libwebp
    libbs2b
    openssl
    libssh
    libsrt
    libarchive
    highway
    libjxl
    angle-headers
    mujs
    spirv-headers
    spirv-tools
    spirv-cross
    glslang
    vulkan-header
    vulkan
    openal-soft
    shaderc
    glad
    fast_float
    libplacebo
    libaribcaption
    libsixel
    whisper
    ffmpeg
    curl
    mpv
    mpv-release
    mpv-packaging
    opusfile
)
foreach(package IN LISTS ep)
    if(NOT ${SINGLE_SOURCE_LOCATION} STREQUAL "")
        set(SOURCE_LOCATION "${SINGLE_SOURCE_LOCATION}/${package}")
    endif()
    include(${CMAKE_CURRENT_SOURCE_DIR}/${package}.cmake)
    set(SOURCE_LOCATION "")

    get_property(_git_repository TARGET ${package} PROPERTY _EP_GIT_REPOSITORY)
    if(_git_repository)
        list(APPEND repo ${package})
    endif()
endforeach()
list(LENGTH ep ep_length)
message(STATUS "Parsing ${ep_length} packages")

# Exclude packages which dont depend on mpv when update
# NOTE: curl is NOT excluded — mpv now links libcurl for its stream_curl
# backend (DEPENDS curl, meson -Dlibcurl=enabled), so it must participate in
# the download/update/clean cycle and build before mpv. curl links the
# already-built openssl (same TLS backend ffmpeg/libssh use), so mbedtls
# stays excluded/unused.
list(APPEND removed "opusfile" "libsixel" "mbedtls" "libressl" "nettle" "gmp")
list(REMOVE_ITEM repo ${removed})
list(REMOVE_ITEM ep ${removed})
list(TRANSFORM repo APPEND "-force-update" OUTPUT_VARIABLE update)
list(TRANSFORM ep APPEND "-download" OUTPUT_VARIABLE download)
list(TRANSFORM ep APPEND "-fullclean" OUTPUT_VARIABLE fullclean)

add_custom_target(update
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${update}
    COMMENT "Updating git repo"
)

add_custom_target(download
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${download}
)

add_custom_target(package-fullclean
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target ${fullclean}
    COMMENT "Remove all stamp files, reset git HEAD"
)

add_custom_target(cargo-clean
    COMMAND ${EXEC} LD_PRELOAD= cargo install --git https://github.com/matthiaskrgr/cargo-cache.git --no-default-features --features ci-autoclean cargo-cache
    COMMAND ${EXEC} cargo-cache
    COMMAND du -sh ${RUSTUP_LOCATION}
    USES_TERMINAL
)
