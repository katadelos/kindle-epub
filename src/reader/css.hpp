#ifndef KINDLE_EPUB_CSS_HPP
#define KINDLE_EPUB_CSS_HPP
#include "reader/links.hpp"
namespace kindle_epub {
namespace reader {
std::string rewriteCssUrls(const std::string& css,
                           const std::string& documentPath,
                           const ResourceLinkMapper& mapper);
std::string rewritePaginationCss(const std::string& css);
} // namespace reader
} // namespace kindle_epub
#endif
