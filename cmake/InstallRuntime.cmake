# Native payload layout shared by standalone device builds and the superbuild.
# Host test binaries and the private static dependencies are not runtime assets.
if(NOT EPUB_DEVICE_BUILD)
    return()
endif()

# Ship regular unversioned files, retaining the adapters' existing ELF SONAMEs.
foreach(epub_target IN ITEMS kindle_epub kindle_epub_kpp kindle_epub_extractor)
    if(TARGET ${epub_target})
        set_property(TARGET ${epub_target} PROPERTY VERSION)
        set_property(TARGET ${epub_target} PROPERTY SOVERSION)
        set_property(TARGET ${epub_target} PROPERTY NO_SONAME TRUE)
        get_target_property(epub_output_name ${epub_target} OUTPUT_NAME)
        target_link_options(${epub_target} PRIVATE
            "-Wl,-soname,${CMAKE_SHARED_LIBRARY_PREFIX}${epub_output_name}${CMAKE_SHARED_LIBRARY_SUFFIX}.0")
    endif()
endforeach()

set(epub_native_destination "native/${EPUB_MOBI8_PROFILE}/abi${EPUB_CXX11_ABI}")
set(epub_library_permissions OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
set(epub_program_permissions
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

foreach(epub_target IN ITEMS kindle_epub kindle_epub_kpp)
    if(TARGET ${epub_target})
        install(TARGETS ${epub_target}
            LIBRARY DESTINATION "${epub_native_destination}"
            PERMISSIONS ${epub_library_permissions}
            COMPONENT Runtime)
    endif()
endforeach()

if(TARGET kindle_epub_extractor)
    install(TARGETS kindle_epub_extractor
        LIBRARY DESTINATION "native/${EPUB_MOBI8_PROFILE}"
        PERMISSIONS ${epub_library_permissions}
        COMPONENT Runtime)
endif()

# The superbuild enables these targets in the first variant of each architecture.
foreach(epub_target IN ITEMS kindle_epub_framework_exec kindle_epub_java_agent)
    if(TARGET ${epub_target})
        install(TARGETS ${epub_target}
            LIBRARY DESTINATION .
            PERMISSIONS ${epub_library_permissions}
            COMPONENT Runtime)
    endif()
endforeach()

foreach(epub_target IN ITEMS kindle-epub-probe kindle_epub_catalog_import kindle_epub_control)
    if(TARGET ${epub_target})
        install(TARGETS ${epub_target}
            RUNTIME DESTINATION .
            PERMISSIONS ${epub_program_permissions}
            COMPONENT Runtime)
    endif()
endforeach()
