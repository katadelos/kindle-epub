#include "discovery.hpp"

#include <cstring>

namespace kindle_epub {
namespace kpp {
namespace {

const unsigned char*
data(const std::vector<ImageRange>& image, std::uintptr_t address, std::size_t size) {
    for (const ImageRange& range : image) {
        if (address >= range.address && size <= range.size &&
            address - range.address <= range.size - size) {
            if (range.bytes != nullptr) {
                return range.bytes + (address - range.address);
            }
            // Live images use their process address rather than a separate file mapping.
            // NOLINTNEXTLINE(performance-no-int-to-ptr)
            return reinterpret_cast<const unsigned char*>(address);
        }
    }
    return nullptr;
}

std::uint16_t half(const std::vector<ImageRange>& image, std::uintptr_t address) {
    std::uint16_t value;
    const unsigned char* bytes = data(image, address, 2);
    if (bytes == nullptr) {
        return 0;
    }
    std::memcpy(&value, bytes, 2);
    return value;
}

// ARM/Thumb instruction encodings and byte offsets are kept together for review.
// NOLINTBEGIN(readability-magic-numbers)
bool literal(const std::vector<ImageRange>& image,
             std::uintptr_t instruction,
             std::uint32_t* value) {
    const std::uint16_t opcode = half(image, instruction);
    if ((opcode & 0xf800U) != 0x4800U) {
        return false;
    }
    const std::uintptr_t address =
        ((instruction + 4) & ~std::uintptr_t(3)) + std::uintptr_t(opcode & 0xffU) * 4;
    const unsigned char* bytes = data(image, address, sizeof(*value));
    if (bytes == nullptr) {
        return false;
    }
    std::memcpy(value, bytes, sizeof(*value));
    return true;
}

bool stringAt(const std::vector<ImageRange>& image, std::uintptr_t address, const char* text) {
    const std::size_t size = std::strlen(text) + 1;
    const unsigned char* bytes = data(image, address, size);
    return bytes != nullptr && std::memcmp(bytes, text, size) == 0;
}

// The logger streams a PC-relative string through r1. Literal displacements and
// the addresses of both strings change between builds; neither is a signature.
bool references(const std::vector<ImageRange>& image, std::uintptr_t entry, const char* text) {
    for (unsigned int offset = 12; offset < 224; offset += 2) {
        const std::uintptr_t instruction = entry + offset;
        if ((half(image, instruction) & 0xff00U) != 0x4900U) {
            continue;
        }
        std::uint32_t relative;
        if (!literal(image, instruction, &relative)) {
            continue;
        }
        // ldr r1, [pc, #imm]; [mov r0, rN]; add r1, pc
        unsigned int distance = 2;
        if (half(image, instruction + 2) != 0x4479U) {
            if ((half(image, instruction + 2) & 0xffc7U) != 0x4600U) {
                continue;
            }
            distance = 4;
        }
        if (half(image, instruction + distance) != 0x4479U) {
            continue;
        }
        const std::uintptr_t target =
            static_cast<std::uint32_t>(instruction + distance + 4 + relative);
        if (stringAt(image, target, text)) {
            return true;
        }
    }
    return false;
}

bool extensionEntry(const std::vector<ImageRange>& image, std::uintptr_t entry, unsigned int* abi) {
    unsigned int literalOffset;
    if (half(image, entry) == 0xb570U && half(image, entry + 2) == 0x460cU &&
        (half(image, entry + 4) & 0xff00U) == 0x4a00U && half(image, entry + 6) == 0xe9d1U &&
        half(image, entry + 8) == 0x0100U) {
        *abi = 1;
        literalOffset = 4;
    } else if (half(image, entry) == 0x6808U && half(image, entry + 2) == 0xb570U &&
               half(image, entry + 4) == 0x460cU && (half(image, entry + 6) & 0xff00U) == 0x4a00U &&
               half(image, entry + 8) == 0xf850U && half(image, entry + 10) == 0x1c0cU) {
        *abi = 0;
        literalOffset = 6;
    } else {
        return false;
    }
    std::uint32_t seed;
    if (!literal(image, entry + literalOffset, &seed) || seed != 0xc70f6907U) {
        return false;
    }
    // The format enum's unknown value is 255. A nearby, otherwise identical
    // string lookup returns -1: accepting just the prologue would patch it too.
    for (unsigned int offset = 32; offset < 60; offset += 2) {
        if (half(image, entry + offset) == 0xbd70U && half(image, entry + offset + 2) == 0x20ffU &&
            half(image, entry + offset - 2) == (*abi != 0 ? 0x69c0U : 0x6880U)) {
            return true;
        }
    }
    return false;
}

bool factoryEntry(const std::vector<ImageRange>& image, std::uintptr_t entry) {
    return (half(image, entry) & 0xff00U) == 0x4a00U &&
           (half(image, entry + 2) & 0xff00U) == 0x4b00U && half(image, entry + 4) == 0xe92dU &&
           half(image, entry + 6) == 0x43f0U && (half(image, entry + 8) & 0xff80U) == 0xb080U &&
           half(image, entry + 10) == 0x447aU;
}

// NOLINTEND(readability-magic-numbers)

void put16(unsigned char* destination, std::size_t offset, std::uint16_t value) {
    std::memcpy(destination + offset, &value, 2);
}
void put32(unsigned char* destination, std::size_t offset, std::uint32_t value) {
    std::memcpy(destination + offset, &value, 4);
}
} // namespace

HookSites discover(const std::vector<ImageRange>& image) {
    constexpr std::size_t searchWindow = 256;
    HookSites sites = {};
    for (const ImageRange& range : image) {
        if (!range.executable || range.size < searchWindow) {
            continue;
        }
        for (std::uintptr_t entry = (range.address + 3) & ~std::uintptr_t(3);
             entry - range.address <= range.size - searchWindow;
             entry += 4) {
            unsigned int abi;
            if (extensionEntry(image, entry, &abi)) {
                sites.extension = entry;
                sites.stringAbi = abi;
                ++sites.extensionCount;
            }
            if (factoryEntry(image, entry) &&
                references(image, entry, "Get Book Manager invoked with ebook ") &&
                references(image, entry, "getBookManager")) {
                sites.factory = entry;
                ++sites.factoryCount;
            }
        }
    }
    return sites;
}

// Relocation emits the exact ARM/Thumb instruction fields checked above.
// NOLINTBEGIN(readability-magic-numbers)
std::size_t makeTrampoline(const std::vector<ImageRange>& image,
                           std::uintptr_t entry,
                           bool extension,
                           unsigned int stringAbi,
                           unsigned char* destination) {
    if ((reinterpret_cast<std::uintptr_t>(destination) & 3U) != 0 ||
        data(image, entry, kTrampolineSize) == nullptr) {
        return 0;
    }
    unsigned int detectedAbi;
    if (extension ? (!extensionEntry(image, entry, &detectedAbi) || detectedAbi != stringAbi)
                  : !factoryEntry(image, entry)) {
        return 0;
    }
    const std::size_t displaced = extension && stringAbi != 0 ? 10 : 8;
    std::size_t emitted = 0;
    std::size_t pool = 32;
    for (std::size_t offset = 0; offset < displaced;) {
        const std::uint16_t instruction = half(image, entry + offset);
        if ((instruction & 0xf800U) == 0x4800U) {
            std::uint32_t value;
            if (!literal(image, entry + offset, &value)) {
                return 0;
            }
            // ldr.w rN, [pc, #imm12], directed at our own literal pool.
            put16(destination, emitted, 0xf8dfU);
            put16(destination,
                  emitted + 2,
                  ((instruction >> 8) & 7U) << 12 | (pool - ((emitted + 4) & ~std::size_t(3))));
            put32(destination, pool, value);
            pool += 4;
            emitted += 4;
            offset += 2;
        } else {
            // Every other accepted instruction is PC independent: push, mov,
            // ldr r0,[r1], or ldrd r0,r1,[r1]. Their exact encodings were checked.
            const std::size_t size = instruction == 0xe92dU || instruction == 0xe9d1U ? 4 : 2;
            std::memcpy(destination + emitted, data(image, entry + offset, size), size);
            emitted += size;
            offset += size;
        }
    }
    put16(destination, emitted, 0xf8dfU);
    put16(destination, emitted + 2, 0xf000U | (pool - ((emitted + 4) & ~std::size_t(3))));
    put32(destination, pool, static_cast<std::uint32_t>(entry + displaced) | 1U);
    return pool + 4;
}

// NOLINTEND(readability-magic-numbers)

} // namespace kpp
} // namespace kindle_epub
