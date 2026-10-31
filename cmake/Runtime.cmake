include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/Java.cmake")

set(EPUB_ARCHITECTURE "" CACHE STRING "Kindle ABI")
if(NOT CMAKE_CROSSCOMPILING OR NOT EPUB_ARCHITECTURE MATCHES "^(kindle5|kindlepw2|kindlehf)$")
    message(FATAL_ERROR "Supply a cross-toolchain file and EPUB_ARCHITECTURE")
endif()
option(EPUB_ENABLE_TRACE "Enable diagnostic device syslog tracing" OFF)

epub_add_java_runtime()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/architectures.json"
    "${PROJECT_SOURCE_DIR}/src/mobi8/contracts.json")
file(READ "${PROJECT_SOURCE_DIR}/architectures.json" matrix)
file(READ "${PROJECT_SOURCE_DIR}/src/mobi8/contracts.json" contracts)
string(JSON profile_count LENGTH "${contracts}")
math(EXPR last_profile "${profile_count} - 1")
set(contract_rows "")
foreach(profile_index RANGE ${last_profile})
    string(JSON profile MEMBER "${contracts}" ${profile_index})
    string(JSON class_count LENGTH "${contracts}" "${profile}")
    math(EXPR last_class "${class_count} - 1")
    foreach(class_index RANGE ${last_class})
        string(JSON class MEMBER "${contracts}" "${profile}" ${class_index})
        string(JSON method_count LENGTH "${contracts}" "${profile}" "${class}")
        math(EXPR last_method "${method_count} - 1")
        foreach(method_index RANGE ${last_method})
            string(JSON method GET "${contracts}" "${profile}" "${class}" ${method_index})
            string(APPEND contract_rows "${profile}\t${class}\t${method}\n")
        endforeach()
    endforeach()
endforeach()
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/native-contracts.tsv"
    CONTENT "${contract_rows}" @ONLY)
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/runtime.conf" CONTENT
    "epub_package_format=2\nepub_architecture=${EPUB_ARCHITECTURE}\n" @ONLY)

# All firmware variants use the supplied toolchain, sysroot and dependency sources.
set(cross_arguments "")
foreach(variable CMAKE_TOOLCHAIN_FILE CMAKE_BUILD_TYPE CMAKE_MAKE_PROGRAM
        CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER CMAKE_C_FLAGS
        CMAKE_CXX_FLAGS CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS
        EPUB_JDK_INCLUDE_ROOT PKG_CONFIG_EXECUTABLE)
    if(DEFINED ${variable})
        list(APPEND cross_arguments "-D${variable}:STRING=${${variable}}")
    endif()
endforeach()

# The firmware loads adapters globally. Shared libzip pulls newer OpenSSL into
# the stock reader and interposes on its incompatible OpenSSL 1.0 API. EPUB
# supports only unencrypted store/deflate entries, so build a private archive
# from the supplied source, once per ABI and without downloads.
set(EPUB_LIBZIP_SOURCE_DIR "" CACHE PATH "Prepared libzip source directory")
if(NOT EXISTS "${EPUB_LIBZIP_SOURCE_DIR}/lib/zip.h")
    message(FATAL_ERROR "Supply EPUB_LIBZIP_SOURCE_DIR matching the sysroot headers")
endif()
set(private_zip "${PROJECT_BINARY_DIR}/libzip/lib/libzip.a")
ExternalProject_Add(epub_private_zip
    SOURCE_DIR "${EPUB_LIBZIP_SOURCE_DIR}"
    BINARY_DIR "${PROJECT_BINARY_DIR}/libzip"
    CMAKE_ARGS ${cross_arguments}
        -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
        -DBUILD_SHARED_LIBS:BOOL=OFF
        -DENABLE_COMMONCRYPTO:BOOL=OFF
        -DENABLE_GNUTLS:BOOL=OFF
        -DENABLE_MBEDTLS:BOOL=OFF
        -DENABLE_OPENSSL:BOOL=OFF
        -DENABLE_WINDOWS_CRYPTO:BOOL=OFF
        -DENABLE_BZIP2:BOOL=OFF
        -DENABLE_LZMA:BOOL=OFF
        -DENABLE_ZSTD:BOOL=OFF
        -DBUILD_TOOLS:BOOL=OFF
        -DBUILD_REGRESS:BOOL=OFF
        -DBUILD_OSSFUZZ:BOOL=OFF
        -DBUILD_EXAMPLES:BOOL=OFF
        -DBUILD_DOC:BOOL=OFF
    BUILD_BYPRODUCTS "${private_zip}"
    INSTALL_COMMAND "")
set(stage "${PROJECT_BINARY_DIR}/stage")
add_custom_target(runtime ALL DEPENDS kindle-epub-java)
string(JSON variant_count LENGTH "${matrix}" "${EPUB_ARCHITECTURE}" variants)
math(EXPR last_variant "${variant_count} - 1")
foreach(index RANGE ${last_variant})
    string(JSON profile GET "${matrix}" "${EPUB_ARCHITECTURE}" variants ${index} profile)
    string(JSON abi GET "${matrix}" "${EPUB_ARCHITECTURE}" variants ${index} abi)
    set(common OFF)
    set(modern ON)
    set(plugin OFF)
    set(worker OFF)
    if(index EQUAL 0)
        set(common ON)
    endif()
    if(EPUB_ARCHITECTURE STREQUAL "kindle5")
        set(modern OFF)
    elseif(common)
        set(worker ON)
    endif()
    if(profile STREQUAL "kt1" OR profile STREQUAL "pw1")
        set(plugin ON)
    endif()
    set(variant "epub_${profile}_abi${abi}")
    ExternalProject_Add(${variant}
        DEPENDS epub_private_zip
        SOURCE_DIR "${PROJECT_SOURCE_DIR}"
        BINARY_DIR "${PROJECT_BINARY_DIR}/${profile}/abi${abi}"
        CMAKE_ARGS ${cross_arguments}
            "-DCMAKE_INSTALL_PREFIX:PATH=${stage}"
            "-DEPUB_DEVICE_BUILD:BOOL=ON"
            "-DEPUB_LIBZIP_ARCHIVE:FILEPATH=${private_zip}"
            "-DEPUB_ARCHITECTURE:STRING=${EPUB_ARCHITECTURE}"
            "-DEPUB_CXX11_ABI:STRING=${abi}"
            "-DEPUB_MOBI8_PROFILE:STRING=${profile}"
            "-DEPUB_MOBI8_TARGET:STRING=${EPUB_ARCHITECTURE}"
            "-DEPUB_KPP_TARGET:STRING=${EPUB_ARCHITECTURE}"
            "-DEPUB_BUILD_TESTS:BOOL=OFF"
            "-DEPUB_BUILD_MOBI8_MODULE:BOOL=ON"
            "-DEPUB_BUILD_FRAMEWORK_SUPPORT:BOOL=${common}"
            "-DEPUB_BUILD_NATIVE_PROBE:BOOL=${common}"
            "-DEPUB_BUILD_CONTROL:BOOL=${common}"
            "-DEPUB_BUILD_KPP_MODULE:BOOL=${modern}"
            "-DEPUB_BUILD_CATALOG_WORKER:BOOL=${worker}"
            "-DEPUB_BUILD_SCANNER_MODULE:BOOL=${plugin}"
            "-DEPUB_ENABLE_TRACE:BOOL=${EPUB_ENABLE_TRACE}"
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --component Runtime
        BUILD_ALWAYS TRUE)
    add_dependencies(runtime ${variant})
endforeach()

install(DIRECTORY "${stage}/" DESTINATION . USE_SOURCE_PERMISSIONS)
install(FILES "${PROJECT_BINARY_DIR}/runtime.conf" "${EPUB_JAVA_JAR}"
    "${PROJECT_BINARY_DIR}/native-contracts.tsv" DESTINATION share/kindle-epub)
