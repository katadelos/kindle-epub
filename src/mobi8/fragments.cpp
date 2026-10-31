#include "providers.hpp"
#include "support.hpp"

#include <algorithm>
#include <climits>

namespace kindle_epub {
namespace {

using Mobi8SDK::FRAGMENT_NOT_AVAILABLE;
using Mobi8SDK::IFragment;
using Mobi8SDK::ISkeleton;
using Mobi8SDK::ISkeletonLink;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;

class EpubSkeletonLink final : public ISkeletonLink {
public:
    explicit EpubSkeletonLink(const std::string& placeholderId)
        : xpath_("//*[@aid='" + placeholderId + "']") {}

    const unsigned char* getXPath() const override {
        return bytes(xpath_);
    }
    unsigned long getLinkType() const override {
        return 0;
    } // HTMLFragment::PARENT

private:
    std::string xpath_;
};

class EpubSkeleton final : public ISkeleton {
public:
    EpubSkeleton(const std::shared_ptr<const reader::Book>& book, std::size_t index)
        : book_(book), document_(book->documents().at(index)) {}

    unsigned long id() const override {
        return document_.index;
    }
    bool hasParentSkeleton() const override {
        return false;
    }
    unsigned long getParentSkeletonId() const override {
        return ULONG_MAX;
    }
    ManagedPtr<ISkeletonLink> getParentSkeletonLink() const override {
        return {};
    }
    unsigned long getFragmentCount() const override {
        return 1;
    }
    unsigned long getSubSkeletonCount() const override {
        return 0;
    }
    const unsigned char* getContent() const override {
        return bytes(document_.skeleton);
    }
    unsigned long getContentLength() const override {
        return nativePosition(document_.skeleton.size());
    }
    bool isPropertySet(const unsigned char* /*property*/) override {
        return false;
    }
    Result getPropertyList(std::vector<std::string>& output) override {
        output.clear();
        return SUCCESS;
    }
    Result getMetadata(std::string& output, const unsigned char* /*key*/) override {
        output.clear();
        return Mobi8SDK::LOOKUP_STRING_NOT_FOUND;
    }

private:
    // Keep the immutable document alive for every pointer returned through the ABI.
    std::shared_ptr<const reader::Book> book_;
    const reader::ContentDocument& document_;
};

class EpubFragment final : public IFragment {
public:
    EpubFragment(const std::shared_ptr<const reader::Book>& book,
                 const reader::ContentDocument& document,
                 std::uint64_t requestedPosition)
        : book_(book), document_(document),
          skeletonLink_(new EpubSkeletonLink(document_.placeholderId)) {
        if (requestedPosition > document_.startPosition && !document_.fragment.empty()) {
            // Skeleton suffixes map to the last byte, never one byte past the fragment.
            const std::uint64_t lastByte = document_.fragment.size() - 1;
            offset_ = std::min(requestedPosition - document_.startPosition, lastByte);
        }
    }

    unsigned long id() const override {
        return document_.index;
    }
    unsigned long getPosition() const override {
        return nativePosition(document_.startPosition);
    }
    unsigned long getOffset() const override {
        return nativePosition(offset_);
    }
    unsigned long getSkeletonId() const override {
        return document_.index;
    }
    ManagedPtr<ISkeletonLink> getSkeletonLink() override {
        // WebReader retains the XPath pointer after releasing the returned temporary.
        // The fragment therefore owns its link for the entire fragment lifetime.
        return skeletonLink_;
    }
    const unsigned char* getContent() const override {
        return bytes(document_.fragment);
    }
    unsigned long getContentLength() const override {
        return nativePosition(document_.fragment.size());
    }
    bool hasPreviousFragment() override {
        return document_.index != 0;
    }
    bool hasNextFragment() override {
        return document_.index + 1 < book_->documents().size();
    }

    Result getPreviousFragment(ManagedPtr<IFragment>& output) override {
        if (!hasPreviousFragment()) {
            output.reset();
            return FRAGMENT_NOT_AVAILABLE;
        }
        if (!previous_) {
            const auto& previous = book_->documents()[document_.index - 1];
            previous_.reset(new EpubFragment(book_, previous, previous.startPosition));
        }
        output = previous_;
        return SUCCESS;
    }

    Result getNextFragment(ManagedPtr<IFragment>& output) override {
        if (!hasNextFragment()) {
            output.reset();
            return FRAGMENT_NOT_AVAILABLE;
        }
        if (!next_) {
            const auto& next = book_->documents()[document_.index + 1];
            next_.reset(new EpubFragment(book_, next, next.startPosition));
        }
        output = next_;
        return SUCCESS;
    }

private:
    std::shared_ptr<const reader::Book> book_;
    const reader::ContentDocument& document_;
    std::uint64_t offset_ = 0;
    ManagedPtr<ISkeletonLink> skeletonLink_;
    ManagedPtr<IFragment> previous_;
    ManagedPtr<IFragment> next_;
};

} // namespace

Mobi8SDK::ManagedPtr<Mobi8SDK::ISkeleton>
makeSkeleton(const std::shared_ptr<const reader::Book>& book, std::size_t index) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::ISkeleton>(new EpubSkeleton(book, index));
}

Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> makeFragment(
    const std::shared_ptr<const reader::Book>& book, std::size_t index, std::uint64_t position) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment>(
        new EpubFragment(book, book->documents().at(index), position));
}

} // namespace kindle_epub
