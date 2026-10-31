#include "epub/error.hpp"
#include "providers.hpp"
#include "support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kindle_epub {
namespace {
using Mobi8SDK::FRAGMENT_NOT_AVAILABLE;
using Mobi8SDK::IContentProvider;
using Mobi8SDK::IFragment;
using Mobi8SDK::IParsedContent;
using Mobi8SDK::IResource;
using Mobi8SDK::ISecondaryTOCProvider;
using Mobi8SDK::ISkeleton;
using Mobi8SDK::ITableOfContents;
using Mobi8SDK::IWordIterator;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::NCX_SECONDARY_TOC_NOT_AVAILABLE;
using Mobi8SDK::NCX_TOC_NOT_AVAILABLE;
using Mobi8SDK::OPERATION_NOT_SUPPORTED;
using Mobi8SDK::POSITION_OUT_OF_BOUND;
using Mobi8SDK::RESOURCE_ID_NOT_FOUND;
using Mobi8SDK::RESOURCE_URL_NOT_VALID;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;

const unsigned long kInternalLinkType = 1;
const unsigned long kUnknownLinkType = 2;

class EpubParsedContent final : public IParsedContent {
public:
    EpubParsedContent(const std::string& content, std::uint64_t start, std::uint64_t end)
        : content_(content), start_(start), end_(end) {}

    const unsigned char* getContent() const override {
        return bytes(content_);
    }
    unsigned long getStartPosition() const override {
        return nativePosition(start_);
    }
    unsigned long getEndPosition() const override {
        return nativePosition(end_);
    }
    unsigned long getContentLength() const override {
        return nativePosition(content_.size());
    }

private:
    std::string content_;
    std::uint64_t start_;
    std::uint64_t end_;
};

std::string extractMarkup(const reader::Book& book, std::uint64_t start, std::uint64_t end) {
    std::string result;
    for (const auto& document : book.documents()) {
        const std::uint64_t overlapStart = std::max(start, document.startPosition);
        const std::uint64_t overlapEnd = std::min(end, document.endPosition);
        if (overlapStart >= overlapEnd) {
            continue;
        }
        const std::size_t localStart =
            static_cast<std::size_t>(overlapStart - document.startPosition);
        const std::size_t length = static_cast<std::size_t>(overlapEnd - overlapStart);
        result.append(document.fragment, localStart, length);
    }
    return result;
}

std::string extractWordText(const reader::Book& book, std::uint64_t start, std::uint64_t end) {
    std::string result;
    for (const auto& document : book.documents()) {
        const std::vector<reader::Word>& words = document.words;
        for (const auto& word : words) {
            if (word.endPosition <= start || word.position >= end) {
                continue;
            }
            if (!result.empty() && word.leadingSpace) {
                result.push_back(' ');
            }
            result += word.text;
        }
    }
    return result;
}

class EpubContentProvider final : public IContentProvider {
public:
    explicit EpubContentProvider(const std::shared_ptr<const reader::Book>& book)
        : book_(book), tableOfContents_(makeTableOfContents(book)) {}

    Result getFragment(ManagedPtr<IFragment>& output, unsigned long position) override {
        if (position >= book_->size()) {
            EPUB_TRACE(LOG_ERR,
                       "getFragment position=%lu out-of-bounds book-size=%lu status=%u",
                       position,
                       nativePosition(book_->size()),
                       static_cast<unsigned int>(POSITION_OUT_OF_BOUND));
            output.reset();
            return POSITION_OUT_OF_BOUND;
        }
        try {
            const reader::ContentDocument& document = book_->documentForPosition(position);
            const std::size_t index = document.index;
            EPUB_TRACE(LOG_INFO,
                       "getFragment position=%lu -> index=%lu id=%lu range=%lu-%lu",
                       position,
                       static_cast<unsigned long>(index),
                       static_cast<unsigned long>(document.index),
                       nativePosition(document.startPosition),
                       nativePosition(document.endPosition));
            output = makeFragment(book_, index, position);
            return SUCCESS;
        } catch (const epub::Error&) {
            EPUB_TRACE(LOG_ERR,
                       "getFragment position=%lu unavailable book-size=%lu status=%u",
                       position,
                       nativePosition(book_->size()),
                       static_cast<unsigned int>(FRAGMENT_NOT_AVAILABLE));
            output.reset();
            return FRAGMENT_NOT_AVAILABLE;
        }
    }
    Result getSkeleton(ManagedPtr<ISkeleton>& output, unsigned long skeletonId) override {
        if (skeletonId >= book_->documents().size()) {
            EPUB_TRACE(LOG_ERR,
                       "getSkeleton id=%lu unavailable count=%lu status=%u",
                       skeletonId,
                       static_cast<unsigned long>(book_->documents().size()),
                       static_cast<unsigned int>(Mobi8SDK::LOOKUP_STRING_NOT_FOUND));
            output.reset();
            return Mobi8SDK::LOOKUP_STRING_NOT_FOUND;
        }
        EPUB_TRACE(LOG_INFO,
                   "getSkeleton id=%lu -> %lu bytes",
                   skeletonId,
                   nativePosition(book_->documents()[skeletonId].skeleton.size()));
        output = makeSkeleton(book_, skeletonId);
        return SUCCESS;
    }
    Result getResource(ManagedPtr<IResource>& output, const unsigned char* resourceId) override {
        if (resourceId == nullptr) {
            EPUB_TRACE(LOG_ERR,
                       "getResource id=(null) status=%u",
                       static_cast<unsigned int>(RESOURCE_URL_NOT_VALID));
            output.reset();
            return RESOURCE_URL_NOT_VALID;
        }
        const std::string requestedId = valueOf(resourceId);
        EPUB_TRACE(LOG_INFO, "getResource id='%s'", requestedId.c_str());
        try {
            const reader::ResourceData resource = book_->readResource(requestedId);
            EPUB_TRACE(LOG_INFO,
                       "getResource id='%s' -> stored-id='%s' mime='%s' content=%lu status=%u",
                       requestedId.c_str(),
                       resource.id.c_str(),
                       resource.mediaType.c_str(),
                       nativePosition(resource.bytes.size()),
                       static_cast<unsigned int>(SUCCESS));
            output = makeResource(resource, requestedId);
            return SUCCESS;
        } catch (const epub::Error&) {
            EPUB_TRACE(LOG_ERR,
                       "getResource id='%s' status=%u",
                       requestedId.c_str(),
                       static_cast<unsigned int>(RESOURCE_ID_NOT_FOUND));
            output.reset();
            return RESOURCE_ID_NOT_FOUND;
        }
    }
    unsigned long getLinkType(const unsigned char* link) override {
        unsigned long position = 0;
        return getPositionForLink(position, link) == SUCCESS ? kInternalLinkType : kUnknownLinkType;
    }
    Result getPositionForLink(unsigned long& output, const unsigned char* link) override {
        try {
            output = nativePosition(book_->positionForLink(valueOf(link)));
            return SUCCESS;
        } catch (const epub::Error&) {
            output = 0;
            return Mobi8SDK::LOOKUP_STRING_NOT_FOUND;
        }
    }
    Result getFragmentForLink(ManagedPtr<IFragment>& output, const unsigned char* link) override {
        unsigned long position = 0;
        const Result status = getPositionForLink(position, link);
        if (status != SUCCESS) {
            output.reset();
            return status;
        }
        return getFragment(output, position);
    }
    Result getWordIterator(ManagedPtr<IWordIterator>& output,
                           unsigned long position,
                           bool forward) override {
        if (position > book_->size()) {
            output.reset();
            return POSITION_OUT_OF_BOUND;
        }
        output = makeWordIterator(book_, position, forward);
        return SUCCESS;
    }
    bool isTableOfContentsAvailable() override {
        return static_cast<bool>(tableOfContents_);
    }
    Result getTableOfContents(ManagedPtr<ITableOfContents>& output) override {
        if (!isTableOfContentsAvailable()) {
            output.reset();
            return NCX_TOC_NOT_AVAILABLE;
        }
        output = tableOfContents_;
        return SUCCESS;
    }
    bool isSecondaryTableOfContentsAvailable() override {
        return false;
    }
    Result getSecondaryTOCProvider(ManagedPtr<ISecondaryTOCProvider>& output) override {
        output.reset();
        return NCX_SECONDARY_TOC_NOT_AVAILABLE;
    }
#ifndef KINDLE_EPUB_KPP_ABI
    Result extractContent(ManagedPtr<IParsedContent>& output,
                          unsigned long start,
                          unsigned long end) override {
        return extract(output, start, end, false);
    }
    Result extractWords(ManagedPtr<IParsedContent>& output,
                        unsigned long start,
                        unsigned long end) override {
        return extract(output, start, end, true);
    }
#endif
    Result extractExactContent(ManagedPtr<IParsedContent>& output,
                               unsigned long start,
                               unsigned long end) override {
        return extract(output, start, end, false);
    }
#ifndef KINDLE_EPUB_EARLY_MOBI8_ABI
    Result getContainerResourceList(unsigned long /*containerId*/,
                                    std::vector<std::string>& output) override {
        output.clear();
        return OPERATION_NOT_SUPPORTED;
    }
    Result getResourceFromContainer(ManagedPtr<IResource>& output,
                                    const unsigned char* /*path*/,
                                    unsigned long /*containerId*/) override {
        output.reset();
        return OPERATION_NOT_SUPPORTED;
    }

#endif

private:
    Result extract(ManagedPtr<IParsedContent>& output,
                   std::uint64_t start,
                   std::uint64_t end,
                   bool wordsOnly) {
        if (start > end || start > book_->size()) {
            output.reset();
            return POSITION_OUT_OF_BOUND;
        }
        end = std::min(end, book_->size());
        const std::string content =
            wordsOnly ? extractWordText(*book_, start, end) : extractMarkup(*book_, start, end);
        output.reset(new EpubParsedContent(content, start, end));
        return SUCCESS;
    }

    std::shared_ptr<const reader::Book> book_;
    ManagedPtr<ITableOfContents> tableOfContents_;
};

} // namespace
Mobi8SDK::ManagedPtr<Mobi8SDK::IContentProvider>
makeContentProvider(const std::shared_ptr<const reader::Book>& book) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::IContentProvider>(new EpubContentProvider(book));
}
} // namespace kindle_epub
