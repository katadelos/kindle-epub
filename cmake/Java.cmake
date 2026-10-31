include_guard(GLOBAL)

function(epub_add_java_runtime)
    # Java tools run on the build host, including in a cross-build.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    find_package(Java REQUIRED COMPONENTS Development)
    find_program(EPUB_ECJ_EXECUTABLE NAMES ecj
        DOC "ECJ compiler with Java 1.4 source and target support"
        NO_CMAKE_FIND_ROOT_PATH REQUIRED)

    set(source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/java")
    set(sources
        "${source_root}/org/katadelos/epub/EpubSupport.java"
        "${source_root}/org/katadelos/epub/runtime/ClassAdapter.java"
        "${source_root}/org/katadelos/epub/runtime/Discovery.java"
        "${source_root}/org/katadelos/epub/runtime/Main.java"
        "${source_root}/org/katadelos/epub/runtime/Probe.java")
    set(classes "${CMAKE_CURRENT_BINARY_DIR}/java-classes")
    set(jar "${CMAKE_CURRENT_BINARY_DIR}/kindle-epub-java.jar")

    add_custom_command(OUTPUT "${jar}"
        # Recreate the class tree so renamed or removed sources leave no stale classes.
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${classes}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${classes}"
        COMMAND "${EPUB_ECJ_EXECUTABLE}" -source 1.4 -target 1.4 -proc:none -nowarn
            -d "${classes}" ${sources}
        COMMAND "${Java_JAR_EXECUTABLE}" cf "${jar}.pending" -C "${classes}" .
        COMMAND "${CMAKE_COMMAND}" -E rename "${jar}.pending" "${jar}"
        DEPENDS ${sources} "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            "${EPUB_ECJ_EXECUTABLE}" "${Java_JAR_EXECUTABLE}"
        COMMENT "Building Java 1.4 runtime helper"
        VERBATIM)
    add_custom_target(kindle-epub-java DEPENDS "${jar}")
    set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES
        "${classes}" "${jar}.pending")
    set(EPUB_JAVA_JAR "${jar}" PARENT_SCOPE)
endfunction()
