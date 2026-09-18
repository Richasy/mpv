if(NOT DEFINED FFMPEG_CONFIG_MAK)
    message(FATAL_ERROR "FFMPEG_CONFIG_MAK is required")
endif()
if(NOT EXISTS "${FFMPEG_CONFIG_MAK}")
    message(FATAL_ERROR
        "FFmpeg config was not found: ${FFMPEG_CONFIG_MAK}")
endif()

file(STRINGS "${FFMPEG_CONFIG_MAK}" ffmpeg_config)
foreach(feature
    CONFIG_LIBDAVS2_DECODER
    CONFIG_LIBUAVS3D_DECODER
)
    list(FIND ffmpeg_config "${feature}=yes" feature_index)
    if(feature_index EQUAL -1)
        message(FATAL_ERROR
            "Required FFmpeg decoder is disabled: ${feature}")
    endif()
endforeach()

if(DEFINED FFMPEG_ARCHIVE OR DEFINED LLVM_NM)
    if(NOT DEFINED FFMPEG_ARCHIVE OR NOT DEFINED LLVM_NM)
        message(FATAL_ERROR
            "FFMPEG_ARCHIVE and LLVM_NM must be supplied together")
    endif()
    if(NOT EXISTS "${FFMPEG_ARCHIVE}")
        message(FATAL_ERROR
            "FFmpeg archive was not found: ${FFMPEG_ARCHIVE}")
    endif()
    if(NOT EXISTS "${LLVM_NM}")
        message(FATAL_ERROR "llvm-nm was not found: ${LLVM_NM}")
    endif()

    execute_process(
        COMMAND "${LLVM_NM}" --defined-only --extern-only "${FFMPEG_ARCHIVE}"
        RESULT_VARIABLE nm_result
        OUTPUT_VARIABLE nm_output
        ERROR_VARIABLE nm_error
    )
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR
            "llvm-nm could not inspect ${FFMPEG_ARCHIVE}: ${nm_error}")
    endif()

    string(REPLACE "\n" ";" nm_lines "${nm_output}")
    foreach(symbol
        ff_libdavs2_decoder
        ff_libuavs3d_decoder
    )
        set(symbol_found FALSE)
        foreach(line IN LISTS nm_lines)
            if(line MATCHES "[ \t]${symbol}\r?$")
                set(symbol_found TRUE)
                break()
            endif()
        endforeach()
        if(NOT symbol_found)
            message(FATAL_ERROR
                "Required FFmpeg decoder symbol is absent: ${symbol}")
        endif()
    endforeach()
endif()
