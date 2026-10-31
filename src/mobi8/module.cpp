#include "epub/filename.hpp"
#include "mobi8/model.hpp"

#include <dlfcn.h>
#include <syslog.h>

#include <cstring>

namespace {

#ifndef KINDLE_EPUB_MOBI8_TARGET
#define KINDLE_EPUB_MOBI8_TARGET "legacy"
#endif

const char kFactorySymbol[] =
    "_ZN8Mobi8SDK18BookManagerFactory14getBookManagerERNS_10ManagedPtrINS_12IBookManagerEEEPKh";

using StockFactory = Mobi8SDK::Result (*)(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>&,
                                          const unsigned char*);
using StockHasSuffix = int (*)(const char*, const char*);

StockFactory findStockFactory() {
    dlerror();
    void* symbol = dlsym(RTLD_NEXT, kFactorySymbol);
    if (symbol != nullptr) {
        return reinterpret_cast<StockFactory>(symbol);
    }

    // The framework loads libMobi with a local dlopen scope, so it is not
    // necessarily visible to RTLD_NEXT from an LD_PRELOAD module. Keep an
    // explicit handle alive and resolve the implementation from that object.
    static void* stockLibrary =
#ifdef KINDLE_EPUB_K4_ABI
        dlopen("/usr/java/lib/libmobi8sdkjni.so", RTLD_LAZY | RTLD_LOCAL);
#elif defined(KINDLE_EPUB_KT1_ABI) || defined(KINDLE_EPUB_PW1_ABI)
        dlopen("libMobi.so.0", RTLD_LAZY | RTLD_LOCAL);
#else
        dlopen("libMobi.so.1", RTLD_LAZY | RTLD_LOCAL);
#endif
    if (stockLibrary == nullptr) {
        return nullptr;
    }
    dlerror();
    symbol = dlsym(stockLibrary, kFactorySymbol);
    return reinterpret_cast<StockFactory>(symbol);
}

} // namespace

// PW6 WebReader rejects unknown filename extensions before asking libMobi for
// a book manager.  The module is already preloaded into that process, so make
// EPUB pass the same reader-mode gate as MOBI while preserving every other
// GLib suffix check verbatim.  This keeps the firmware executable untouched.
#ifndef KINDLE_EPUB_K4_ABI
extern "C" __attribute__((visibility("default"))) int g_str_has_suffix(const char* value,
                                                                       const char* suffix) {
    if (epub::isEpubPath(reinterpret_cast<const unsigned char*>(value)) && suffix != nullptr &&
        std::strcmp(suffix, ".mobi") == 0) {
        return 1;
    }

    static StockHasSuffix stockHasSuffix =
        reinterpret_cast<StockHasSuffix>(dlsym(RTLD_NEXT, "g_str_has_suffix"));
    return stockHasSuffix == nullptr ? 0 : stockHasSuffix(value, suffix);
}
#endif

extern "C" __attribute__((visibility("default"))) Mobi8SDK::Result kindleEpubGetBookManager(
    Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
    const unsigned char* path) __asm__("_ZN8Mobi8SDK18BookManagerFactory14getBookManagerERNS_"
                                       "10ManagedPtrINS_12IBookManagerEEEPKh");

extern "C" Mobi8SDK::Result
kindleEpubGetBookManager(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
                         const unsigned char* path) {
    if (epub::isEpubPath(path)) {
        const Mobi8SDK::Result status =
            kindle_epub::createBookManager(output, reinterpret_cast<const char*>(path));
        if (status != Mobi8SDK::SUCCESS) {
            syslog(LOG_ERR,
                   "kindle-epub: failed to open EPUB '%s' (status %u)",
                   path == nullptr ? "<null>" : reinterpret_cast<const char*>(path),
                   static_cast<unsigned int>(status));
        } else {
            syslog(LOG_INFO,
                   "kindle-epub: native EPUB manager active for '%s'",
                   reinterpret_cast<const char*>(path));
        }
        return status;
    }

    static StockFactory stockFactory = findStockFactory();
    if (stockFactory == nullptr) {
        output.reset();
        syslog(LOG_ERR, "kindle-epub: RTLD_NEXT could not resolve stock BookManagerFactory");
        return Mobi8SDK::INSTANTIATION_ERROR;
    }
    return stockFactory(output, path);
}

extern "C" __attribute__((visibility("default"))) const char* kindle_epub_module_version() {
    return "kindle-epub-native/0.1.0 mobi8-" KINDLE_EPUB_MOBI8_TARGET;
}
