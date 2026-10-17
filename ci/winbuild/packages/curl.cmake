ExternalProject_Add(curl
    DEPENDS
        openssl
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG eaa9103225bff16bdf1bb2842f312897b1342e85
    SOURCE_DIR ${SOURCE_LOCATION}
    GIT_CLONE_FLAGS "--filter=tree:0"
    UPDATE_COMMAND ""
    PATCH_COMMAND ${EXEC} git apply ${CMAKE_CURRENT_SOURCE_DIR}/curl-0001-ip-family-preference.patch
    CONFIGURE_COMMAND ${EXEC} autoreconf -fi && CONF=1 <SOURCE_DIR>/configure
        --host=${TARGET_ARCH}
        --prefix=${MINGW_INSTALL_PREFIX}
        --disable-shared
        --with-openssl
        --without-libpsl
    BUILD_COMMAND ${MAKE}
    INSTALL_COMMAND ${MAKE} install
    BUILD_IN_SOURCE 1
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

force_rebuild_git(curl)
cleanup(curl install)

# Download the cacert.pem file: https://curl.haxx.se/docs/caextract.html
# Rename the cacert.pem file to curl-ca-bundle.crt
# Put in same directory as curl.exe
