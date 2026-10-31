#include "reader/book.hpp"
#include "reader/css.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include "epub/error.hpp"
#include "epub/path.hpp"

namespace kindle_epub {
namespace reader {
namespace {

const std::uint64_t kBytesPerUserLocation = 150;

std::string stripQuery(const std::string& value) {
    const std::size_t query = value.find('?');
    return query == std::string::npos ? value : value.substr(0, query);
}

std::string mimeSuffix(const std::string& mediaType) {
    return mediaType.empty() ? std::string() : "?mime=" + mediaType;
}

} // namespace

std::shared_ptr<Book> Book::open(const std::string& filename, const epub::ArchiveLimits& limits) {
    const std::shared_ptr<epub::Archive> archive = epub::Archive::open(filename, limits);
    const epub::Package package = epub::Package::parse(*archive);
    std::shared_ptr<Book> book(new Book(archive, package));
    book->buildResourceIndex();
    book->buildDocuments();
    return book;
}

void Book::buildResourceIndex() {
    std::uint32_t index = 1;
    for (const auto& entry : package_.manifest()) {
        const epub::ManifestItem& item = entry.second;
        if (index > std::numeric_limits<std::uint16_t>::max()) {
            throw epub::Error(epub::ErrorCode::LimitExceeded,
                              "EPUB has too many manifest resources for Kindle IDs");
        }
        std::ostringstream identifier;
        identifier << (item.mediaType == "text/css" ? "kindle:flow:" : "kindle:embed:")
                   << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << index++;
        ResourceRecord record;
        record.id = identifier.str();
        record.path = item.path;
        record.mediaType = item.mediaType;
        resourcesById_.insert(std::make_pair(record.id, record));
        resourceIdByPath_.insert(std::make_pair(record.path, record.id));
    }
}

std::string Book::resourceIdForPath(const std::string& path) const {
    if (path.empty()) {
        return {};
    }
    const auto found = resourceIdByPath_.find(epub::canonicalArchivePath(path));
    return found == resourceIdByPath_.end() ? std::string() : found->second;
}

std::string Book::resourceUri(const std::string& path) const {
    const std::string identifier = resourceIdForPath(path);
    if (identifier.empty()) {
        return {};
    }
    return identifier + mimeSuffix(resourcesById_.at(identifier).mediaType);
}

std::string Book::linkUri(const std::string& path, const std::string& fragment) {
    const LinkTarget target{epub::canonicalArchivePath(path), fragment};
    for (const auto& entry : linksByUri_) {
        if (entry.second.path == target.path && entry.second.fragment == target.fragment) {
            return entry.first;
        }
    }
    const std::string uri = "kindle:epub-link:" + std::to_string(linksByUri_.size() + 1);
    linksByUri_.emplace(uri, target);
    return uri;
}

void Book::buildDocuments() {
    documents_.reserve(package_.spine().size());
    std::uint64_t cursor = 0;
    std::uint64_t nextAid = 0;
    for (const auto& spine : package_.spine()) {
        ContentDocument document = parseContentDocument(
            spine.path,
            archive_->readText(spine.path),
            static_cast<std::uint32_t>(documents_.size()),
            nextAid,
            [this](const std::string& path, const std::string& fragment) {
                return linkUri(path, fragment);
            },
            [this](const std::string& path) { return resourceUri(path); });
        const std::size_t insertion = document.fragmentInsertionOffset;
        if (insertion > document.skeleton.size()) {
            throw epub::Error(epub::ErrorCode::InvalidDocument,
                              "EPUB fragment insertion point is outside its skeleton");
        }
        const std::uint64_t remaining = std::numeric_limits<std::uint64_t>::max() - cursor;
        if (document.skeleton.size() > remaining ||
            document.fragment.size() > remaining - document.skeleton.size()) {
            throw epub::Error(epub::ErrorCode::LimitExceeded,
                              "EPUB content position space overflowed");
        }
        document.startPosition = cursor + insertion;
        document.endPosition = document.startPosition + document.fragment.size();
        for (auto& word : document.words) {
            word.position += document.startPosition;
            word.endPosition += document.startPosition;
        }
        for (auto& anchor : document.anchorOffsets) {
            anchor.second += document.startPosition;
        }
        documentIndexByPath_.insert(std::make_pair(document.path, documents_.size()));
        cursor += document.skeleton.size();
        cursor += document.fragment.size();
        documents_.push_back(std::move(document));
    }
    if (documents_.empty()) {
        throw epub::Error(epub::ErrorCode::MissingSpine,
                          "EPUB contains no renderable spine documents");
    }
    size_ = cursor;
}

const ContentDocument& Book::documentForPosition(std::uint64_t position) const {
    if (position >= size_) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "book position is outside EPUB content");
    }

    // The Kindle position space includes the skeleton bytes surrounding each
    // fragment. Stock Mobi8 maps a position at a fragment end, or in the gap
    // before the following fragment, forward to that following fragment.
    // Searching by start position maps those gaps backward and can leave KRF
    // asking for a fragment at its exact end indefinitely.
    const auto found = std::upper_bound(documents_.begin(),
                                        documents_.end(),
                                        position,
                                        [](std::uint64_t value, const ContentDocument& document) {
                                            return value < document.endPosition;
                                        });
    // The final skeleton suffix belongs to the last fragment.
    return found == documents_.end() ? documents_.back() : *found;
}

const ContentDocument& Book::documentByPath(const std::string& path) const {
    const std::string canonical = epub::canonicalArchivePath(path);
    const std::map<std::string, std::size_t>::const_iterator found =
        documentIndexByPath_.find(canonical);
    if (found == documentIndexByPath_.end()) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "path is not present in the EPUB spine: " + canonical);
    }
    return documents_[found->second];
}

ResourceData Book::readResource(const std::string& idOrUri) const {
    const std::string identifier = stripQuery(idOrUri);
    const auto found = resourcesById_.find(identifier);
    if (found == resourcesById_.end()) {
        throw epub::Error(epub::ErrorCode::MissingResource,
                          "unknown EPUB resource identifier: " + idOrUri);
    }
    ResourceData result;
    result.id = found->second.id;
    result.path = found->second.path;
    result.mediaType = found->second.mediaType;
    result.bytes = archive_->read(found->second.path);
    if (result.mediaType == "text/css") {
        const std::string source(result.bytes.begin(), result.bytes.end());
        const std::string rewritten = rewritePaginationCss(rewriteCssUrls(
            source, result.path, [this](const std::string& path) { return resourceUri(path); }));
        result.bytes.assign(rewritten.begin(), rewritten.end());
    }
    return result;
}

std::uint64_t Book::positionForLink(const std::string& uri) const {
    const auto found = linksByUri_.find(uri);
    if (found == linksByUri_.end()) {
        throw epub::Error(epub::ErrorCode::InvalidDocument, "unknown EPUB link: " + uri);
    }
    return positionForPath(found->second.path, found->second.fragment);
}

std::uint64_t Book::positionForPath(const std::string& path, const std::string& fragment) const {
    const ContentDocument& document = documentByPath(path);
    if (fragment.empty()) {
        return document.startPosition;
    }
    const std::map<std::string, std::uint64_t>::const_iterator anchor =
        document.anchorOffsets.find(fragment);
    if (anchor == document.anchorOffsets.end()) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "EPUB link target does not exist: " + path + "#" + fragment);
    }
    return anchor->second;
}

std::uint64_t Book::userLocationForPosition(std::uint64_t position) const {
    if (position > size_) {
        position = size_;
    }
    return position / kBytesPerUserLocation;
}

std::uint64_t Book::positionForUserLocation(std::uint64_t location) const {
    if (location > std::numeric_limits<std::uint64_t>::max() / kBytesPerUserLocation) {
        return size_;
    }
    return std::min(size_, location * kBytesPerUserLocation);
}

} // namespace reader

} // namespace kindle_epub
