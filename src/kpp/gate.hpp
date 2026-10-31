#ifndef KINDLE_EPUB_KPP_GATE_HPP
#define KINDLE_EPUB_KPP_GATE_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace kindle_epub {
namespace kpp {

constexpr std::size_t kThumbAbsoluteVeneerSize = 8;

// Encodes `ldr.w pc, [pc, #0]` followed by a little-endian Thumb function pointer.
std::array<unsigned char, kThumbAbsoluteVeneerSize>
makeThumbAbsoluteVeneer(std::uint32_t functionPointer);

} // namespace kpp
} // namespace kindle_epub
#endif
