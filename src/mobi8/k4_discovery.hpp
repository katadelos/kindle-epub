#ifndef KINDLE_EPUB_K4_DISCOVERY_HPP
#define KINDLE_EPUB_K4_DISCOVERY_HPP
#include <cstdint>
namespace kindle_epub {
class ElfImage;
std::uint32_t discoverK4Factory(const ElfImage& image);
} // namespace kindle_epub
#endif
