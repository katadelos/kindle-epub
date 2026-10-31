#ifndef KINDLE_EPUB_SCANNER_STOCK_API_HPP
#define KINDLE_EPUB_SCANNER_STOCK_API_HPP

#include <cstddef>

namespace kindle_epub {

// The stock scanner owns JSON parsing and CCAT transport. Passing text keeps
// its private cJSON layout and calling convention out of both integrations.
struct ScannerApi {
    void (*generateUuid)(char*, std::size_t);
    int (*postString)(const char*);
    int (*deleteEntry)(const char*);
};

struct ExtractorApi {
    // The source is the scanner's LIPC handle address. Returned thumbnail
    // strings are allocated by the stock library and released with free().
    char* (*constructThumbnail)(const void*, const unsigned char*, std::size_t, int);
    int (*shouldRemoveThumbnail)(const void*, const char*);
    char* (*thumbnailForUuid)(const char*);
};

// Consumes the input image path and returns a malloc-owned output path.
using ResizeThumbnail = char* (*)(const char*, int, int);

// Resolve complete interfaces before exposing them. Libraries remain loaded
// for the process lifetime, including while a scanner callback uses a table.
const ScannerApi& scannerApi();
const ExtractorApi& extractorApi();
ResizeThumbnail thumbnailResizer();

} // namespace kindle_epub
#endif
