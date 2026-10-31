#include "scanner/abi.hpp"
#include "stock_api.hpp"

#include <sys/stat.h>
#include <syslog.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "epub/archive.hpp"
#include "epub/error.hpp"
#include "epub/identity.hpp"
#include "epub/package.hpp"
#include "scanner/catalog.hpp"

namespace {

using StockString = std::unique_ptr<char, decltype(&std::free)>;

const std::uint32_t kScannerAdd = 0;
const std::uint32_t kScannerDelete = 1;
const std::uint32_t kScannerUpdate = 2;
// Scanner event types 3/4 are thumbnail add/update notifications. Stock
// extractors acknowledge them without reposting metadata; cover generation is
// part of the normal add/update request below.
const std::uint32_t kScannerAddThumbnail = 3;
const std::uint32_t kScannerUpdateThumbnail = 4;

std::string eventPath(const scanner_event& event) {
    if (event.path == nullptr || *event.path == '\0') {
        return std::string();
    }
    if (event.filename == nullptr || *event.filename == '\0') {
        return event.path;
    }

    // Firmware 5.18 puts the complete filename in +4 for add events, while
    // 5.16 uses the same directory-in-+4, basename-in-+12 layout as updates.
    // If +4 already names a regular file, do not append the basename again.
    struct stat pathStatus;
    if (stat(event.path, &pathStatus) == 0 && S_ISREG(pathStatus.st_mode)) {
        return event.path;
    }

    std::string result(event.path);
    if (!result.empty() && result[result.size() - 1] != '/') {
        result.push_back('/');
    }
    result += event.filename;
    return result;
}

int deleteEntry(const scanner_event& event) {
    if (!kindle_epub::validUuid(event.uuid)) {
        return 1;
    }
    StockString thumbnail(nullptr, &std::free);
    if (event.source != nullptr &&
        kindle_epub::extractorApi().shouldRemoveThumbnail(event.source, event.uuid) != 0) {
        thumbnail.reset(kindle_epub::extractorApi().thumbnailForUuid(event.uuid));
    }
    if (kindle_epub::scannerApi().deleteEntry(event.uuid) < 0) {
        return 1;
    }
    if (thumbnail) {
        errno = 0;
        const int removed = std::remove(thumbnail.get());
        if (removed != 0 && errno != ENOENT) {
            syslog(LOG_ERR,
                   "kindle-epub-extractor: could not remove old thumbnail for '%s'",
                   event.uuid);
            return 1;
        }
    }
    return 0;
}

kindle_epub::ThumbnailChange
thumbnailForCover(const void* source, const epub::Archive& archive, const std::string& coverPath) {
    if (coverPath.empty()) {
        return kindle_epub::ThumbnailChange::clear();
    }
    if (source == nullptr) {
        return {};
    }
    const std::vector<unsigned char> cover = archive.read(coverPath);
    if (cover.empty()) {
        return {};
    }
    const StockString thumbnail(
        kindle_epub::extractorApi().constructThumbnail(source, cover.data(), cover.size(), 1),
        &std::free);
    if (!thumbnail) {
        return {};
    }
    return kindle_epub::ThumbnailChange::replace(thumbnail.get());
}

int postChange(const scanner_event& event, kindle_epub::CatalogOperation operation) {
    const bool update = operation == kindle_epub::CatalogOperation::Update;
    const std::string path = eventPath(event);
    if (path.empty()) {
        return 1;
    }
    if (update && !kindle_epub::validUuid(event.uuid)) {
        return 1;
    }

    struct stat status;
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        syslog(LOG_ERR, "kindle-epub-extractor: cannot stat '%s'", path.c_str());
        return 1;
    }

    // Classify import failures while the fully assembled file path is alive.
    // The callback catch below also protects deletion and path construction.
    try {
        const std::shared_ptr<epub::Archive> archive = epub::Archive::open(path);
        const epub::Package package = epub::Package::parse(*archive);

        std::string uuid;
        if (update) {
            uuid = event.uuid;
        } else {
            char generatedUuid[kindle_epub::kUuidLength + 1] = {};
            kindle_epub::scannerApi().generateUuid(generatedUuid, sizeof(generatedUuid));
            generatedUuid[sizeof(generatedUuid) - 1] = '\0';
            uuid = generatedUuid;
        }
        if (!kindle_epub::validUuid(uuid.c_str())) {
            syslog(LOG_ERR, "kindle-epub-extractor: scanner generated an invalid UUID");
            return 1;
        }

        kindle_epub::CatalogRecord record;
        record.uuid = uuid;
        record.location = path;
        record.bookId = epub::stableBookId(package.metadata(), path);
        record.packageVersion = package.version();
        record.metadata = package.metadata();
        record.modificationTime =
            status.st_mtime < 0 ? 0 : static_cast<std::uint64_t>(status.st_mtime);
        record.contentSize = static_cast<std::uint64_t>(status.st_size);
        std::string oldThumbnail;
        if (update && event.source != nullptr &&
            kindle_epub::extractorApi().shouldRemoveThumbnail(event.source, event.uuid) != 0) {
            const StockString previous(kindle_epub::extractorApi().thumbnailForUuid(event.uuid),
                                       &std::free);
            if (previous) {
                oldThumbnail = previous.get();
            }
        }

        record.thumbnail = thumbnailForCover(event.source, *archive, package.coverPath());

        const std::string requestText = kindle_epub::buildCatalogChange(record, operation);
        if (kindle_epub::scannerApi().postString(requestText.c_str()) != 0) {
            syslog(LOG_ERR, "kindle-epub-extractor: CCAT rejected '%s'", path.c_str());
            return 1;
        }
        // Keep the old cover through validation and rejected updates. A
        // thumbnail-helper failure leaves CCAT's existing reference intact.
        if (!oldThumbnail.empty() && oldThumbnail != record.thumbnail.uri() &&
            record.thumbnail.action() != kindle_epub::ThumbnailChange::Action::Keep &&
            std::remove(oldThumbnail.c_str()) != 0 && errno != ENOENT) {
            syslog(LOG_WARNING,
                   "kindle-epub-extractor: cannot retire thumbnail '%s'",
                   oldThumbnail.c_str());
        }
        return 0;
    } catch (const epub::Error& error) {
        syslog(LOG_ERR, "kindle-epub-extractor: invalid EPUB '%s': %s", path.c_str(), error.what());
    } catch (const std::bad_alloc&) {
        syslog(LOG_ERR, "kindle-epub-extractor: out of memory for '%s'", path.c_str());
    } catch (const std::exception& error) {
        syslog(
            LOG_ERR, "kindle-epub-extractor: cannot import '%s': %s", path.c_str(), error.what());
    } catch (...) {
        syslog(LOG_ERR, "kindle-epub-extractor: unexpected error for '%s'", path.c_str());
    }
    return 1;
}

int extractEpub(const scanner_event* event, void* /*context*/) {
    if (event == nullptr) {
        return 1;
    }
    try {
        switch (event->type) {
        case kScannerAdd:
            return postChange(*event, kindle_epub::CatalogOperation::Insert);
        case kScannerDelete:
            return deleteEntry(*event);
        case kScannerUpdate:
            // Updating CCAT preserves progress and collections.
            return postChange(*event, kindle_epub::CatalogOperation::Update);
        case kScannerAddThumbnail:
        case kScannerUpdateThumbnail:
            return 0;
        default:
            return 1;
        }
    } catch (const std::exception& error) {
        syslog(LOG_ERR, "kindle-epub-extractor: callback failed: %s", error.what());
    } catch (...) {
        syslog(LOG_ERR, "kindle-epub-extractor: callback failed unexpectedly");
    }
    return 1;
}

} // namespace

extern "C" __attribute__((visibility("default"))) int
load_kindle_epub_extractor(scanner_handler* handler, void** context) {
    if (handler == nullptr || context == nullptr) {
        return 1;
    }
    *handler = nullptr;
    *context = nullptr;
    try {
        kindle_epub::scannerApi();
        kindle_epub::extractorApi();
        *handler = &extractEpub;
        return 0;
    } catch (const std::exception& error) {
        syslog(LOG_ERR, "kindle-epub-extractor: unavailable stock API: %s", error.what());
    } catch (...) {
        syslog(LOG_ERR, "kindle-epub-extractor: stock API discovery failed");
    }
    return 1;
}

extern "C" __attribute__((visibility("default"))) const char* kindle_epub_extractor_version() {
#ifndef KINDLE_EPUB_SCANNER_TARGET
#define KINDLE_EPUB_SCANNER_TARGET "host"
#endif
    return "kindle-epub-extractor/0.1.0 scanner-" KINDLE_EPUB_SCANNER_TARGET;
}
