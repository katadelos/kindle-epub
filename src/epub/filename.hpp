#ifndef KINDLE_EPUB_FILENAME_HPP
#define KINDLE_EPUB_FILENAME_HPP

#include <cstddef>
#include <cstring>

namespace epub {

inline bool isEpubExtension(const char* bytes, std::size_t length) {
    if (bytes == nullptr || length != 4) {
        return false;
    }
    const char extension[] = "epub";
    for (std::size_t index = 0; index < length; ++index) {
        char value = bytes[index];
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
        if (value != extension[index]) {
            return false;
        }
    }
    return true;
}

// Native reader entry points may receive a filename with a query or fragment.
inline bool isEpubPath(const unsigned char* rawPath) {
    if (rawPath == nullptr) {
        return false;
    }
    const char* path = reinterpret_cast<const char*>(rawPath);
    const std::size_t length = std::strcspn(path, "?#");
    const std::size_t suffixLength = sizeof(".epub") - 1;
    return length >= suffixLength && path[length - suffixLength] == '.' &&
           isEpubExtension(path + length - suffixLength + 1, suffixLength - 1);
}

} // namespace epub
#endif
