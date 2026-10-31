// GCC 14's headers emit this libstdc++ convenience symbol for oversized
// vector allocations, but Kindle's libstdc++ 6.0.28 exposes only the
// underlying C++ ABI throw helper. Keep the compatibility definition local to
// our DSO and delegate to the firmware-provided, versioned ABI function.

#ifdef KINDLE_EPUB_OLD_STDCXX
#include <new>
#else
// The name is fixed by the firmware C++ runtime ABI.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
extern "C" __attribute__((noreturn)) void __cxa_throw_bad_array_new_length();
#endif

extern "C" __attribute__((noreturn, visibility("hidden"))) void
kindle_epub_throw_bad_array_new_length() __asm__("_ZSt28__throw_bad_array_new_lengthv");

extern "C" void kindle_epub_throw_bad_array_new_length() {
#ifdef KINDLE_EPUB_OLD_STDCXX
    throw std::bad_alloc();
#else
    __cxa_throw_bad_array_new_length();
#endif
    __builtin_unreachable();
}

#ifdef KINDLE_EPUB_OLD_STDCXX
#include <cstdarg>
#include <cstdio>
#include <stdexcept>
namespace std {
// The name is fixed by the firmware libstdc++ ABI.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
__attribute__((noreturn)) void __throw_out_of_range_fmt(const char* format, ...) {
    constexpr std::size_t messageCapacity = 256;
    char message[messageCapacity];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    throw out_of_range(std::string(message));
}
} // namespace std
#endif
