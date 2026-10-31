#ifndef KINDLE_EPUB_ARCHIVE_HPP
#define KINDLE_EPUB_ARCHIVE_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace epub {

struct ArchiveLimits {
    std::size_t maxEntries = 20000;
    std::uint64_t maxEntryBytes = UINT64_C(64) * 1024U * 1024U;
    std::uint64_t maxTotalBytes = UINT64_C(512) * 1024U * 1024U;
};

struct ArchiveEntry {
    std::string path;
    std::string zipPath;
    std::uint64_t size = 0;
    std::uint64_t compressedSize = 0;
    std::uint32_t checksum = 0;
    std::uint16_t compressionMethod = 0;
    bool directory = false;
};

class Archive {
public:
    static std::shared_ptr<Archive> open(const std::string& filename,
                                         const ArchiveLimits& limits = ArchiveLimits());

    const std::string& filename() const {
        return filename_;
    }
    const std::map<std::string, ArchiveEntry>& entries() const {
        return entries_;
    }

    bool contains(const std::string& path) const;
    const ArchiveEntry& entry(const std::string& path) const;
    std::vector<unsigned char> read(const std::string& path) const;
    std::string readText(const std::string& path) const;

private:
    Archive(const std::string& filename, const ArchiveLimits& limits)
        : filename_(filename), limits_(limits) {}

    void index();
    void validateMimetype() const;

    std::string filename_;
    ArchiveLimits limits_;
    struct ZipState;
    std::shared_ptr<ZipState> zip_;
    std::map<std::string, ArchiveEntry> entries_;
};

} // namespace epub

#endif
