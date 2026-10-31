set(EPUB_TOOLCHAIN_ROOT_VARIABLE KINDLE5_TOOLCHAIN_ROOT)
set(EPUB_TOOLCHAIN_TRIPLE arm-kindle5-linux-gnueabi)
set(EPUB_TOOLCHAIN_FLAGS
    "-march=armv7-a -mtune=cortex-a8 -mfpu=vfpv3 -mfloat-abi=softfp -marm")
include("${CMAKE_CURRENT_LIST_DIR}/Toolchain.cmake")
