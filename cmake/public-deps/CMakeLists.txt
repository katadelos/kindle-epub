cmake_minimum_required(VERSION 3.24)
project(kindle_epub_public_dependencies LANGUAGES C)

include(FetchContent)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POLICY_DEFAULT_CMP0063 NEW)
# cJSON still declares CMake 3.0 compatibility; current CMake requires 3.5+.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(CMAKE_INSTALL_LIBDIR lib CACHE STRING "Library directory" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz
    URL_HASH SHA256=d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# EPUB uses the XML/HTML tree parser and serializer, with character conversion.
# Leave out network access, validation, querying and standalone programs.
foreach(feature C14N CATALOG DEBUG DOCS HTTP ICU LEGACY MODULES PATTERN
        PROGRAMS PUSH PYTHON READLINE READER REGEXPS RELAXNG SAX1 SCHEMAS
        SCHEMATRON TESTS THREAD_ALLOC VALID WRITER XINCLUDE XPATH XPTR ZLIB)
    set(LIBXML2_WITH_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(feature HTML ICONV OUTPUT THREADS)
    set(LIBXML2_WITH_${feature} ON CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libxml2
    URL https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz
    URL_HASH SHA256=98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(zlib libxml2)

# libzip uses the zlib target built above, never a host or toolchain copy.
set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}z${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}")
# libzip exports its build targets. Treat zlib as an external dependency there,
# while retaining the local build dependency and generated zconf.h include path.
add_library(ZLIB::ZLIB STATIC IMPORTED GLOBAL)
set_target_properties(ZLIB::ZLIB PROPERTIES
    IMPORTED_LOCATION "${ZLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}")
add_dependencies(ZLIB::ZLIB zlibstatic)
set(ZLIB_LINK_LIBRARY_NAME z)
foreach(feature COMMONCRYPTO GNUTLS OPENSSL WINDOWS_CRYPTO BZIP2 LZMA ZSTD)
    set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(component TOOLS REGRESS OSSFUZZ EXAMPLES DOC)
    set(BUILD_${component} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(libzip
    URL https://libzip.org/download/libzip-1.12.1.tar.xz
    URL_HASH SHA256=c40062022a1963c80573d48f6070792297cecd7c6bffe3aefcac0d8b9bee8913
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# The firmware has its own incompatible cJSON ABI. Keep this copy private.
set(ENABLE_PUBLIC_SYMBOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_HIDDEN_SYMBOLS ON CACHE BOOL "" FORCE)
foreach(feature CUSTOM_COMPILER_FLAGS CJSON_TEST CJSON_UTILS CJSON_UNINSTALL TARGET_EXPORT)
    set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
endforeach()
FetchContent_Declare(cjson
    URL https://codeload.github.com/DaveGamble/cJSON/tar.gz/refs/tags/v1.7.19
    URL_HASH SHA256=7fa616e3046edfa7a28a32d5f9eacfd23f92900fe1f8ccd988c1662f30454562
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(libzip cjson)

install(FILES "${zlib_SOURCE_DIR}/LICENSE"
    DESTINATION share/licenses/kindle-epub RENAME zlib.txt)
install(FILES "${libxml2_SOURCE_DIR}/Copyright"
    DESTINATION share/licenses/kindle-epub RENAME libxml2.txt)
install(FILES "${libzip_SOURCE_DIR}/LICENSE"
    DESTINATION share/licenses/kindle-epub RENAME libzip.txt)
install(FILES "${cjson_SOURCE_DIR}/LICENSE"
    DESTINATION share/licenses/kindle-epub RENAME cJSON.txt)
