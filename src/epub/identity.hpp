#ifndef KINDLE_EPUB_IDENTITY_HPP
#define KINDLE_EPUB_IDENTITY_HPP

#include <string>

#include "epub/package.hpp"

namespace epub {

// A compact, deterministic identifier shared by the reader model and CCAT
// extractor.  Kindle treats this as a local PDOC key, not as an EPUB UUID.
std::string stableBookId(const Metadata& metadata, const std::string& fallbackPath);

} // namespace epub

#endif
