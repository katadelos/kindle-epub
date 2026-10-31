include_guard(GLOBAL)
include(ExternalProject)

function(epub_acquire_toolchain architecture root_output target_output)
    if(NOT architecture MATCHES "^(kindle5|kindlepw2|kindlehf)$")
        message(FATAL_ERROR "Unknown Kindle architecture: ${architecture}")
    endif()

    string(TOUPPER "${architecture}" architecture_upper)
    set(version "2026.08")
    set(root_variable "${architecture_upper}_TOOLCHAIN_ROOT")
    set(archive_variable "${architecture_upper}_TOOLCHAIN_ARCHIVE")
    set(${root_variable} "" CACHE PATH "Installed ${architecture} KOReader toolchain")
    set(${archive_variable} "" CACHE FILEPATH "Local KOReader ${version} ${architecture} archive")
    set(triple "arm-${architecture}-linux-gnueabi")
    if(architecture STREQUAL "kindlehf")
        string(APPEND triple "hf")
    endif()
    set(target "epub_toolchain_${architecture}")

    if(NOT "${${root_variable}}" STREQUAL "")
        get_filename_component(root "${${root_variable}}" ABSOLUTE)
        foreach(required "bin/${triple}-gcc" "bin/${triple}-g++" "${triple}/sysroot")
            if(NOT EXISTS "${root}/${required}")
                message(FATAL_ERROR "${root_variable} is missing ${required}: ${root}")
            endif()
        endforeach()
        add_custom_target(${target})
    else()
        set(archive "https://github.com/koreader/koxtoolchain/releases/download/${version}/${architecture}.tar.zst")
        if(NOT "${${archive_variable}}" STREQUAL "")
            get_filename_component(archive "${${archive_variable}}" ABSOLUTE)
            if(NOT EXISTS "${archive}")
                message(FATAL_ERROR "${archive_variable} does not exist: ${archive}")
            endif()
        endif()
        set(source "${CMAKE_BINARY_DIR}/toolchains/${version}/${architecture}")
        set(root "${source}/x-tools/${triple}")
        ExternalProject_Add(${target}
            URL "${archive}"
            TLS_VERIFY TRUE
            DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads/${version}"
            DOWNLOAD_NAME "${architecture}.tar.zst"
            DOWNLOAD_NO_EXTRACT TRUE
            SOURCE_DIR "${source}"
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND "")
        ExternalProject_Add_Step(${target} extract
            COMMAND "${CMAKE_COMMAND}"
                "-DARCHIVE=<DOWNLOADED_FILE>"
                "-DDESTINATION=<SOURCE_DIR>"
                "-DTRIPLE=${triple}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ExtractToolchain.cmake"
            DEPENDS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ExtractToolchain.cmake"
            DEPENDEES download
            DEPENDERS configure
            BYPRODUCTS "${root}/bin/${triple}-gcc" "${root}/bin/${triple}-g++")
    endif()

    set(${root_output} "${root}" PARENT_SCOPE)
    set(${target_output} "${target}" PARENT_SCOPE)
endfunction()
