#include "providers.hpp"
#include "support.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace kindle_epub {
namespace {
using Mobi8SDK::IWord;
using Mobi8SDK::IWordIterator;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;
using Mobi8SDK::WORD_END;

class EpubWord final : public IWord {
public:
    explicit EpubWord(const reader::Word& word) : word_(word) {}

    const unsigned char* get() const override {
        return bytes(word_.text);
    }
    unsigned long getPosition() const override {
        return nativePosition(word_.position);
    }
    unsigned long getEndPosition() const override {
        return nativePosition(word_.endPosition);
    }
    bool hasLeadingSpace() const override {
        return word_.leadingSpace;
    }

private:
    reader::Word word_;
};

class EpubWordIterator final : public IWordIterator {
public:
    EpubWordIterator(const std::shared_ptr<const reader::Book>& book,
                     std::uint64_t position,
                     bool forward)
        : book_(book), forward_(forward) {
        for (const auto& document : book_->documents()) {
            const std::vector<reader::Word>& words = document.words;
            for (const auto& word : words) {
                words_.push_back(&word);
            }
        }
        seek(position);
    }

    bool hasNext() override {
        return cursor_ >= 0 && cursor_ < static_cast<std::ptrdiff_t>(words_.size());
    }

    Result next(ManagedPtr<IWord>& output) override {
        if (!hasNext()) {
            output.reset();
            return WORD_END;
        }
        output.reset(new EpubWord(*words_[static_cast<std::size_t>(cursor_)]));
        cursor_ += forward_ ? 1 : -1;
        return SUCCESS;
    }

    Result next(std::vector<ManagedPtr<IWord>>& output, int count) override {
        output.clear();
        if (count < 0) {
            return Mobi8SDK::INVALID_INDEX;
        }
        for (int i = 0; i < count && hasNext(); ++i) {
            output.push_back(
                ManagedPtr<IWord>(new EpubWord(*words_[static_cast<std::size_t>(cursor_)])));
            cursor_ += forward_ ? 1 : -1;
        }
        return output.empty() && count != 0 ? WORD_END : SUCCESS;
    }

    Result gotoPosition(unsigned long position) override {
        if (position > book_->size()) {
            return Mobi8SDK::POSITION_OUT_OF_BOUND;
        }
        seek(position);
        return hasNext() ? SUCCESS : WORD_END;
    }

    // Stock Mobi8WordIterator returns 1. KRF consumes this appended ABI slot
    // when it selects text-position behavior.
    unsigned long getWordIteratorType() override {
        return 1;
    }

private:
    void seek(std::uint64_t position) {
        if (forward_) {
            cursor_ = static_cast<std::ptrdiff_t>(words_.size());
            for (std::size_t i = 0; i < words_.size(); ++i) {
                if (words_[i]->endPosition > position) {
                    cursor_ = static_cast<std::ptrdiff_t>(i);
                    break;
                }
            }
        } else {
            cursor_ = -1;
            for (std::size_t i = 0; i < words_.size(); ++i) {
                if (words_[i]->position <= position) {
                    cursor_ = static_cast<std::ptrdiff_t>(i);
                } else {
                    break;
                }
            }
        }
    }

    std::shared_ptr<const reader::Book> book_;
    bool forward_;
    std::vector<const reader::Word*> words_;
    std::ptrdiff_t cursor_{-1};
};

} // namespace
Mobi8SDK::ManagedPtr<Mobi8SDK::IWordIterator> makeWordIterator(
    const std::shared_ptr<const reader::Book>& book, std::uint64_t position, bool forward) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::IWordIterator>(
        new EpubWordIterator(book, position, forward));
}
} // namespace kindle_epub
