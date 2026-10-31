#include "k4_discovery.hpp"
#include "runtime/elf_image.hpp"

namespace kindle_epub {
std::uint32_t discoverK4Factory(const ElfImage& image) {
    // These two instructions are copied verbatim to the trampoline. Neither
    // reads PC or branches; the rest of the function stays at its stock address.
    const std::uint32_t displaced[] = {0xe92d4810, 0xe28db008};
    Elf32_Sym factory;
    const char* name =
        "_ZN8Mobi8SDK18BookManagerFactory14getBookManagerERNS_10ManagedPtrINS_12IBookManagerEEEPKh";
    if (!image.symbol(name, &factory) || ELF32_ST_TYPE(factory.st_info) != STT_FUNC ||
        (factory.st_value & 3) != 0 || factory.st_size < sizeof(displaced) || image.hardFloat()) {
        throw std::runtime_error(std::string("No supported embedded ARM Mobi8 factory"));
    }
    if (std::memcmp(image.at(factory.st_value, sizeof(displaced), true),
                    displaced,
                    sizeof(displaced)) != 0) {
        throw std::runtime_error(
            std::string("Embedded Mobi8 factory has an unsupported ARM prologue"));
    }
    return factory.st_value;
}
} // namespace kindle_epub
