#ifndef KINDLE_EPUB_LINKS_HPP
#define KINDLE_EPUB_LINKS_HPP
#include <functional>
#include <string>
namespace kindle_epub {
namespace reader {
using InternalLinkMapper = std::function<std::string(const std::string&, const std::string&)>;
using ResourceLinkMapper = std::function<std::string(const std::string&)>;

} // namespace reader
} // namespace kindle_epub
#endif
