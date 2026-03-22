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
        -DBUILD_SHARED_LIBS=OFF
        -DWHISPER_BUILD_TESTS=OFF
        -DWHISPER_BUILD_EXAMPLES=OFF
        -DWHISPER_BUILD_SERVER=OFF
    BUILD_COMMAND ${EXEC} ninja -C <BINARY_DIR>
    INSTALL_COMMAND ${EXEC} ninja -C <BINARY_DIR> install
    LOG_DOWNLOAD 1 LOG_UPDATE 1 LOG_CONFIGURE 1 LOG_BUILD 1 LOG_INSTALL 1
)

# whisper.cpp installs ggml libraries without the "lib" prefix (ggml.a, ggml-base.a)
# but whisper.pc references them as -lggml -lggml-base which expects libggml.a etc.
# Create symlinks so the linker can find them.
#
# Also fix whisper.pc: the generated Libs line is missing -lggml-cpu (which contains
# ggml_backend_cpu_reg) and has wrong link order. Fix to:
#   Libs: -L${libdir} -lwhisper -lggml-cpu -lggml-base -lggml
ExternalProject_Add_Step(whisper fix-pkgconfig
    DEPENDEES install
    # Create lib-prefixed symlinks
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${MINGW_INSTALL_PREFIX}/lib/ggml.a
        ${MINGW_INSTALL_PREFIX}/lib/libggml.a
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${MINGW_INSTALL_PREFIX}/lib/ggml-base.a
        ${MINGW_INSTALL_PREFIX}/lib/libggml-base.a
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        ${MINGW_INSTALL_PREFIX}/lib/ggml-cpu.a
        ${MINGW_INSTALL_PREFIX}/lib/libggml-cpu.a
    # Fix whisper.pc Libs line: add -lggml-cpu and correct link order
    COMMAND sed -i
        "s|Libs:.*|Libs: -L\$\{libdir\} -lwhisper -lggml-cpu -lggml-base -lggml|"
        ${MINGW_INSTALL_PREFIX}/lib/pkgconfig/whisper.pc
)

force_rebuild_git(whisper)
cleanup(whisper install)
