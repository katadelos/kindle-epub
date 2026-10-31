#include "epub/error.hpp"
#include "epub/path.hpp"
#include "providers.hpp"
#include "support.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace kindle_epub {
namespace {
using Mobi8SDK::IResource;
using Mobi8SDK::ITableOfContents;
using Mobi8SDK::ITableOfContentsEntry;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;
using Mobi8SDK::TOC_FIRST_CHILD_NOT_FOUND;
using Mobi8SDK::TOC_LAST_CHILD_NOT_FOUND;
using Mobi8SDK::TOC_NEXT_ENTRY_NOT_FOUND;
using Mobi8SDK::TOC_PARENT_NOT_FOUND;
using Mobi8SDK::TOC_PREV_ENTRY_NOT_FOUND;

struct TocNode {
    // Position, parent index and depth remain distinct fields of this traversal record.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    TocNode(const std::string& label,
            std::uint64_t position,
            unsigned long level,
            std::ptrdiff_t parent)
        : label(label), position(position), level(level), parent(parent) {}
    // NOLINTEND(bugprone-easily-swappable-parameters)

    std::string label;
    std::uint64_t position;
    unsigned long level;
    std::ptrdiff_t parent;
    std::vector<std::size_t> children;
};

std::uint64_t navigationPosition(const reader::Book& book, const epub::NavigationPoint& point) {
    if (!point.path.empty()) {
        return book.positionForPath(point.path, point.fragment);
    }
    // EPUB navigation groups have labels but no link. Their first usable
    // descendant supplies the position; a broken href is never a position zero.
    for (const auto& child : point.children) {
        try {
            return navigationPosition(book, child);
        } catch (const epub::Error&) {
            // Continue to the next usable descendant of this grouping node.
            continue;
        }
    }
    throw epub::Error(epub::ErrorCode::InvalidDocument, "navigation group has no readable target");
}

struct TocModel {
    explicit TocModel(const std::shared_ptr<const reader::Book>& source) : book(source) {
        append(source->package().navigation(), -1, 0);
    }

    void append(const std::vector<epub::NavigationPoint>& points,
                std::ptrdiff_t parent,
                unsigned long level) {
        for (const auto& point : points) {
            const std::size_t index = nodes.size();
            try {
                nodes.emplace_back(point.label, navigationPosition(*book, point), level, parent);
            } catch (const epub::Error&) {
                // A bad link does not hide valid descendants or fabricate a target.
                append(point.children, parent, level);
                continue;
            }
            if (parent >= 0) {
                nodes[static_cast<std::size_t>(parent)].children.push_back(index);
            }
            append(point.children, static_cast<std::ptrdiff_t>(index), level + 1);
        }
    }

    std::uint64_t length(std::size_t index) const {
        const TocNode& node = nodes[index];
        std::uint64_t end = book->size();
        for (std::size_t next = index + 1; next < nodes.size(); ++next) {
            if (nodes[next].level <= node.level && nodes[next].position >= node.position) {
                end = nodes[next].position;
                break;
            }
        }
        return end >= node.position ? end - node.position : 0;
    }

    std::shared_ptr<const reader::Book> book;
    std::vector<TocNode> nodes;
};

class EpubTocEntry final : public ITableOfContentsEntry {
public:
    EpubTocEntry(const std::shared_ptr<const TocModel>& model, std::size_t index)
        : model_(model), index_(index) {}

    unsigned long getLevel() const override {
        return model_->nodes[index_].level;
    }
    unsigned long getLength() const override {
        return nativePosition(model_->length(index_));
    }
    unsigned long getPosition() const override {
        return nativePosition(model_->nodes[index_].position);
    }
    const unsigned char* getNodeClass() const override {
        return reinterpret_cast<const unsigned char*>("chapter");
    }
    const unsigned char* getLabel() const override {
        return bytes(model_->nodes[index_].label);
    }
    Result getFirstChild(ManagedPtr<ITableOfContentsEntry>& output) override {
        const std::vector<std::size_t>& children = model_->nodes[index_].children;
        if (children.empty()) {
            output.reset();
            return TOC_FIRST_CHILD_NOT_FOUND;
        }
        output.reset(new EpubTocEntry(model_, children.front()));
        return SUCCESS;
    }
    Result getLastChild(ManagedPtr<ITableOfContentsEntry>& output) override {
        const std::vector<std::size_t>& children = model_->nodes[index_].children;
        if (children.empty()) {
            output.reset();
            return TOC_LAST_CHILD_NOT_FOUND;
        }
        output.reset(new EpubTocEntry(model_, children.back()));
        return SUCCESS;
    }
    Result getChildren(std::vector<ManagedPtr<ITableOfContentsEntry>>& output) override {
        output.clear();
        const std::vector<std::size_t>& children = model_->nodes[index_].children;
        for (std::size_t child : children) {
            output.push_back(ManagedPtr<ITableOfContentsEntry>(new EpubTocEntry(model_, child)));
        }
        return SUCCESS;
    }
    Result getParent(ManagedPtr<ITableOfContentsEntry>& output) override {
        const std::ptrdiff_t parent = model_->nodes[index_].parent;
        if (parent < 0) {
            output.reset();
            return TOC_PARENT_NOT_FOUND;
        }
        output.reset(new EpubTocEntry(model_, static_cast<std::size_t>(parent)));
        return SUCCESS;
    }
    Result getNextEntry(ManagedPtr<ITableOfContentsEntry>& output) override {
        if (index_ + 1 >= model_->nodes.size()) {
            output.reset();
            return TOC_NEXT_ENTRY_NOT_FOUND;
        }
        output.reset(new EpubTocEntry(model_, index_ + 1));
        return SUCCESS;
    }
    Result getPreviousEntry(ManagedPtr<ITableOfContentsEntry>& output) override {
        if (index_ == 0) {
            output.reset();
            return TOC_PREV_ENTRY_NOT_FOUND;
        }
        output.reset(new EpubTocEntry(model_, index_ - 1));
        return SUCCESS;
    }
    Result getImageResourcesByTagName(std::vector<ManagedPtr<IResource>>& output,
                                      const unsigned char* /*tagName*/) override {
        output.clear();
        return SUCCESS;
    }
    Result getStringsByTagName(std::vector<std::string>& output,
                               const unsigned char* tagName) override {
        output.clear();
        const std::string tag = epub::lowercaseAscii(valueOf(tagName));
        if (tag == "label" || tag == "title") {
            output.push_back(model_->nodes[index_].label);
        }
        return SUCCESS;
    }
#ifdef KINDLE_EPUB_KPP_ABI
    Result getStringMetadataList(std::vector<std::string>& names,
                                 std::vector<std::string>& values) override {
        names.clear();
        values.clear();
        return SUCCESS;
    }
    Result getImageMetadataList(std::vector<std::string>& names,
                                std::vector<ManagedPtr<IResource>>& values) override {
        names.clear();
        values.clear();
        return SUCCESS;
    }
#endif

private:
    std::shared_ptr<const TocModel> model_;
    std::size_t index_;
};

class EpubTableOfContents final : public ITableOfContents {
public:
    explicit EpubTableOfContents(const std::shared_ptr<const TocModel>& model) : model_(model) {}

    Result getEntryFromPosition(ManagedPtr<ITableOfContentsEntry>& output,
                                unsigned long position) override {
        if (position >= model_->book->size()) {
            output.reset();
            return Mobi8SDK::POSITION_OUT_OF_BOUND;
        }
        std::size_t best = 0;
        std::uint64_t bestPosition = 0;
        for (std::size_t i = 0; i < model_->nodes.size(); ++i) {
            if (model_->nodes[i].position <= position &&
                model_->nodes[i].position >= bestPosition) {
                best = i;
                bestPosition = model_->nodes[i].position;
            }
        }
        output.reset(new EpubTocEntry(model_, best));
        return SUCCESS;
    }
    Result getEntryForFirstSection(ManagedPtr<ITableOfContentsEntry>& output) override {
        output.reset(new EpubTocEntry(model_, 0));
        return SUCCESS;
    }

private:
    std::shared_ptr<const TocModel> model_;
};

} // namespace
Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContents>
makeTableOfContents(const std::shared_ptr<const reader::Book>& book) {
    const auto model = std::make_shared<TocModel>(book);
    if (model->nodes.empty()) {
        return {};
    }
    return Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContents>(new EpubTableOfContents(model));
}
} // namespace kindle_epub
