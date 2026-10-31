#include "kpp/gate.hpp"

namespace kindle_epub {
namespace kpp {

// Exact instruction bytes and little-endian byte shifts.
// NOLINTBEGIN(readability-magic-numbers)
std::array<unsigned char, kThumbAbsoluteVeneerSize>
makeThumbAbsoluteVeneer(std::uint32_t functionPointer) {
    const std::uint32_t thumbPointer = functionPointer | 1U;
    return {{0xdfU,
             0xf8U,
             0x00U,
             0xf0U,
             static_cast<unsigned char>(thumbPointer),
             static_cast<unsigned char>(thumbPointer >> 8U),
             static_cast<unsigned char>(thumbPointer >> 16U),
             static_cast<unsigned char>(thumbPointer >> 24U)}};
}
// NOLINTEND(readability-magic-numbers)

} // namespace kpp
} // namespace kindle_epub
