#ifndef KINDLE_EPUB_MOBI8_SUPPORT_HPP
#define KINDLE_EPUB_MOBI8_SUPPORT_HPP
#include <climits>
#include <cstdint>
#include <string>
#ifdef KINDLE_EPUB_TRACE
#include <syslog.h>
#endif
namespace kindle_epub {
#ifdef KINDLE_EPUB_TRACE
#ifdef KINDLE_EPUB_KPP_ABI
const char* const kTracePrefix = "kindle-epub-model[kpp]";
#else
const char* const kTracePrefix = "kindle-epub-model[webreader]";
#endif
#define EPUB_TRACE(priority, format, ...) syslog(priority, "%s: " format, kTracePrefix, __VA_ARGS__)
#else
#define EPUB_TRACE(priority, format, ...) ((void)0)
#endif

inline const unsigned char* bytes(const std::string& value) {
    return reinterpret_cast<const unsigned char*>(value.c_str());
}

inline unsigned long nativePosition(std::uint64_t value) {
    return value > static_cast<std::uint64_t>(ULONG_MAX) ? ULONG_MAX
                                                         : static_cast<unsigned long>(value);
}

inline std::string valueOf(const unsigned char* value) {
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
}

} // namespace kindle_epub
#endif
