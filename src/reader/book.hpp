#ifndef KINDLE_EPUB_BOOK_HPP
#define KINDLE_EPUB_BOOK_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "epub/archive.hpp"
#include "epub/package.hpp"
#include "reader/document.hpp"

namespace kindle_epub {
namespace reader {

struct ResourceRecord {
    std::string id;
    std::string path;
    std::string mediaType;
};

struct ResourceData {
    std::string id;
    std::string path;
    std::string mediaType;
    std::vector<unsigned char> bytes;
};

class Book {
public:
    static std::shared_ptr<Book> open(const std::string& filename,
                                      const epub::ArchiveLimits& limits = epub::ArchiveLimits());

    const epub::Archive& archive() const {
        return *archive_;
    }
    const epub::Package& package() const {
        return package_;
    }
    const std::vector<ContentDocument>& documents() const {
        return documents_;
    }
    const std::map<std::string, ResourceRecord>& resources() const {
        return resourcesById_;
    }
    std::uint64_t size() const {
        return size_;
    }

    const ContentDocument& documentForPosition(std::uint64_t position) const;
    const ContentDocument& documentByPath(const std::string& path) const;
    ResourceData readResource(const std::string& idOrUri) const;
    std::uint64_t positionForLink(const std::string& uri) const;
    std::string resourceIdForPath(const std::string& path) const;

    std::uint64_t positionForPath(const std::string& path, const std::string& fragment) const;
    std::uint64_t userLocationForPosition(std::uint64_t position) const;
    std::uint64_t positionForUserLocation(std::uint64_t location) const;

private:
    struct LinkTarget {
        std::string path;
        std::string fragment;
    };

    Book(const std::shared_ptr<epub::Archive>& archive, const epub::Package& package)
        : archive_(archive), package_(package) {}

    void buildResourceIndex();
    void buildDocuments();
    std::string resourceUri(const std::string& path) const;
    std::string linkUri(const std::string& path, const std::string& fragment);

    std::shared_ptr<epub::Archive> archive_;
    epub::Package package_;
    std::vector<ContentDocument> documents_;
    std::map<std::string, std::size_t> documentIndexByPath_;
    std::map<std::string, ResourceRecord> resourcesById_;
    std::map<std::string, std::string> resourceIdByPath_;
    std::map<std::string, LinkTarget> linksByUri_;
    std::uint64_t size_{0};
};

} // namespace reader

} // namespace kindle_epub
#endif
