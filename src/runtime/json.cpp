#include "runtime/json.hpp"

#ifdef KINDLE_EPUB_DEVICE_JSON
#include <dlfcn.h>
#include <link.h>

#include <memory>
#include <stdexcept>
#include <string>
#endif

namespace kindle_epub {
#ifdef KINDLE_EPUB_DEVICE_JSON
namespace {

void* openJsonLibrary() {
    // Keep both SONAME lookup and symbol resolution separate from stock cJSON.
    void* handle =
        dlmopen(LM_ID_NEWLM, "/var/run/eink-sdk/usr/lib/libcjson.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        throw std::runtime_error(std::string("Cannot load opkg cJSON: ") +
                                 (error != nullptr ? error : "unknown loader error"));
    }
    return handle;
}

template <typename Function> Function resolveJson(void* handle, const char* name) {
    dlerror();
    void* address = dlsym(handle, name);
    const char* error = dlerror();
    if (address == nullptr || error != nullptr) {
        throw std::runtime_error(std::string("Missing opkg cJSON API: ") + name);
    }
    return reinterpret_cast<Function>(address);
}

} // namespace
#endif

const JsonApi& jsonApi() {
#ifdef KINDLE_EPUB_DEVICE_JSON
    static const std::unique_ptr<void, int (*)(void*)> library(openJsonLibrary(), &dlclose);
#define JSON_FUNCTION(name) resolveJson<decltype(&name)>(library.get(), #name)
#else
#define JSON_FUNCTION(name) &name
#endif
    static const JsonApi api = {JSON_FUNCTION(cJSON_Delete),
                                JSON_FUNCTION(cJSON_free),
                                JSON_FUNCTION(cJSON_PrintUnformatted),
                                JSON_FUNCTION(cJSON_CreateString),
                                JSON_FUNCTION(cJSON_CreateRaw),
                                JSON_FUNCTION(cJSON_AddItemToObject),
                                JSON_FUNCTION(cJSON_CreateObject),
                                JSON_FUNCTION(cJSON_AddStringToObject),
                                JSON_FUNCTION(cJSON_AddArrayToObject),
                                JSON_FUNCTION(cJSON_AddItemToArray),
                                JSON_FUNCTION(cJSON_AddObjectToObject),
                                JSON_FUNCTION(cJSON_AddRawToObject),
                                JSON_FUNCTION(cJSON_AddTrueToObject),
                                JSON_FUNCTION(cJSON_AddFalseToObject),
                                JSON_FUNCTION(cJSON_Parse),
                                JSON_FUNCTION(cJSON_GetObjectItemCaseSensitive),
                                JSON_FUNCTION(cJSON_GetArrayItem),
                                JSON_FUNCTION(cJSON_SetValuestring)};
#undef JSON_FUNCTION
    return api;
}

} // namespace kindle_epub
