#ifndef KINDLE_EPUB_CATALOG_THUMBNAIL_HPP
#define KINDLE_EPUB_CATALOG_THUMBNAIL_HPP

#include <string>

namespace epub {
class Archive;
class Package;
} // namespace epub

namespace epub_catalog {
std::string createThumbnail(const epub::Archive& archive,
                            const epub::Package& package,
                            const std::string& uuid);
void removeThumbnail(const std::string& path);
} // namespace epub_catalog

#endif
