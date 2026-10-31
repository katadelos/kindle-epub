#include "stock_api.hpp"
#include "shared_library.hpp"

#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace kindle_epub {
namespace {

std::string libraryPath(const char* versioned, const char* unversioned) {
    const std::string path = std::string("/usr/lib/") + versioned;
    // Older stock libraries have an unversioned SONAME. Choose the installed
    // file before loading, so a present but broken library cannot be skipped.
    return access(path.c_str(), F_OK) == 0 ? path : std::string("/usr/lib/") + unversioned;
}

const SharedLibrary& extractorLibrary() {
    static const SharedLibrary library(
        libraryPath("libextractor_util.so.1", "libextractor_util.so"));
    return library;
}

} // namespace

const ScannerApi& scannerApi() {
    static const SharedLibrary library(libraryPath("libscanner.so.1", "libscanner.so"));
    static const ScannerApi api = {
        library.resolve<void (*)(char*, std::size_t)>("scanner_gen_uuid"),
        library.resolve<int (*)(const char*)>("scanner_post_string"),
        library.resolve<int (*)(const char*)>("scanner_delete_ccat_entry")};
    return api;
}

const ExtractorApi& extractorApi() {
    const SharedLibrary& library = extractorLibrary();
    static const ExtractorApi api = {
        library.resolve<char* (*)(const void*, const unsigned char*, std::size_t, int)>(
            "construct_thumbnail_uri"),
        library.resolve<int (*)(const void*, const char*)>(
            "should_remove_thumbnail_on_delete_entry"),
        library.resolve<char* (*)(const char*)>("extractor_get_thumbnail_for_uuid")};
    return api;
}

ResizeThumbnail thumbnailResizer() {
    static const ResizeThumbnail resize =
        extractorLibrary().resolve<ResizeThumbnail>("extractor_resize_thumbnail");
    return resize;
}

} // namespace kindle_epub
