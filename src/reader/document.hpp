#ifndef KINDLE_EPUB_DOCUMENT_HPP
#define KINDLE_EPUB_DOCUMENT_HPP

#include "reader/links.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kindle_epub {
namespace reader {

struct Word {
    std::string text;
    std::uint64_t position = 0;
    std::uint64_t endPosition = 0;
    bool leadingSpace = false;
};

struct ContentDocument {
    std::string path;
    // Spine ordinal shared by the native skeleton and fragment.
    std::uint32_t index = 0;
    std::string placeholderId;
    std::string skeleton;
    // Byte offset in the skeleton at which fragment bytes are inserted.
    std::size_t fragmentInsertionOffset = 0;
    std::string fragment;
    std::string plainText;
    std::vector<Word> words;
    std::map<std::string, std::uint64_t> anchorOffsets;
    std::uint64_t startPosition = 0;
    std::uint64_t endPosition = 0;
};

// nextAid is shared by every spine document so generated XPath targets stay unique.
ContentDocument parseContentDocument(const std::string& archivePath,
                                     const std::string& bytes,
                                     std::uint32_t index,
                                     std::uint64_t& nextAid,
                                     const InternalLinkMapper& internalLinkMapper,
                                     const ResourceLinkMapper& resourceLinkMapper);

} // namespace reader

} // namespace kindle_epub
#endif
