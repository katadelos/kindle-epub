include_guard(GLOBAL)

function(epub_add_checks)
    find_program(EPUB_CLANG_FORMAT_EXECUTABLE NAMES clang-format-18
        DOC "LLVM 18 clang-format executable" NO_CMAKE_FIND_ROOT_PATH)
    find_program(EPUB_CLANG_TIDY_EXECUTABLE NAMES clang-tidy-18
        DOC "LLVM 18 clang-tidy executable" NO_CMAKE_FIND_ROOT_PATH)
    find_program(EPUB_RUN_CLANG_TIDY_EXECUTABLE NAMES run-clang-tidy-18 run-clang-tidy18
        DOC "LLVM 18 run-clang-tidy executable" NO_CMAKE_FIND_ROOT_PATH)
    set(EPUB_LINT_JOBS "4" CACHE STRING "Maximum concurrent clang-tidy processes")
    set(EPUB_CLANG_TIDY_EXTRA_ARGS "" CACHE STRING
        "Extra compiler arguments for clang-tidy, such as --gcc-toolchain=/path")

    file(GLOB_RECURSE format_sources CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp" "${PROJECT_SOURCE_DIR}/tests/*.hpp")
    if(EPUB_CLANG_FORMAT_EXECUTABLE)
        add_custom_target(format
            COMMAND "${EPUB_CLANG_FORMAT_EXECUTABLE}" --style=file -i ${format_sources}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Formatting project C++ sources"
            VERBATIM USES_TERMINAL)
        add_custom_target(format-check
            COMMAND "${EPUB_CLANG_FORMAT_EXECUTABLE}" --style=file --dry-run --Werror
                ${format_sources}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Checking project C++ formatting"
            VERBATIM USES_TERMINAL)
    else()
        foreach(target format format-check)
            add_custom_target(${target}
                COMMAND "${CMAKE_COMMAND}" -E echo
                    "LLVM 18 clang-format is required. Install it and reconfigure, or set EPUB_CLANG_FORMAT_EXECUTABLE."
                COMMAND "${CMAKE_COMMAND}" -E false
                VERBATIM USES_TERMINAL)
        endforeach()
    endif()

    if(EPUB_CLANG_TIDY_EXECUTABLE AND EPUB_RUN_CLANG_TIDY_EXECUTABLE)
        set(extra_args ${EPUB_CLANG_TIDY_EXTRA_ARGS})
        list(TRANSFORM extra_args PREPEND "-extra-arg=")
        add_custom_target(lint
            COMMAND "${EPUB_RUN_CLANG_TIDY_EXECUTABLE}"
                -p "${PROJECT_BINARY_DIR}"
                -clang-tidy-binary "${EPUB_CLANG_TIDY_EXECUTABLE}"
                -j "${EPUB_LINT_JOBS}" ${extra_args}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "Linting the configured C++ translation units"
            VERBATIM USES_TERMINAL)
    else()
        add_custom_target(lint
            COMMAND "${CMAKE_COMMAND}" -E echo
                "LLVM 18 clang-tidy and run-clang-tidy are required. Install them and reconfigure, or set EPUB_CLANG_TIDY_EXECUTABLE and EPUB_RUN_CLANG_TIDY_EXECUTABLE."
            COMMAND "${CMAKE_COMMAND}" -E false
            VERBATIM USES_TERMINAL)
    endif()
endfunction()
