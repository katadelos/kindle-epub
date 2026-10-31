#include "epub/archive.hpp"
#include "epub/error.hpp"
#include "epub/path.hpp"

#include <zip.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace epub {
namespace {

using ZipHandle = std::unique_ptr<zip_t, decltype(&zip_discard)>;
using ZipFile = std::unique_ptr<zip_file_t, decltype(&zip_fclose)>;

const std::uint32_t kCentralHeaderSignature = 0x02014b50U;
const std::uint32_t kEndSignature = 0x06054b50U;
const std::uint32_t kZip64EndSignature = 0x06064b50U;
const std::uint32_t kZip64LocatorSignature = 0x07064b50U;
const std::uint64_t kZip32Limit = 0xffffffffULL;
const std::uint64_t kCompressedSlack = UINT64_C(1024) * 1024U;

// ZIP field offsets, record lengths, and byte shifts below follow the wire format.
// NOLINTBEGIN(readability-magic-numbers)
std::uint16_t little16(const unsigned char* value) {
    return static_cast<std::uint16_t>(value[0]) | static_cast<std::uint16_t>(value[1] << 8U);
}

std::uint32_t little32(const unsigned char* value) {
    return static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8U) |
           (static_cast<std::uint32_t>(value[2]) << 16U) |
           (static_cast<std::uint32_t>(value[3]) << 24U);
}

std::uint64_t little64(const unsigned char* value) {
    return static_cast<std::uint64_t>(little32(value)) |
           (static_cast<std::uint64_t>(little32(value + 4)) << 32U);
}

bool checkedAdd(std::uint64_t left, std::uint64_t right, std::uint64_t& output) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    output = left + right;
    return true;
}

using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

void readAt(std::FILE* file, std::uint64_t offset, void* output, std::size_t length) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) {
        throw Error(ErrorCode::Io, "cannot seek inside EPUB archive");
    }
    if (std::fread(output, 1, length, file) != length) {
        if (std::ferror(file) != 0) {
            throw Error(ErrorCode::Io, "cannot read EPUB archive");
        }
        throw Error(ErrorCode::InvalidArchive, "unexpected end of EPUB ZIP directory");
    }
}

struct DirectoryLocation {
    DirectoryLocation() = default;

    std::uint64_t entries{0};
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t endOffset{0};
};

DirectoryLocation findDirectory(std::FILE* file, std::uint64_t fileSize) {
    if (fileSize < 22U) {
        throw Error(ErrorCode::InvalidArchive, "EPUB is too small to contain a ZIP directory");
    }
    const std::uint64_t tailSize64 = std::min<std::uint64_t>(fileSize, 65557U);
    const std::size_t tailSize = static_cast<std::size_t>(tailSize64);
    std::vector<unsigned char> tail(tailSize);
    readAt(file, fileSize - tailSize64, tail.data(), tail.size());

    std::size_t endIndex = tail.size();
    for (std::size_t cursor = tail.size() - 22U + 1U; cursor-- > 0U;) {
        if (little32(&tail[cursor]) != kEndSignature) {
            continue;
        }
        const std::uint16_t commentLength = little16(&tail[cursor + 20U]);
        if (cursor + 22U + commentLength == tail.size()) {
            endIndex = cursor;
            break;
        }
    }
    if (endIndex == tail.size()) {
        throw Error(ErrorCode::InvalidArchive, "EPUB has no valid ZIP end-of-directory record");
    }

    const unsigned char* end = &tail[endIndex];
    if (little16(end + 4U) != 0U || little16(end + 6U) != 0U ||
        little16(end + 8U) != little16(end + 10U)) {
        throw Error(ErrorCode::InvalidArchive, "multi-disk EPUB ZIP archives are not supported");
    }

    DirectoryLocation result;
    result.entries = little16(end + 10U);
    result.size = little32(end + 12U);
    result.offset = little32(end + 16U);
    result.endOffset = fileSize - tailSize64 + endIndex;

    const bool needsZip64 =
        result.entries == 0xffffU || result.size == kZip32Limit || result.offset == kZip32Limit;
    if (result.endOffset < 20U) {
        if (!needsZip64) {
            return result;
        }
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP64 directory has no locator");
    }
    unsigned char locator[20];
    readAt(file, result.endOffset - 20U, locator, sizeof(locator));
    if (!needsZip64 && little32(locator) != kZip64LocatorSignature) {
        return result;
    }
    // libzip also honors optional ZIP64 records even when the ZIP32 values fit.
    // Inspect the same locator so its chosen directory cannot bypass this bound.
    if (little32(locator) != kZip64LocatorSignature || little32(locator + 4U) != 0U ||
        little32(locator + 16U) != 1U) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP64 locator is invalid or multi-disk");
    }

    const std::uint64_t zip64Offset = little64(locator + 8U);
    unsigned char zip64[56];
    readAt(file, zip64Offset, zip64, sizeof(zip64));
    if (little32(zip64) != kZip64EndSignature || little64(zip64 + 4U) < 44U ||
        little32(zip64 + 16U) != 0U || little32(zip64 + 20U) != 0U ||
        little64(zip64 + 24U) != little64(zip64 + 32U)) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP64 end-of-directory record is invalid");
    }
    result.entries = little64(zip64 + 32U);
    result.size = little64(zip64 + 40U);
    result.offset = little64(zip64 + 48U);
    result.endOffset = zip64Offset;
    return result;
}

// libzip allocates the directory before exposing its entry count. Bound that
// allocation first, including archives whose actual count exceeds their EOCD
// count. Entry metadata and all ZIP data decoding belong to libzip.
void validateDirectoryCount(std::FILE* file, const ArchiveLimits& limits) {
    struct stat status;
    if (fstat(fileno(file), &status) != 0 || status.st_size < 0) {
        throw Error(ErrorCode::Io, "cannot determine EPUB archive size");
    }
    const std::uint64_t fileSize = static_cast<std::uint64_t>(status.st_size);
    const DirectoryLocation directory = findDirectory(file, fileSize);
    if (directory.entries == 0) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP archive is empty");
    }
    if (directory.entries > limits.maxEntries) {
        throw Error(ErrorCode::LimitExceeded, "EPUB contains more archive entries than permitted");
    }
    std::uint64_t directoryEnd = 0;
    if (!checkedAdd(directory.offset, directory.size, directoryEnd) ||
        directoryEnd > directory.endOffset || directoryEnd > fileSize) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP central directory is outside the archive");
    }
    std::uint64_t cursor = directory.offset;
    for (std::uint64_t index = 0; index < directory.entries; ++index) {
        if (directoryEnd - cursor < 46) {
            throw Error(ErrorCode::InvalidArchive, "EPUB ZIP central directory is truncated");
        }
        unsigned char header[46];
        readAt(file, cursor, header, sizeof(header));
        if (little32(header) != kCentralHeaderSignature) {
            throw Error(ErrorCode::InvalidArchive,
                        "EPUB ZIP central-directory signature is invalid");
        }
        const std::uint64_t length = sizeof(header) +
                                     static_cast<std::uint64_t>(little16(header + 28)) +
                                     little16(header + 30) + little16(header + 32);
        if (length > directoryEnd - cursor) {
            throw Error(ErrorCode::InvalidArchive, "EPUB ZIP central-directory entry is truncated");
        }
        cursor += length;
    }
    if (cursor != directoryEnd) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP central-directory count is inconsistent");
    }
}

// NOLINTEND(readability-magic-numbers)

[[noreturn]] void failZip(const std::string& operation, int code) {
    if (code == ZIP_ER_MEMORY) {
        throw std::bad_alloc();
    }
    ErrorCode category = ErrorCode::InvalidArchive;
    switch (code) {
    case ZIP_ER_OPEN:
    case ZIP_ER_NOENT:
    case ZIP_ER_READ:
    case ZIP_ER_SEEK:
    case ZIP_ER_TELL:
    case ZIP_ER_CLOSE:
        category = ErrorCode::Io;
        break;
    case ZIP_ER_ENCRNOTSUPP:
    case ZIP_ER_NOPASSWD:
    case ZIP_ER_WRONGPASSWD:
        category = ErrorCode::UnsupportedEncryption;
        break;
    default:
        break;
    }
    // libzip error objects own their formatted diagnostic string.
    zip_error_t error;
    zip_error_init_with_code(&error, code);
    std::unique_ptr<zip_error_t, decltype(&zip_error_fini)> cleanup(&error, &zip_error_fini);
    throw Error(category, operation + ": " + zip_error_strerror(&error));
}

ArchiveEntry
readEntry(zip_t* archive, zip_uint64_t index, const ArchiveLimits& limits, std::uint64_t& total) {
    zip_stat_t status;
    zip_uint8_t host = 0;
    zip_uint32_t attributes = 0;
    if (zip_stat_index(archive, index, ZIP_FL_ENC_RAW, &status) < 0 ||
        zip_file_get_external_attributes(archive, index, 0, &host, &attributes) < 0) {
        failZip("cannot inspect EPUB ZIP entry", zip_error_code_zip(zip_get_error(archive)));
    }
    if (status.encryption_method != ZIP_EM_NONE) {
        throw Error(ErrorCode::UnsupportedEncryption,
                    "encrypted EPUB ZIP entries are not supported");
    }
    if (status.comp_method != ZIP_CM_STORE && status.comp_method != ZIP_CM_DEFLATE) {
        throw Error(ErrorCode::InvalidArchive, "EPUB uses an unsupported ZIP compression method");
    }
    if (status.size > limits.maxEntryBytes ||
        status.comp_size > limits.maxEntryBytes + kCompressedSlack) {
        throw Error(ErrorCode::LimitExceeded, "EPUB entry exceeds its size limit");
    }
    if (status.size > limits.maxTotalBytes - total) {
        throw Error(ErrorCode::LimitExceeded, "EPUB exceeds the total expanded-size limit");
    }
    total += status.size;
    const std::string zipPath = status.name;
    if (zipPath.empty()) {
        throw Error(ErrorCode::InvalidArchive, "EPUB contains an entry without a path");
    }
    const bool directory = zipPath.back() == '/' || (attributes & 0x10U) != 0;
    const mode_t type = static_cast<mode_t>(attributes >> 16U) & S_IFMT;
    if (host == ZIP_OPSYS_UNIX && type != 0 && (directory ? type != S_IFDIR : type != S_IFREG)) {
        throw Error(ErrorCode::InvalidArchive, "EPUB contains a linked or non-regular ZIP entry");
    }
    ArchiveEntry entry;
    try {
        entry.path = canonicalArchivePath(zipPath);
    } catch (const Error&) {
        throw Error(ErrorCode::InvalidArchive, "EPUB contains an unsafe ZIP entry path");
    }
    entry.zipPath = zipPath;
    entry.size = status.size;
    entry.compressedSize = status.comp_size;
    entry.checksum = status.crc;
    entry.compressionMethod = status.comp_method;
    entry.directory = directory;
    return entry;
}

} // namespace

// Copies of Archive share an immutable backing ZIP. Keep its mutex with its
// handle so every read, including reads through a copy, uses the same lock.
struct Archive::ZipState {
    explicit ZipState(ZipHandle source) : handle(std::move(source)) {}
    ZipHandle handle;
    std::mutex mutex;
    std::map<std::string, zip_uint64_t> indexes;
};

std::shared_ptr<Archive> Archive::open(const std::string& filename, const ArchiveLimits& limits) {
    if (filename.empty() || filename.find('\0') != std::string::npos) {
        throw Error(ErrorCode::InvalidPath, "EPUB filename is empty or contains a NUL");
    }
    // Resource buffers must remain representable on the 32-bit device.
    const std::uint64_t maxBufferBytes = std::min<std::uint64_t>(
        std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::uint32_t>::max());
    if (limits.maxEntries == 0 || limits.maxEntryBytes == 0 || limits.maxTotalBytes == 0 ||
        limits.maxEntryBytes > maxBufferBytes - kCompressedSlack) {
        throw Error(ErrorCode::LimitExceeded, "invalid EPUB archive limits");
    }
    std::shared_ptr<Archive> archive(new Archive(filename, limits));
    archive->index();
    archive->validateMimetype();
    return archive;
}

void Archive::index() {
    File file(std::fopen(filename_.c_str(), "rb"), &std::fclose);
    if (!file) {
        throw Error(ErrorCode::Io, "cannot open EPUB archive: " + filename_);
    }
    validateDirectoryCount(file.get(), limits_);
    const int descriptor = dup(fileno(file.get()));
    if (descriptor < 0) {
        throw Error(ErrorCode::Io, "cannot duplicate EPUB archive descriptor");
    }
    int error = ZIP_ER_OK;
    ZipHandle handle(zip_fdopen(descriptor, ZIP_RDONLY | ZIP_CHECKCONS, &error), &zip_discard);
    if (!handle) {
        close(descriptor);
        failZip("cannot open EPUB ZIP archive", error);
    }
    zip_ = std::make_shared<ZipState>(std::move(handle));
    zip_t* archive = zip_->handle.get();
    const zip_int64_t count = zip_get_num_entries(archive, ZIP_FL_UNCHANGED);
    if (count < 0) {
        failZip("cannot inspect EPUB ZIP directory", zip_error_code_zip(zip_get_error(archive)));
    }
    if (static_cast<std::uint64_t>(count) > limits_.maxEntries) {
        throw Error(ErrorCode::LimitExceeded, "EPUB contains more archive entries than permitted");
    }
    std::uint64_t total = 0;
    for (zip_uint64_t index = 0; index < static_cast<zip_uint64_t>(count); ++index) {
        const ArchiveEntry entry = readEntry(archive, index, limits_, total);
        if (!entries_.emplace(entry.path, entry).second) {
            throw Error(ErrorCode::InvalidArchive,
                        "EPUB contains duplicate normalized paths: " + entry.path);
        }
        zip_->indexes.emplace(entry.path, index);
    }
}

void Archive::validateMimetype() const {
    if (!contains("mimetype")) {
        throw Error(ErrorCode::InvalidArchive, "EPUB is missing its mimetype entry");
    }
    if (readText("mimetype") != "application/epub+zip") {
        throw Error(ErrorCode::InvalidArchive, "EPUB mimetype entry is not application/epub+zip");
    }
}

bool Archive::contains(const std::string& path) const {
    return entries_.find(canonicalArchivePath(path)) != entries_.end();
}

const ArchiveEntry& Archive::entry(const std::string& path) const {
    const std::string canonical = canonicalArchivePath(path);
    const auto found = entries_.find(canonical);
    if (found == entries_.end() || found->second.directory) {
        throw Error(ErrorCode::MissingResource, "EPUB archive entry does not exist: " + canonical);
    }
    return found->second;
}

std::vector<unsigned char> Archive::read(const std::string& path) const {
    const ArchiveEntry& wanted = entry(path);
    const std::lock_guard<std::mutex> lock(zip_->mutex);
    zip_t* archive = zip_->handle.get();
    ZipFile file(zip_fopen_index(archive, zip_->indexes.at(wanted.path), 0), &zip_fclose);
    if (!file) {
        failZip("cannot read EPUB ZIP entry: " + wanted.path,
                zip_error_code_zip(zip_get_error(archive)));
    }
    std::vector<unsigned char> result(static_cast<std::size_t>(wanted.size));
    std::size_t cursor = 0;
    while (cursor < result.size()) {
        const zip_int64_t count =
            zip_fread(file.get(), result.data() + cursor, result.size() - cursor);
        if (count < 0) {
            failZip("cannot decode EPUB ZIP entry: " + wanted.path,
                    zip_error_code_zip(zip_file_get_error(file.get())));
        }
        if (count == 0) {
            throw Error(ErrorCode::InvalidArchive,
                        "EPUB ZIP entry is shorter than its declared size");
        }
        cursor += static_cast<std::size_t>(count);
    }
    // Read through EOF even for empty entries: libzip validates length and CRC there.
    unsigned char extra = 0;
    const zip_int64_t remaining = zip_fread(file.get(), &extra, 1);
    if (remaining < 0) {
        failZip("cannot validate EPUB ZIP entry: " + wanted.path,
                zip_error_code_zip(zip_file_get_error(file.get())));
    }
    if (remaining != 0) {
        throw Error(ErrorCode::InvalidArchive, "EPUB ZIP entry exceeds its declared size");
    }
    const int closeError = zip_fclose(file.release());
    if (closeError != 0) {
        failZip("cannot close EPUB ZIP entry: " + wanted.path, closeError);
    }
    return result;
}

std::string Archive::readText(const std::string& path) const {
    const auto content = read(path);
    return std::string(content.begin(), content.end());
}

} // namespace epub
