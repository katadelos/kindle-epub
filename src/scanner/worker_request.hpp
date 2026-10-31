#ifndef KINDLE_EPUB_WORKER_REQUEST_HPP
#define KINDLE_EPUB_WORKER_REQUEST_HPP

#include <cstdint>
#include <string>

namespace epub_catalog {

// Keep the request constructor's existing int-sized parameter representation.
// NOLINTNEXTLINE(performance-enum-size)
enum class PathScope : int { Any, Documents };

// Construction validates the complete request before archive or catalogue work.
// An import always names a resolved, regular EPUB file.
class ImportRequest {
public:
    ImportRequest(const char* filename, const char* requestedUuid, PathScope scope);
    const std::string& path() const {
        return path_;
    }
    const std::string& uuid() const {
        return uuid_;
    }
    std::uint64_t size() const {
        return size_;
    }
    std::uint64_t modificationTime() const {
        return modificationTime_;
    }

private:
    std::string path_;
    std::string uuid_;
    std::uint64_t size_{0};
    std::uint64_t modificationTime_{0};
};

// A deletion uses the catalogue path because the file may already be gone.
class DeleteRequest {
public:
    DeleteRequest(const char* filename, const char* requestedUuid);
    const std::string& path() const {
        return path_;
    }
    const std::string& uuid() const {
        return uuid_;
    }

private:
    std::string path_;
    std::string uuid_;
};

} // namespace epub_catalog
#endif
