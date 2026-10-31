cmake_minimum_required(VERSION 3.24)

option(EPUB_BUILD_RUNTIME "Build the complete runtime for one Kindle ABI" OFF)
if(EPUB_BUILD_RUNTIME)
    project(kindle_epub_runtime LANGUAGES CXX)
    include(cmake/Runtime.cmake)
    return()
endif()

project(kindle_epub_native LANGUAGES CXX)
include(CMakeDependentOption)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Every project target uses the same language, warning and include settings.
# Firmware string ABI is a separate usage requirement for shared C++ objects.
add_library(epub_compile_options INTERFACE)
target_include_directories(epub_compile_options INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(epub_compile_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror=return-type>)

option(EPUB_BUILD_TESTS "Build host-side EPUB parser tests" ON)
option(EPUB_BUILD_MOBI8_MODULE "Build the firmware Mobi8 ABI adapter" OFF)
option(EPUB_BUILD_SCANNER_MODULE "Build the firmware CCAT extractor" OFF)
option(EPUB_BUILD_KPP_MODULE "Build the in-process KPP adapter" OFF)
option(EPUB_BUILD_FRAMEWORK_SUPPORT "Build Java activation support" OFF)
option(EPUB_BUILD_CATALOG_WORKER "Build the out-of-process catalogue worker" OFF)
option(EPUB_BUILD_NATIVE_PROBE "Build on-device native interface discovery" OFF)
cmake_dependent_option(EPUB_BUILD_CONTROL "Build the native runtime control CLI" ON
    "CMAKE_SYSTEM_NAME MATCHES Linux|Buildroot" OFF)
option(EPUB_ENABLE_TRACE "Enable diagnostic device syslog tracing" OFF)
set(EPUB_CXX11_ABI "0" CACHE STRING
    "libstdc++ dual ABI used by the target firmware (0 or 1)")
if(NOT EPUB_CXX11_ABI MATCHES "^[01]$")
    message(FATAL_ERROR "EPUB_CXX11_ABI must be 0 or 1")
endif()

add_library(epub_abi INTERFACE)
target_compile_features(epub_abi INTERFACE cxx_std_11)
target_compile_definitions(epub_abi INTERFACE
    _FILE_OFFSET_BITS=64
    _GLIBCXX_USE_CXX11_ABI=${EPUB_CXX11_ABI})
target_include_directories(epub_abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

option(EPUB_DEVICE_BUILD "Build with a supplied Kindle cross-toolchain" OFF)
set(EPUB_ARCHITECTURE "" CACHE STRING "Kindle CPU family: kindle5, kindlepw2, kindlehf")
set(EPUB_MOBI8_TARGET "external" CACHE STRING "Target label reported by the Mobi8 adapter")
set(EPUB_MOBI8_PROFILE "mobi8" CACHE STRING "Native interface compatibility profile")
set(EPUB_KPP_TARGET "${EPUB_MOBI8_TARGET}" CACHE STRING "Target label reported by the KPP adapter")

if(EPUB_DEVICE_BUILD)
    if(NOT CMAKE_CROSSCOMPILING OR NOT EPUB_ARCHITECTURE MATCHES "^(kindle5|kindlepw2|kindlehf)$")
        message(FATAL_ERROR "A Kindle toolchain and EPUB_ARCHITECTURE are required")
    endif()
    if(NOT EPUB_MOBI8_PROFILE MATCHES "^(k4|kt1|pw1|mobi8-unsized|mobi8)$")
        message(FATAL_ERROR "Unknown EPUB_MOBI8_PROFILE")
    endif()
    if(EPUB_BUILD_TESTS)
        message(FATAL_ERROR "EPUB_BUILD_TESTS must be OFF for a Kindle cross-build")
    endif()
    if(EPUB_MOBI8_PROFILE STREQUAL "k4" OR EPUB_MOBI8_PROFILE STREQUAL "kt1")
        target_compile_definitions(epub_compile_options INTERFACE KINDLE_EPUB_EARLY_MOBI8_ABI=1)
    endif()
    if(EPUB_ARCHITECTURE STREQUAL "kindle5")
        target_compile_definitions(epub_compile_options INTERFACE KINDLE_EPUB_OLD_STDCXX=1)
    endif()
    # Keep adapters compatible with libstdc++ runtimes predating sized delete.
    target_compile_options(epub_compile_options INTERFACE -fno-sized-deallocation -Wno-psabi)
    if(EPUB_MOBI8_PROFILE STREQUAL "k4")
        target_compile_definitions(epub_compile_options INTERFACE KINDLE_EPUB_K4_ABI=1)
        set(EPUB_MOBI8_EXTRA_SOURCE src/mobi8/k4_webreader.cpp src/mobi8/k4_discovery.cpp)
    elseif(EPUB_MOBI8_PROFILE STREQUAL "kt1")
        target_compile_definitions(epub_compile_options INTERFACE KINDLE_EPUB_KT1_ABI=1)
    elseif(EPUB_MOBI8_PROFILE STREQUAL "pw1")
        target_compile_definitions(epub_compile_options INTERFACE KINDLE_EPUB_PW1_ABI=1)
    endif()
    set(EPUB_DEVICE_COMPAT_SOURCE src/runtime/device_compat.cpp)
endif()

find_package(PkgConfig REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson>=1.7.17)
if(EPUB_DEVICE_BUILD)
    set(EPUB_LIBZIP_ARCHIVE "" CACHE FILEPATH "Private libzip archive")
    if(NOT EXISTS "${EPUB_LIBZIP_ARCHIVE}")
        message(FATAL_ERROR "Supply EPUB_LIBZIP_ARCHIVE from the runtime build")
    endif()
    pkg_check_modules(LIBZIP REQUIRED libzip>=1.11.4)
    add_library(epub_zip STATIC IMPORTED)
    set_target_properties(epub_zip PROPERTIES
        IMPORTED_LOCATION "${EPUB_LIBZIP_ARCHIVE}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBZIP_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
    target_link_options(epub_compile_options INTERFACE -Wl,--exclude-libs,ALL)
    set(EPUB_ZIP_LIBRARY epub_zip)
else()
    pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip>=1.11.4)
    set(EPUB_ZIP_LIBRARY PkgConfig::LIBZIP)
endif()
set(EPUB_PLATFORM_LIBRARIES PkgConfig::LIBXML2 ${EPUB_ZIP_LIBRARY} ZLIB::ZLIB)
set(EPUB_CJSON_LIBRARY PkgConfig::CJSON)

add_library(epub_core STATIC
    src/epub/archive.cpp
    src/epub/xml.cpp
    src/epub/identity.cpp
    src/epub/package.cpp
    src/epub/path.cpp
)

target_link_libraries(epub_core PRIVATE epub_compile_options)

target_link_libraries(epub_core
    PUBLIC
        epub_abi
        Threads::Threads
        ${EPUB_PLATFORM_LIBRARIES}
)

add_library(epub_reader STATIC
    src/reader/book.cpp
    src/reader/document.cpp
    src/reader/css.cpp
    src/reader/text.cpp
)
target_link_libraries(epub_reader PUBLIC epub_core PRIVATE epub_compile_options)

add_library(kindle_epub_kpp_gate STATIC src/kpp/gate.cpp)

target_link_libraries(kindle_epub_kpp_gate PUBLIC epub_abi PRIVATE epub_compile_options)

add_library(kindle_epub_catalog STATIC
    src/scanner/catalog.cpp
    src/scanner/worker_request.cpp
)

target_link_libraries(kindle_epub_catalog PRIVATE epub_compile_options)
target_link_libraries(kindle_epub_catalog PUBLIC epub_core PRIVATE ${EPUB_CJSON_LIBRARY})

if(EPUB_BUILD_TESTS)
    enable_testing()
    add_executable(epub_core_tests tests/epub_core_tests.cpp)
    target_link_libraries(epub_core_tests PRIVATE epub_compile_options)
    target_link_libraries(epub_core_tests PRIVATE epub_reader archive)
    add_test(NAME epub_core_tests COMMAND epub_core_tests)

    add_executable(kindle_catalog_tests tests/catalog_tests.cpp)
    target_link_libraries(kindle_catalog_tests PRIVATE epub_compile_options)
    target_link_libraries(kindle_catalog_tests PRIVATE
        kindle_epub_catalog
        ${EPUB_CJSON_LIBRARY}
    )
    add_test(NAME kindle_catalog_tests COMMAND kindle_catalog_tests)

    add_executable(kindle_kpp_gate_tests tests/kpp_gate_tests.cpp)

    target_link_libraries(kindle_kpp_gate_tests PRIVATE epub_compile_options)
    target_link_libraries(kindle_kpp_gate_tests PRIVATE kindle_epub_kpp_gate)
    add_test(NAME kindle_kpp_gate_tests COMMAND kindle_kpp_gate_tests)

    add_executable(kindle_scanner_extractor_tests
        tests/scanner_extractor_tests.cpp
        src/scanner/extractor.cpp
    )

    target_link_libraries(kindle_scanner_extractor_tests PRIVATE epub_compile_options)
    target_link_libraries(kindle_scanner_extractor_tests PRIVATE
        kindle_epub_catalog
        archive
    )
    add_test(NAME kindle_scanner_extractor_tests
             COMMAND kindle_scanner_extractor_tests)
endif()

if(EPUB_BUILD_SCANNER_MODULE)
    if(NOT EPUB_DEVICE_BUILD)
        message(FATAL_ERROR
            "EPUB_BUILD_SCANNER_MODULE requires EPUB_DEVICE_BUILD")
    endif()
    add_library(kindle_epub_extractor SHARED
        src/scanner/extractor.cpp
        src/scanner/stock_api.cpp
        ${EPUB_DEVICE_COMPAT_SOURCE}
    )
    target_link_libraries(kindle_epub_extractor PRIVATE epub_compile_options)
    target_compile_definitions(kindle_epub_extractor PRIVATE
        KINDLE_EPUB_SCANNER_TARGET="${EPUB_MOBI8_TARGET}"
    )
    target_link_libraries(kindle_epub_extractor PRIVATE
        kindle_epub_catalog
        ${CMAKE_DL_LIBS}
    )
    set_target_properties(kindle_epub_extractor PROPERTIES
        OUTPUT_NAME kindle-epub-extractor
        VERSION 0.1.0
        SOVERSION 0
        LINK_FLAGS
            "-Wl,-z,defs,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/scanner/exports.map"
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/scanner/exports.map"
    )
endif()

set(EPUB_MOBI8_SOURCES
    src/mobi8/model.cpp
    src/mobi8/content.cpp
    src/mobi8/fragments.cpp
    src/mobi8/words.cpp
    src/mobi8/navigation.cpp
    src/mobi8/metadata.cpp
    src/mobi8/resource.cpp
)

if(EPUB_BUILD_KPP_MODULE OR
   (EPUB_BUILD_TESTS AND EPUB_BUILD_MOBI8_MODULE))
    add_library(kindle_epub_model_kpp STATIC ${EPUB_MOBI8_SOURCES})
    target_link_libraries(kindle_epub_model_kpp PRIVATE epub_compile_options)
    target_compile_definitions(kindle_epub_model_kpp PUBLIC
        KINDLE_EPUB_KPP_ABI=1
    )
    if(EPUB_ENABLE_TRACE)
        target_compile_definitions(kindle_epub_model_kpp PRIVATE
            KINDLE_EPUB_TRACE=1
        )
    endif()
    target_link_libraries(kindle_epub_model_kpp PUBLIC epub_reader)

    if(EPUB_BUILD_TESTS)
        add_executable(kindle_mobi8_kpp_adapter_tests
            tests/mobi8_adapter_tests.cpp
        )
        target_link_libraries(kindle_mobi8_kpp_adapter_tests PRIVATE epub_compile_options)
        target_link_libraries(kindle_mobi8_kpp_adapter_tests PRIVATE
            kindle_epub_model_kpp
            archive
        )
        add_test(NAME kindle_mobi8_kpp_adapter_tests
                 COMMAND kindle_mobi8_kpp_adapter_tests)
    endif()
endif()

if(EPUB_BUILD_NATIVE_PROBE)
    add_executable(kindle-epub-probe
        src/runtime/native_probe.cpp src/mobi8/k4_discovery.cpp src/kpp/discovery.cpp
        ${EPUB_DEVICE_COMPAT_SOURCE})
    target_link_libraries(kindle-epub-probe PRIVATE epub_compile_options)
    target_compile_definitions(kindle-epub-probe PRIVATE _GLIBCXX_USE_CXX11_ABI=0)
    target_compile_options(kindle-epub-probe PRIVATE -fno-sized-deallocation)
endif()

if(EPUB_BUILD_FRAMEWORK_SUPPORT)
    if(NOT EPUB_DEVICE_BUILD)
        message(FATAL_ERROR "EPUB_BUILD_FRAMEWORK_SUPPORT requires EPUB_DEVICE_BUILD")
    endif()
    find_path(EPUB_JDK_INCLUDE_ROOT jni.h
        HINTS "$ENV{JAVA_HOME}/include" "/usr/lib/jvm/default-java/include"
        NO_CMAKE_FIND_ROOT_PATH)
    find_path(EPUB_JDK_PLATFORM_INCLUDE jni_md.h
        HINTS "${EPUB_JDK_INCLUDE_ROOT}/linux" "${EPUB_JDK_INCLUDE_ROOT}/darwin"
        NO_CMAKE_FIND_ROOT_PATH)
    if(NOT EPUB_JDK_INCLUDE_ROOT OR NOT EPUB_JDK_PLATFORM_INCLUDE)
        message(FATAL_ERROR "Set EPUB_JDK_INCLUDE_ROOT to JDK headers containing jni.h")
    endif()
    add_library(kindle_epub_java_agent SHARED src/runtime/java_agent.cpp)
    target_link_libraries(kindle_epub_java_agent PRIVATE epub_compile_options)
    target_include_directories(kindle_epub_java_agent PRIVATE
        ${EPUB_JDK_INCLUDE_ROOT} ${EPUB_JDK_PLATFORM_INCLUDE})
    target_compile_options(kindle_epub_java_agent PRIVATE -fno-exceptions -fno-rtti)
    set_target_properties(kindle_epub_java_agent PROPERTIES
        OUTPUT_NAME kindle-epub-java-agent)
    add_library(kindle_epub_framework_exec SHARED src/runtime/framework_exec.cpp)
    target_link_libraries(kindle_epub_framework_exec PRIVATE epub_compile_options)
    target_compile_options(kindle_epub_framework_exec PRIVATE -fno-exceptions -fno-rtti)
    target_link_libraries(kindle_epub_framework_exec PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(kindle_epub_framework_exec PROPERTIES
        OUTPUT_NAME kindle-epub-framework-exec)
endif()

if(EPUB_BUILD_CATALOG_WORKER)
    if(NOT EPUB_DEVICE_BUILD)
        message(FATAL_ERROR "EPUB_BUILD_CATALOG_WORKER requires EPUB_DEVICE_BUILD")
    endif()
    add_executable(kindle_epub_catalog_import
        src/scanner/worker_main.cpp
        src/scanner/worker_catalog.cpp
        src/scanner/worker_check.cpp
        src/scanner/worker_server.cpp
        src/scanner/worker_thumbnail.cpp
        src/scanner/stock_api.cpp
        ${EPUB_DEVICE_COMPAT_SOURCE}
    )
    target_link_libraries(kindle_epub_catalog_import PRIVATE epub_compile_options)
    target_link_libraries(kindle_epub_catalog_import PRIVATE
        kindle_epub_catalog ${CMAKE_DL_LIBS})
    set_target_properties(kindle_epub_catalog_import PROPERTIES
        OUTPUT_NAME kindle-epub-import)
endif()

if(EPUB_BUILD_KPP_MODULE)
    if(NOT EPUB_DEVICE_BUILD)
        message(FATAL_ERROR "EPUB_BUILD_KPP_MODULE requires EPUB_DEVICE_BUILD")
    endif()
    enable_language(ASM)
    add_library(kindle_epub_kpp SHARED
        src/kpp/hook.cpp
        src/kpp/discovery.cpp
        src/kpp/trampolines.S
        ${EPUB_DEVICE_COMPAT_SOURCE}
    )
    target_link_libraries(kindle_epub_kpp PRIVATE epub_compile_options)
    target_compile_definitions(kindle_epub_kpp PRIVATE
        KINDLE_EPUB_KPP_ABI=1
        KINDLE_EPUB_KPP_CXX11_ABI=${EPUB_CXX11_ABI}
        KINDLE_EPUB_KPP_TARGET="${EPUB_KPP_TARGET}"
    )
    target_link_libraries(kindle_epub_kpp PRIVATE
        kindle_epub_model_kpp
        kindle_epub_kpp_gate
        ${CMAKE_DL_LIBS}
    )
    set_target_properties(kindle_epub_kpp PROPERTIES
        OUTPUT_NAME kindle-epub-kpp
        VERSION 0.1.0
        SOVERSION 0
        LINK_FLAGS
            "-Wl,-z,defs,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/kpp/exports.map"
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/kpp/exports.map"
    )
endif()

if(EPUB_BUILD_MOBI8_MODULE)
    add_library(kindle_epub_model STATIC ${EPUB_MOBI8_SOURCES})
    target_link_libraries(kindle_epub_model PRIVATE epub_compile_options)
    if(EPUB_ENABLE_TRACE)
        target_compile_definitions(kindle_epub_model PRIVATE
            KINDLE_EPUB_TRACE=1
        )
    endif()
    target_link_libraries(kindle_epub_model PUBLIC epub_reader)

    add_library(kindle_epub SHARED
        src/mobi8/module.cpp
        ${EPUB_MOBI8_EXTRA_SOURCE}
        ${EPUB_DEVICE_COMPAT_SOURCE}
    )

    target_link_libraries(kindle_epub PRIVATE epub_compile_options)
    target_compile_definitions(kindle_epub PRIVATE
        KINDLE_EPUB_MOBI8_TARGET="${EPUB_MOBI8_TARGET}"
    )
    target_link_libraries(kindle_epub PRIVATE kindle_epub_model ${CMAKE_DL_LIBS})
    set_target_properties(kindle_epub PROPERTIES
        OUTPUT_NAME kindle-epub
        VERSION 0.1.0
        SOVERSION 0
        LINK_FLAGS
            "-Wl,-z,defs,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/mobi8/exports.map"
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/mobi8/exports.map"
    )

    if(EPUB_BUILD_TESTS)
        add_executable(kindle_mobi8_adapter_tests
            tests/mobi8_adapter_tests.cpp
        )
        target_link_libraries(kindle_mobi8_adapter_tests PRIVATE epub_compile_options)
        target_link_libraries(kindle_mobi8_adapter_tests PRIVATE
            kindle_epub_model
            archive
        )
        add_test(NAME kindle_mobi8_adapter_tests
                 COMMAND kindle_mobi8_adapter_tests)
    endif()
endif()

if(EPUB_BUILD_CONTROL)
    add_executable(kindle_epub_control
        src/control/main.cpp
        src/control/boot.cpp
        src/control/system.cpp
        src/control/runtime.cpp
        src/control/activation.cpp
        src/control/catalogue.cpp
        src/control/worker.cpp)
    target_link_libraries(kindle_epub_control PRIVATE epub_compile_options
        ${EPUB_CJSON_LIBRARY})
    target_compile_definitions(kindle_epub_control PRIVATE
        _FILE_OFFSET_BITS=64 _GLIBCXX_USE_CXX11_ABI=0)
    set_target_properties(kindle_epub_control PROPERTIES OUTPUT_NAME epubctl)
endif()

include(cmake/InstallRuntime.cmake)
include(cmake/Checks.cmake)
epub_add_checks()
