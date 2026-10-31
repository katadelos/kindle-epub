#ifndef KINDLE_EPUB_MOBI8_PROVIDERS_HPP
#define KINDLE_EPUB_MOBI8_PROVIDERS_HPP
#include "mobi8/abi.hpp"
#include "reader/book.hpp"
namespace kindle_epub {
Mobi8SDK::ManagedPtr<Mobi8SDK::IResource> makeResource(const reader::ResourceData& resource,
                                                       const std::string& requestedId);
Mobi8SDK::ManagedPtr<Mobi8SDK::ISkeleton>
makeSkeleton(const std::shared_ptr<const reader::Book>& book, std::size_t index);
Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> makeFragment(
    const std::shared_ptr<const reader::Book>& book, std::size_t index, std::uint64_t position);
Mobi8SDK::ManagedPtr<Mobi8SDK::IWordIterator> makeWordIterator(
    const std::shared_ptr<const reader::Book>& book, std::uint64_t position, bool forward);
Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContents>
makeTableOfContents(const std::shared_ptr<const reader::Book>& book);
Mobi8SDK::ManagedPtr<Mobi8SDK::IContentProvider>
makeContentProvider(const std::shared_ptr<const reader::Book>& book);
Mobi8SDK::ManagedPtr<Mobi8SDK::IMetadataProvider>
makeMetadataProvider(const std::shared_ptr<const reader::Book>& book);
} // namespace kindle_epub
#endif
