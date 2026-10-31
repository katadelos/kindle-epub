#include "epub/archive.hpp"
#include "epub/identity.hpp"
#include "epub/package.hpp"
#include "scanner/catalog.hpp"
#include "shared_library.hpp"
#include "stock_api.hpp"
#include "worker_request.hpp"
#include "worker_runtime.hpp"
#include "worker_thumbnail.hpp"
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>
namespace epub_catalog {
namespace {
using kindle_epub::SharedLibrary;
constexpr int kLockPermissions = 0644;
constexpr int kQueryTimeoutMs = 1500;
// The daemon and explicit imports can run concurrently. Serialize the entire
// lookup/write operation, including thumbnail replacement, across processes.
class CatalogMutationLock {
public:
    CatalogMutationLock()
        : descriptor_(open("/var/local/kindleepub-catalog.lock",
                           O_RDONLY | O_CREAT | O_CLOEXEC,
                           kLockPermissions)) {
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open EPUB catalogue lock");
        }
        while (flock(descriptor_, LOCK_EX) != 0) {
            if (errno == EINTR) {
                continue;
            }
            close(descriptor_);
            throw std::runtime_error("Cannot lock EPUB catalogue mutation");
        }
    }
    ~CatalogMutationLock() {
        close(descriptor_);
    }
    CatalogMutationLock(const CatalogMutationLock&) = delete;
    CatalogMutationLock& operator=(const CatalogMutationLock&) = delete;

private:
    int descriptor_;
};

// Read only: all catalogue mutations go through libscanner's ChangeRequest
// API. Looking up the existing row makes rescans and repeated manual imports
// idempotent without depending on libscanner's private result-list layout.
struct Entry {
    std::string uuid;
    std::string location;
    std::string thumbnail;
};

enum class LookupField : std::uint8_t { Location, Uuid };

Entry findEntry(const char* value, LookupField field) {
    using Open = int (*)(const char*, void**, int, const char*);
    using Close = int (*)(void*);
    using Prepare = int (*)(void*, const char*, int, void**, const char**);
    using Bind = int (*)(void*, int, const char*, int, void (*)(void*));
    using Step = int (*)(void*);
    using Column = const unsigned char* (*)(void*, int);
    using Timeout = int (*)(void*, int);
    constexpr int sqliteOk = 0;
    constexpr int sqliteRow = 100;
    constexpr int sqliteDone = 101;
    constexpr int sqliteOpenReadOnly = 1;

    const SharedLibrary sqlite("libsqlite3.so.0");
    const Close closeDatabase = sqlite.resolve<Close>("sqlite3_close");
    const Close finalize = sqlite.resolve<Close>("sqlite3_finalize");
    void* rawDatabase = nullptr;
    const int opened = sqlite.resolve<Open>("sqlite3_open_v2")(
        "/var/local/cc.db", &rawDatabase, sqliteOpenReadOnly, nullptr);
    std::unique_ptr<void, Close> database(rawDatabase, closeDatabase);
    if (opened != sqliteOk) {
        throw std::runtime_error("Cannot open content catalogue read-only");
    }
    if (sqlite.resolve<Timeout>("sqlite3_busy_timeout")(database.get(), kQueryTimeoutMs) !=
        sqliteOk) {
        throw std::runtime_error("Cannot set catalogue query timeout");
    }

    const char* query =
        field == LookupField::Uuid
            ? "SELECT p_uuid,p_location,p_thumbnail FROM Entries WHERE p_uuid = ?1"
            : "SELECT p_uuid,p_location,p_thumbnail FROM Entries WHERE p_location = ?1";
    void* rawStatement = nullptr;
    const int prepared = sqlite.resolve<Prepare>("sqlite3_prepare_v2")(
        database.get(), query, -1, &rawStatement, nullptr);
    std::unique_ptr<void, Close> statement(rawStatement, finalize);
    if (prepared != sqliteOk || sqlite.resolve<Bind>("sqlite3_bind_text")(
                                    statement.get(), 1, value, -1, nullptr) != sqliteOk) {
        throw std::runtime_error("Cannot query content catalogue");
    }

    const Step step = sqlite.resolve<Step>("sqlite3_step");
    const Column column = sqlite.resolve<Column>("sqlite3_column_text");
    Entry entry;
    int result = step(statement.get());
    if (result == sqliteRow) {
        const unsigned char* uuid = column(statement.get(), 0);
        const unsigned char* location = column(statement.get(), 1);
        if (uuid == nullptr || location == nullptr ||
            !kindle_epub::validUuid(reinterpret_cast<const char*>(uuid))) {
            throw std::runtime_error("Invalid catalogue UUID or location");
        }
        entry.uuid = reinterpret_cast<const char*>(uuid);
        entry.location = reinterpret_cast<const char*>(location);
        const unsigned char* thumbnail = column(statement.get(), 2);
        if (thumbnail != nullptr) {
            entry.thumbnail = reinterpret_cast<const char*>(thumbnail);
        }
        result = step(statement.get());
        if (result == sqliteRow) {
            throw std::runtime_error("Multiple catalogue entries name this EPUB");
        }
    }
    if (result != sqliteDone) {
        throw std::runtime_error("Content catalogue query failed");
    }
    return entry;
}

} // namespace
std::string importFile(const ImportRequest& request) {
    const std::string& path = request.path();
    const std::string& requestedUuid = request.uuid();
    const std::shared_ptr<epub::Archive> archive = epub::Archive::open(path);
    const epub::Package package = epub::Package::parse(*archive);
    const CatalogMutationLock mutationLock;
    Entry existing = findEntry(path.c_str(), LookupField::Location);
    if (!requestedUuid.empty() && !existing.uuid.empty() && existing.uuid != requestedUuid) {
        throw std::runtime_error("Requested UUID does not match the EPUB catalogue entry");
    }
    if (existing.uuid.empty() && !requestedUuid.empty()) {
        existing = findEntry(requestedUuid.c_str(), LookupField::Uuid);
        if (!existing.uuid.empty()) {
            struct stat oldStatus;
            const std::size_t length = existing.location.size();
            constexpr std::size_t extensionLength = sizeof(".epub") - 1;
            errno = 0;
            const int oldExists = stat(existing.location.c_str(), &oldStatus);
            const int oldError = errno;
            if (length < extensionLength ||
                strcasecmp(existing.location.c_str() + length - extensionLength, ".epub") != 0 ||
                oldExists == 0 || (oldError != ENOENT && oldError != ENOTDIR)) {
                throw std::runtime_error("Requested UUID belongs to another existing book");
            }
        }
    }

    const kindle_epub::ScannerApi& scanner = kindle_epub::scannerApi();
    std::string uuid = existing.uuid.empty() ? requestedUuid : existing.uuid;
    if (uuid.empty()) {
        char generated[kindle_epub::kUuidLength + 1] = {};
        scanner.generateUuid(generated, sizeof(generated));
        generated[sizeof(generated) - 1] = '\0';
        if (!kindle_epub::validUuid(generated)) {
            throw std::runtime_error("Scanner generated an invalid UUID");
        }
        uuid = generated;
    }

    kindle_epub::CatalogRecord record;
    record.uuid = uuid;
    record.location = path;
    record.bookId = epub::stableBookId(package.metadata(), path);
    record.packageVersion = package.version();
    record.metadata = package.metadata();
    record.modificationTime = request.modificationTime();
    record.contentSize = request.size();
    try {
        const std::string thumbnail = createThumbnail(*archive, package, uuid);
        record.thumbnail = thumbnail.empty() ? kindle_epub::ThumbnailChange::clear()
                                             : kindle_epub::ThumbnailChange::replace(thumbnail);
    } catch (const std::exception& error) {
        // A bad cover must not make an otherwise readable book disappear.
        // Keep its previous image until a valid replacement is available.
        syslog(LOG_WARNING, "EPUB cover '%s': %s", path.c_str(), error.what());
    }
    const std::string change = kindle_epub::buildCatalogChange(
        record,
        existing.uuid.empty() ? kindle_epub::CatalogOperation::Insert
                              : kindle_epub::CatalogOperation::Update);
    // Keep the JSON object opaque: Kindle's cJSON_Parse has an extra
    // firmware-specific parameter that upstream cJSON does not expose.
    if (scanner.postString(change.c_str()) != 0) {
        if (record.thumbnail.action() == kindle_epub::ThumbnailChange::Action::Replace &&
            record.thumbnail.uri() != existing.thumbnail) {
            removeThumbnail(record.thumbnail.uri());
        }
        throw std::runtime_error("Content catalogue rejected the EPUB metadata");
    }
    if (record.thumbnail.action() != kindle_epub::ThumbnailChange::Action::Keep &&
        record.thumbnail.uri() != existing.thumbnail) {
        removeThumbnail(existing.thumbnail);
    }
    return uuid;
}

void deleteFile(const DeleteRequest& request) {
    const std::string& requestedUuid = request.uuid();
    const CatalogMutationLock mutationLock;
    const Entry existing = findEntry(request.path().c_str(), LookupField::Location);
    const std::string& uuid = existing.uuid;
    if (uuid.empty()) {
        return;
    }
    if (!requestedUuid.empty() && uuid != requestedUuid) {
        throw std::runtime_error("Delete UUID does not match the EPUB catalogue entry");
    }
    if (kindle_epub::scannerApi().deleteEntry(uuid.c_str()) < 0) {
        throw std::runtime_error("Content catalogue rejected EPUB deletion");
    }
    removeThumbnail(existing.thumbnail);
}

} // namespace epub_catalog
