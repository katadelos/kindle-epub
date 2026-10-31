#ifndef KINDLE_EPUB_SCANNER_SHARED_LIBRARY_HPP
#define KINDLE_EPUB_SCANNER_SHARED_LIBRARY_HPP

#include <dlfcn.h>
#include <stdexcept>
#include <string>

namespace kindle_epub {

// Own the library until every resolved function and library-owned object is done.
class SharedLibrary {
public:
    explicit SharedLibrary(const std::string& path)
        : handle_(dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        if (handle_ == nullptr) {
            const char* error = dlerror();
            throw std::runtime_error("Cannot load " + path + ": " +
                                     (error != nullptr ? error : "unknown loader error"));
        }
    }

    ~SharedLibrary() {
        dlclose(handle_);
    }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    template <typename Function> Function resolve(const char* name) const {
        dlerror();
        void* address = dlsym(handle_, name);
        const char* error = dlerror();
        if (address == nullptr || error != nullptr) {
            throw std::runtime_error(std::string("Missing native API: ") + name);
        }
        return reinterpret_cast<Function>(address);
    }

private:
    void* handle_;
};

} // namespace kindle_epub
#endif
