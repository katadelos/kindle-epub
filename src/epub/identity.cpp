#include "epub/identity.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace epub {

std::string stableBookId(const Metadata& metadata, const std::string& fallbackPath) {
    const std::string& source = metadata.identifier.empty() ? fallbackPath : metadata.identifier;
    // FNV-1a/64 keeps the local PDOC key compact while avoiding the practical
    // collision rate of a 32-bit catalog identity. This value is deliberately
    // computed by the shared core so scanner and reader always agree.
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (char character : source) {
        hash ^= static_cast<unsigned char>(character);
        hash *= UINT64_C(1099511628211);
    }
    std::ostringstream result;
    const int hexWidth = 16;
    result << "EPUB-" << std::uppercase << std::hex << std::setw(hexWidth) << std::setfill('0')
           << hash;
    return result.str();
}

} // namespace epub
