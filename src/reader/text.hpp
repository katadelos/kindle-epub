#ifndef KINDLE_EPUB_TEXT_HPP
#define KINDLE_EPUB_TEXT_HPP
#include "reader/document.hpp"
namespace kindle_epub {
namespace reader {
// Positions are byte offsets in the serialized fragment consumed by the reader.
void indexDocumentText(ContentDocument& document);
} // namespace reader
} // namespace kindle_epub
#endif
