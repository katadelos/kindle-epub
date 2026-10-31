#ifndef KINDLE_EPUB_SCANNER_ABI_HPP
#define KINDLE_EPUB_SCANNER_ABI_HPP

#include <cstddef>
#include <cstdint>

// Clean-room declarations reconstructed from libscanner.so.1 and the stock
// native extractors in Kindle firmware 5.18.1.1.1/5.19.2.  On ARM32 these
// fields occupy the observed offsets 0x00 through 0x14.
struct scanner_event {
    std::uint32_t type;
    const char* path;
    const void* source;
    const char* filename;
    const char* uuid;
    const char* glob;
};

using scanner_handler = int (*)(const scanner_event* event, void* context);

#endif
