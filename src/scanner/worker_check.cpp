#include "shared_library.hpp"
#include "stock_api.hpp"
#include "worker_runtime.hpp"
namespace epub_catalog {
namespace {
template <std::size_t Count>
void checkLibrary(const char* name, const char* const (&symbols)[Count]) {
    const kindle_epub::SharedLibrary library(name);
    for (const char* symbol : symbols) {
        library.resolve<void (*)()>(symbol);
    }
}

} // namespace
void checkRuntime() {
    const char* const sqlite[] = {"sqlite3_open_v2",
                                  "sqlite3_close",
                                  "sqlite3_finalize",
                                  "sqlite3_prepare_v2",
                                  "sqlite3_bind_text",
                                  "sqlite3_step",
                                  "sqlite3_column_text",
                                  "sqlite3_busy_timeout"};
    const char* const lipc[] = {"LipcOpenEx",
                                "LipcOpenNoName",
                                "LipcClose",
                                "LipcRegisterStringProperty",
                                "LipcRegisterIntProperty",
                                "LipcGetIntProperty"};
    kindle_epub::scannerApi();
    checkLibrary("libsqlite3.so.0", sqlite);
    checkLibrary("liblipc.so.1", lipc);
}

} // namespace epub_catalog
