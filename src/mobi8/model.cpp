#include "mobi8/model.hpp"
#include "epub/error.hpp"
#include "providers.hpp"
#include "support.hpp"

#include <climits>
#include <memory>

namespace kindle_epub {
namespace {
using Mobi8SDK::IBookManager;
using Mobi8SDK::IBookSecurityManager;
using Mobi8SDK::IContentProvider;
using Mobi8SDK::IMetadataProvider;
using Mobi8SDK::IResourceContainer;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::OPERATION_NOT_SUPPORTED;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;
using Mobi8SDK::UNEXPECTED_ERROR;

class EpubBookManager final : public IBookManager {
public:
    explicit EpubBookManager(const std::shared_ptr<const reader::Book>& book) : book_(book) {}

    Result getContentProvider(ManagedPtr<IContentProvider>& output,
                              const Mobi8SDK::BookSecurity& /*security*/) override {
        EPUB_TRACE(LOG_INFO,
                   "getContentProvider(BookSecurity) book-size=%lu",
                   nativePosition(book_->size()));
        output = makeContentProvider(book_);
        return SUCCESS;
    }
    Result getContentProvider(ManagedPtr<IContentProvider>& output,
                              ManagedPtr<IBookSecurityManager> /*securityManager*/) override {
        EPUB_TRACE(LOG_INFO,
                   "getContentProvider(SecurityManager) book-size=%lu",
                   nativePosition(book_->size()));
        output = makeContentProvider(book_);
        return SUCCESS;
    }
    Result getUserLocationForPosition(unsigned long& output, unsigned long position) override {
        output = nativePosition(book_->userLocationForPosition(position));
        return SUCCESS;
    }
    Result getPositionForUserLocation(unsigned long& output, unsigned long location) override {
        output = nativePosition(book_->positionForUserLocation(location));
        return SUCCESS;
    }
    Result getMetadataProvider(ManagedPtr<IMetadataProvider>& output) override {
        output = makeMetadataProvider(book_);
        return SUCCESS;
    }
#ifndef KINDLE_EPUB_EARLY_MOBI8_ABI
    bool bookContainsResourceContainers() override {
        return false;
    }
    Result getResourceContainersInfo(std::vector<ManagedPtr<IResourceContainer>>& output) override {
        output.clear();
        return SUCCESS;
    }
    Result attachResourceContainer(const unsigned char* /*path*/,
                                   ManagedPtr<IResourceContainer>& output) override {
        output.reset();
        return OPERATION_NOT_SUPPORTED;
    }

#endif

private:
    std::shared_ptr<const reader::Book> book_;
};

} // namespace
Mobi8SDK::Result createBookManager(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
                                   const char* path) {
    if (path == nullptr || *path == '\0') {
        output.reset();
        return Mobi8SDK::FAILED_LOADING_FILE;
    }
    try {
        const std::shared_ptr<const reader::Book> book = reader::Book::open(path);
        if (book->size() > ULONG_MAX) {
            output.reset();
            return Mobi8SDK::FAILED_LOADING_FILE;
        }
        EPUB_TRACE(LOG_INFO,
                   "createBookManager path='%s' documents=%lu book-size=%lu",
                   path,
                   static_cast<unsigned long>(book->documents().size()),
                   nativePosition(book->size()));
        output.reset(new EpubBookManager(book));
        return SUCCESS;
    } catch (const std::bad_alloc&) {
        output.reset();
        return Mobi8SDK::INSTANTIATION_ERROR;
    } catch (const epub::Error&) {
        output.reset();
        return Mobi8SDK::FAILED_LOADING_FILE;
    } catch (...) {
        output.reset();
        return UNEXPECTED_ERROR;
    }
}

} // namespace kindle_epub
