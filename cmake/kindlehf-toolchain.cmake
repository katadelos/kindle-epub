set(EPUB_TOOLCHAIN_ROOT_VARIABLE KINDLEHF_TOOLCHAIN_ROOT)
set(EPUB_TOOLCHAIN_TRIPLE arm-kindlehf-linux-gnueabihf)
set(EPUB_TOOLCHAIN_FLAGS
    "-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb")
include("${CMAKE_CURRENT_LIST_DIR}/Toolchain.cmake")
