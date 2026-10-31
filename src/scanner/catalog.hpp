#ifndef KINDLE_EPUB_CATALOG_HPP
#define KINDLE_EPUB_CATALOG_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "epub/package.hpp"

namespace kindle_epub {

constexpr std::size_t kUuidLength = 36;

bool validUuid(const char* uuid);

// Keep, replace, and clear are distinct catalogue operations. An empty URI
// cannot accidentally mean either a failed thumbnail helper or a removal.
class ThumbnailChange {
public:
    // Preserve the catalogue interface's existing int-sized enum representation.
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Action : int { Keep, Replace, Clear };

    ThumbnailChange() = default;
    static ThumbnailChange replace(const std::string& uri);
    static ThumbnailChange clear();
    Action action() const {
        return action_;
    }
    const std::string& uri() const {
        return uri_;
    }

private:
    ThumbnailChange(Action action, const std::string& uri) : action_(action), uri_(uri) {}
    Action action_ = Action::Keep;
    std::string uri_;
};

struct CatalogRecord {
    CatalogRecord() = default;

    std::string uuid;
    std::string location;
    std::string bookId;
    std::string packageVersion;
    epub::Metadata metadata;
    std::uint64_t modificationTime{0};
    std::uint64_t contentSize{0};
    ThumbnailChange thumbnail;
};

// Preserve the catalogue interface's existing int-sized enum representation.
// NOLINTNEXTLINE(performance-enum-size)
enum class CatalogOperation : int { Insert, Update };

// Updates replace EPUB-owned metadata only, leaving progress and collections
// untouched. Missing optional metadata is explicitly cleared on an update.
// Every string is JSON-escaped and bounded before entering the catalog.
std::string buildCatalogChange(const CatalogRecord& record, CatalogOperation operation);

} // namespace kindle_epub

#endif
