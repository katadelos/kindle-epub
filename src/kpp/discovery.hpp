#ifndef KINDLE_EPUB_KPP_DISCOVERY_HPP
#define KINDLE_EPUB_KPP_DISCOVERY_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace kindle_epub {
namespace kpp {

struct ImageRange {
    std::uintptr_t address;
    std::size_t size;
    bool executable;
    // Null for a live process; otherwise file bytes corresponding to address.
    const unsigned char* bytes;
};

struct HookSites {
    std::uintptr_t extension;
    std::uintptr_t factory;
    unsigned int stringAbi;
    unsigned int extensionCount;
    unsigned int factoryCount;
};

// Searches the loaded main executable, including its load bias. A match needs
// both a relocatable entry sequence and the function's semantic anchors.
HookSites discover(const std::vector<ImageRange>& image);

constexpr std::size_t kTrampolineSize = 64;

// Relocates only the entry instructions accepted by discover(). The destination
// must be word aligned and have 64 writable bytes. No unsupported instructions
// are copied speculatively. Returns the emitted size, or zero.
std::size_t makeTrampoline(const std::vector<ImageRange>& image,
                           std::uintptr_t entry,
                           bool extension,
                           unsigned int stringAbi,
                           unsigned char* destination);

} // namespace kpp
} // namespace kindle_epub
#endif
