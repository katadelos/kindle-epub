set(EPUB_TOOLCHAIN_ROOT_VARIABLE KINDLEPW2_TOOLCHAIN_ROOT)
set(EPUB_TOOLCHAIN_TRIPLE arm-kindlepw2-linux-gnueabi)
set(EPUB_TOOLCHAIN_FLAGS
    "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb")
include("${CMAKE_CURRENT_LIST_DIR}/Toolchain.cmake")
