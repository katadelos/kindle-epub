#include "worker_request.hpp"
#include "scanner/catalog.hpp"

#include <limits.h>
#include <strings.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace epub_catalog {
namespace {

std::string requestedUuid(const char* uuid) {
    if (uuid == nullptr) {
        return {};
    }
    if (!kindle_epub::validUuid(uuid)) {
        throw std::runtime_error("Invalid requested UUID");
    }
    return uuid;
}

void requireEpubFilename(const char* filename) {
    if (filename == nullptr || *filename == '\0') {
        throw std::runtime_error("EPUB filename is empty");
    }
    const std::size_t length = std::strlen(filename);
    constexpr std::size_t extensionLength = sizeof(".epub") - 1;
    if (length < extensionLength || strcasecmp(filename + length - extensionLength, ".epub") != 0) {
        throw std::runtime_error("EPUB filename must end in .epub");
    }
}

void requireDocumentPath(const std::string& path) {
    const std::string directory = "/mnt/us/documents/";
    if (path.compare(0, directory.size(), directory) != 0) {
        throw std::runtime_error("Scanner path must name an EPUB under documents");
    }
    std::size_t start = directory.size();
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string component = path.substr(start, slash - start);
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error("Scanner path must use a canonical document location");
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
}

} // namespace

// Swapped arguments fail the UUID/path validation at this boundary.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ImportRequest::ImportRequest(const char* filename, const char* uuid, PathScope scope)
    : uuid_(requestedUuid(uuid)) {
    requireEpubFilename(filename);
    if (scope == PathScope::Documents) {
        requireDocumentPath(filename);
    }
    char resolved[PATH_MAX];
    if (realpath(filename, resolved) == nullptr) {
        throw std::runtime_error(std::string("Cannot resolve EPUB: ") + filename);
    }
    requireEpubFilename(resolved);
    path_ = resolved;
    if (scope == PathScope::Documents) {
        requireDocumentPath(path_);
    }
    struct stat status = {};
    if (stat(path_.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        throw std::runtime_error("EPUB path must name a readable regular file");
    }
    size_ = static_cast<std::uint64_t>(status.st_size);
    modificationTime_ = status.st_mtime < 0 ? 0 : static_cast<std::uint64_t>(status.st_mtime);
}

// Swapped arguments fail the UUID/path validation at this boundary.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
DeleteRequest::DeleteRequest(const char* filename, const char* uuid) : uuid_(requestedUuid(uuid)) {
    requireEpubFilename(filename);
    path_ = filename;
    requireDocumentPath(path_);
}

} // namespace epub_catalog
