#include "worker_thumbnail.hpp"
#include "epub/archive.hpp"
#include "epub/package.hpp"
#include "scanner/catalog.hpp"
#include "stock_api.hpp"

#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace epub_catalog {
namespace {
const char* const directory = "/mnt/us/system/thumbnails";
const char* const prefix = "/mnt/us/system/thumbnails/epub-";
constexpr int kDirectoryPermissions = 0755;
constexpr int kFilePermissions = 0644;
const char kTemporarySuffix[] = "XXXXXX";
const char kImageExtension[] = ".jpg";

struct TemporaryCover {
    int descriptor{-1};
    std::string input;
    std::string output;
    TemporaryCover() = default;
    TemporaryCover(const TemporaryCover&) = delete;
    TemporaryCover& operator=(const TemporaryCover&) = delete;
    ~TemporaryCover() {
        if (descriptor >= 0) {
            close(descriptor);
        }
        if (!input.empty()) {
            std::remove(input.c_str());
        }
        if (!output.empty()) {
            std::remove(output.c_str());
        }
    }
};
} // namespace

std::string createThumbnail(const epub::Archive& archive,
                            const epub::Package& package,
                            const std::string& uuid) {
    if (package.coverPath().empty()) {
        return std::string();
    }
    if (!kindle_epub::validUuid(uuid.c_str())) {
        throw std::runtime_error("Invalid thumbnail UUID");
    }
    const std::vector<unsigned char> bytes = archive.read(package.coverPath());
    if (bytes.empty()) {
        throw std::runtime_error("EPUB cover image is empty");
    }

    const kindle_epub::ResizeThumbnail resize = kindle_epub::thumbnailResizer();

    if (mkdir(directory, kDirectoryPermissions) != 0 && errno != EEXIST) {
        throw std::runtime_error("Cannot create thumbnail directory");
    }
    struct stat status;
    if (stat(directory, &status) != 0 || !S_ISDIR(status.st_mode)) {
        throw std::runtime_error("Thumbnail directory is unavailable");
    }

    std::string filename = std::string(prefix) + uuid + "-" + kTemporarySuffix;
    TemporaryCover cover;
    // C++11 string::data() is const; mkstemp writes the generated suffix.
    // NOLINTNEXTLINE(readability-container-data-pointer)
    cover.descriptor = mkstemp(&filename[0]);
    if (cover.descriptor < 0) {
        throw std::runtime_error("Cannot create temporary EPUB cover");
    }
    cover.input = filename;
    cover.output = cover.input + kImageExtension;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = write(cover.descriptor, &bytes[written], bytes.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Cannot write temporary EPUB cover");
        }
        written += static_cast<std::size_t>(count);
    }
    const int closed = close(cover.descriptor);
    cover.descriptor = -1;
    if (closed != 0) {
        throw std::runtime_error("Cannot close temporary EPUB cover");
    }

    // Exported C ABI in libextractor_util. The stock cover-art wrapper uses
    // 221 x 335; this helper preserves aspect ratio and never enlarges an image.
    // It consumes the input file and returns malloc-owned "inputPath.jpg".
    const std::unique_ptr<char, decltype(&std::free)> result(resize(cover.input.c_str(), 221, 335),
                                                             &std::free);
    if (!result || cover.output != result.get()) {
        throw std::runtime_error("Stock thumbnail resizer could not decode the EPUB cover");
    }
    if (stat(cover.output.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || chmod(cover.output.c_str(), kFilePermissions) != 0) {
        throw std::runtime_error("EPUB thumbnail was not saved as a readable image");
    }
    std::string output;
    output.swap(cover.output);
    return output;
}

void removeThumbnail(const std::string& path) {
    const std::size_t uuidStart = std::strlen(prefix);
    const std::size_t suffixStart = uuidStart + kindle_epub::kUuidLength + 1;
    const std::size_t suffixEnd = suffixStart + sizeof(kTemporarySuffix) - 1;
    if (path.compare(0, uuidStart, prefix) != 0 ||
        path.size() != suffixEnd + sizeof(kImageExtension) - 1 || path[suffixStart - 1] != '-' ||
        path.compare(suffixEnd, sizeof(kImageExtension) - 1, kImageExtension) != 0 ||
        !kindle_epub::validUuid(path.substr(uuidStart, kindle_epub::kUuidLength).c_str())) {
        return;
    }
    for (std::size_t index = suffixStart; index < suffixEnd; ++index) {
        const char character = path[index];
        const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if (!alphanumeric) {
            return;
        }
    }
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        syslog(LOG_WARNING,
               "Cannot remove EPUB thumbnail '%s': %s",
               path.c_str(),
               std::strerror(errno));
    }
}
} // namespace epub_catalog
