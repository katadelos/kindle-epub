# ExternalProject's automatic extractor does not recognize .tar.zst in CMake
# 3.24, while the native archive API supports its compression format.
file(ARCHIVE_EXTRACT INPUT "${ARCHIVE}" DESTINATION "${DESTINATION}" TOUCH)
foreach(required "bin/${TRIPLE}-gcc" "bin/${TRIPLE}-g++" "${TRIPLE}/sysroot")
    if(NOT EXISTS "${DESTINATION}/x-tools/${TRIPLE}/${required}")
        message(FATAL_ERROR "KOReader archive is missing ${TRIPLE}/${required}")
    endif()
endforeach()
