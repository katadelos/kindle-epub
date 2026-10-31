include(ExternalProject)
include("${CMAKE_CURRENT_LIST_DIR}/AcquireToolchain.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/Java.cmake")

set(EPUB_ARCHITECTURES "kindle5;kindlepw2;kindlehf" CACHE STRING
    "Kindle architecture packages to build")
option(EPUB_ENABLE_TRACE "Enable diagnostic device syslog tracing" OFF)
set(architectures ${EPUB_ARCHITECTURES})
list(REMOVE_DUPLICATES architectures)
if(NOT architectures OR NOT "${architectures}" STREQUAL "${EPUB_ARCHITECTURES}")
    message(FATAL_ERROR "EPUB_ARCHITECTURES must be a nonempty list without duplicates")
endif()

find_package(Java REQUIRED COMPONENTS Development)
get_filename_component(javac "${Java_JAVAC_EXECUTABLE}" REALPATH)
get_filename_component(java_bin "${javac}" DIRECTORY)
get_filename_component(java_home "${java_bin}" DIRECTORY)
set(EPUB_JDK_INCLUDE_ROOT "${java_home}/include" CACHE PATH "Host JDK headers for JNI")
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

# Forward standard FetchContent source overrides for offline dependency builds.
set(dependency_sources "")
foreach(dependency ZLIB LIBXML2 LIBZIP CJSON)
    if(DEFINED FETCHCONTENT_SOURCE_DIR_${dependency})
        list(APPEND dependency_sources
            "-DFETCHCONTENT_SOURCE_DIR_${dependency}:PATH=${FETCHCONTENT_SOURCE_DIR_${dependency}}")
    endif()
endforeach()

# The architecture payloads have identical relative paths: install one at a time.
list(LENGTH architectures architecture_count)
install(CODE "
    set(epub_install_architectures \"${architectures}\")
    list(FIND epub_install_architectures \"\${CMAKE_INSTALL_COMPONENT}\" epub_component_index)
    if(CMAKE_INSTALL_COMPONENT AND epub_component_index EQUAL -1)
        message(FATAL_ERROR \"Unknown architecture component: \${CMAKE_INSTALL_COMPONENT}\")
    elseif(NOT CMAKE_INSTALL_COMPONENT AND ${architecture_count} GREATER 1)
        message(FATAL_ERROR \"Select an architecture with --component for installation\")
    endif()
" ALL_COMPONENTS)

add_custom_target(runtime ALL DEPENDS kindle-epub-java)

foreach(architecture IN LISTS architectures)
    if(NOT architecture MATCHES "^(kindle5|kindlepw2|kindlehf)$")
        message(FATAL_ERROR "Unknown architecture: ${architecture}")
    endif()
    string(TOUPPER "${architecture}" architecture_upper)
    epub_acquire_toolchain("${architecture}" toolchain_root toolchain_target)
    set(toolchain "${PROJECT_SOURCE_DIR}/cmake/${architecture}-toolchain.cmake")
    set(build "${PROJECT_BINARY_DIR}/${architecture}")
    set(dependencies "${build}/dependencies")
    set(stage "${build}/stage")
    set(cross_arguments
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${toolchain}"
        "-D${architecture_upper}_TOOLCHAIN_ROOT:PATH=${toolchain_root}"
        "-DCMAKE_BUILD_TYPE:STRING=Release"
        "-DCMAKE_C_COMPILER_LAUNCHER:STRING=${CMAKE_C_COMPILER_LAUNCHER}"
        "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING=${CMAKE_CXX_COMPILER_LAUNCHER}")
    ExternalProject_Add(epub_dependencies_${architecture}
        SOURCE_DIR "${PROJECT_SOURCE_DIR}/cmake/public-deps"
        BINARY_DIR "${build}/dependencies-build"
        CMAKE_ARGS ${cross_arguments} ${dependency_sources}
            "-DCMAKE_INSTALL_PREFIX:PATH=${dependencies}"
        BUILD_ALWAYS TRUE
        DEPENDS ${toolchain_target})

    string(JSON variant_count LENGTH "${matrix}" "${architecture}" variants)
    math(EXPR last_variant "${variant_count} - 1")
    foreach(index RANGE ${last_variant})
        string(JSON profile GET "${matrix}" "${architecture}" variants ${index} profile)
        string(JSON abi GET "${matrix}" "${architecture}" variants ${index} abi)
        set(common OFF)
        set(modern ON)
        set(plugin OFF)
        set(worker OFF)
        if(index EQUAL 0)
            set(common ON)
        endif()
        if(architecture STREQUAL "kindle5")
            set(modern OFF)
        elseif(common)
            set(worker ON)
        endif()
        if(profile STREQUAL "kt1" OR profile STREQUAL "pw1")
            set(plugin ON)
        endif()
        ExternalProject_Add(epub_${architecture}_${profile}_abi${abi}
            SOURCE_DIR "${PROJECT_SOURCE_DIR}"
            BINARY_DIR "${build}/${profile}/abi${abi}"
            CMAKE_ARGS ${cross_arguments}
                "-DEPUB_DEVICE_BUILD:BOOL=ON"
                "-DEPUB_ARCHITECTURE:STRING=${architecture}"
                "-DEPUB_DEPENDENCIES:PATH=${dependencies}"
                "-DEPUB_CXX11_ABI:STRING=${abi}"
                "-DEPUB_MOBI8_PROFILE:STRING=${profile}"
                "-DEPUB_MOBI8_TARGET:STRING=${architecture}"
                "-DEPUB_KPP_TARGET:STRING=${architecture}"
                "-DEPUB_JDK_INCLUDE_ROOT:PATH=${EPUB_JDK_INCLUDE_ROOT}"
                "-DEPUB_BUILD_TESTS:BOOL=OFF"
                "-DEPUB_BUILD_MOBI8_MODULE:BOOL=ON"
                "-DEPUB_BUILD_FRAMEWORK_SUPPORT:BOOL=${common}"
                "-DEPUB_BUILD_NATIVE_PROBE:BOOL=${common}"
                "-DEPUB_BUILD_CONTROL:BOOL=${common}"
                "-DEPUB_BUILD_KPP_MODULE:BOOL=${modern}"
                "-DEPUB_BUILD_CATALOG_WORKER:BOOL=${worker}"
                "-DEPUB_BUILD_SCANNER_MODULE:BOOL=${plugin}"
                "-DEPUB_ENABLE_TRACE:BOOL=${EPUB_ENABLE_TRACE}"
            INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR>
                --prefix "${stage}" --component Runtime --strip
            BUILD_ALWAYS TRUE
            DEPENDS epub_dependencies_${architecture})
        add_dependencies(runtime epub_${architecture}_${profile}_abi${abi})

        set(native "native/${profile}/abi${abi}")
        set(libraries "${stage}/${native}/libkindle-epub.so")
        if(modern)
            list(APPEND libraries "${stage}/${native}/libkindle-epub-kpp.so")
        endif()
        install(FILES ${libraries} DESTINATION "${native}" COMPONENT ${architecture})
        if(plugin)
            install(FILES "${stage}/native/${profile}/libkindle-epub-extractor.so"
                DESTINATION "native/${profile}" COMPONENT ${architecture})
        endif()
    endforeach()

    file(CONFIGURE OUTPUT "${build}/runtime.conf" CONTENT
        "epub_package_format=2\nepub_architecture=${architecture}\n" @ONLY)
    install(FILES "${build}/runtime.conf" "${EPUB_JAVA_JAR}"
        "${PROJECT_BINARY_DIR}/native-contracts.tsv"
        "${stage}/libkindle-epub-framework-exec.so"
        "${stage}/libkindle-epub-java-agent.so"
        DESTINATION . COMPONENT ${architecture})
    install(PROGRAMS "${stage}/epubctl" "${stage}/kindle-epub-probe"
        DESTINATION . COMPONENT ${architecture})
    if(NOT architecture STREQUAL "kindle5")
        install(PROGRAMS "${stage}/kindle-epub-import"
            DESTINATION . COMPONENT ${architecture})
    endif()
    foreach(license zlib libxml2 libzip cJSON)
        install(FILES "${dependencies}/share/licenses/kindle-epub/${license}.txt"
            DESTINATION licenses COMPONENT ${architecture})
    endforeach()
    install(FILES "${toolchain_root}/share/licenses/gcc/COPYING3"
        DESTINATION licenses RENAME GCC-GPL-3.0.txt COMPONENT ${architecture})
    install(FILES "${toolchain_root}/share/licenses/gcc/COPYING.RUNTIME"
        DESTINATION licenses RENAME GCC-Runtime-Library-Exception.txt COMPONENT ${architecture})
    set(CPACK_ARCHIVE_${architecture_upper}_FILE_NAME "kindle-epub-${architecture}")
endforeach()

set(CPACK_GENERATOR TGZ)
set(CPACK_COMPONENTS_ALL ${architectures})
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_GROUPING IGNORE)
set(CPACK_PACKAGING_INSTALL_PREFIX /kindle-epub)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
include(CPack)
