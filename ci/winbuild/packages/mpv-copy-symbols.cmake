if(EXISTS "${INPUT_FILE}")
    configure_file("${INPUT_FILE}" "${OUTPUT_FILE}" COPYONLY)
endif()
