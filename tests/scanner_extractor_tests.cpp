#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "scanner/abi.hpp"
#include "scanner/stock_api.hpp"

extern "C" int load_kindle_epub_extractor(scanner_handler* handler, void** context);

namespace {

struct TestFailure : public std::runtime_error {
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

std::string postedJson;
std::string deletedUuid;
std::string thumbnailBytes;
std::string thumbnailToDelete;
int deleteStatus = 0;
bool removeThumbnail = false;
int postCount = 0;
int postStatus = 0;
int unavailableApi = 0;

class TemporaryPath {
public:
    TemporaryPath() {
        char pattern[] = "/tmp/kindle-epub-scanner-test-XXXXXX";
        const int descriptor = mkstemp(pattern);
        if (descriptor < 0) {
            throw TestFailure("mkstemp failed");
        }
        close(descriptor);
        std::remove(pattern);
        path_ = pattern;
    }
    ~TemporaryPath() {
        std::remove(path_.c_str());
    }
    const std::string& path() const {
        return path_;
    }
    std::string directory() const {
        return path_.substr(0, path_.find_last_of('/'));
    }
    std::string filename() const {
        return path_.substr(path_.find_last_of('/') + 1U);
    }

private:
    std::string path_;
};

void writeFixture(const std::string& path) {
    using Entries = std::vector<std::pair<std::string, std::string>>;
    Entries entries;
    entries.push_back(std::make_pair("mimetype", "application/epub+zip"));
    entries.push_back(
        std::make_pair("META-INF/container.xml",
                       "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                       "version=\"1.0\"><rootfiles><rootfile full-path=\"OPS/book.opf\" "
                       "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    entries.push_back(
        std::make_pair("OPS/book.opf",
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                       "unique-identifier=\"id\"><metadata "
                       "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                       "<dc:identifier id=\"id\">urn:test:scanner</dc:identifier>"
                       "<dc:title>Scanner EPUB</dc:title><dc:creator>Catalog Author</dc:creator>"
                       "<dc:language>en</dc:language></metadata><manifest>"
                       "<item id=\"chapter\" href=\"chapter.xhtml\" "
                       "media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"cover\" href=\"cover.jpg\" media-type=\"image/jpeg\" "
                       "properties=\"cover-image\"/></manifest>"
                       "<spine><itemref idref=\"chapter\"/></spine></package>"));
    entries.push_back(
        std::make_pair("OPS/chapter.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>Text</body></html>"));
    entries.push_back(std::make_pair("OPS/cover.jpg", "JPEG-COVER"));

    struct archive* writer = archive_write_new();
    check(writer != nullptr, "archive_write_new failed");
    check(archive_write_set_format_zip(writer) == ARCHIVE_OK, "cannot select ZIP output");
    check(archive_write_set_options(writer, "zip:compression=store") == ARCHIVE_OK,
          "cannot select stored ZIP entries");
    check(archive_write_open_filename(writer, path.c_str()) == ARCHIVE_OK,
          "cannot open EPUB fixture");
    constexpr int filePermissions = 0644;
    for (const auto& zipEntry : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, zipEntry.first.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, filePermissions);
        archive_entry_set_size(entry, static_cast<la_int64_t>(zipEntry.second.size()));
        check(archive_write_header(writer, entry) == ARCHIVE_OK, "cannot write ZIP header");
        check(archive_write_data(writer, zipEntry.second.data(), zipEntry.second.size()) ==
                  static_cast<la_ssize_t>(zipEntry.second.size()),
              "cannot write ZIP data");
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

void resetMocks() {
    postedJson.clear();
    deletedUuid.clear();
    thumbnailBytes.clear();
    thumbnailToDelete.clear();
    deleteStatus = 0;
    removeThumbnail = false;
    postCount = 0;
    postStatus = 0;
}

void testUnavailableApi() {
    for (int missing = 1; missing <= 2; ++missing) {
        unavailableApi = missing;
        scanner_handler handler = nullptr;
        void* context = reinterpret_cast<void*>(1);
        check(load_kindle_epub_extractor(&handler, &context) != 0,
              "loader accepted an unavailable stock API");
        check(handler == nullptr && context == nullptr,
              "failed loader exposed a callback or context");
    }
    unavailableApi = 0;
}

void testEvents() {
    TemporaryPath fixture;
    writeFixture(fixture.path());
    resetMocks();

    scanner_handler handler = nullptr;
    void* context = reinterpret_cast<void*>(1);
    check(load_kindle_epub_extractor(&handler, &context) == 0, "extractor loader failed");
    check(handler != nullptr && context == nullptr, "extractor loader ABI mismatch");

    int sourceHandle = 0;
    scanner_event add = {0,
                         fixture.path().c_str(),
                         &sourceHandle,
                         "must-not-be-appended.epub",
                         nullptr,
                         "GL:*.epub"};
    check(handler(&add, context) == 0, "add event failed");
    check(postCount == 1, "add event did not post exactly once");
    check(postedJson.find("\"location\":\"" + fixture.path() + "\"") != std::string::npos,
          "add event did not treat +4 as the complete path");
    check(postedJson.find("\"mimeType\":\"application/x-mobi8-ebook\"") != std::string::npos,
          "catalog MIME missing");
    check(postedJson.find("Scanner EPUB") != std::string::npos, "catalog title missing");
    check(postedJson.find("thumbnail://fixture") != std::string::npos, "catalog thumbnail missing");
    check(thumbnailBytes == "JPEG-COVER", "wrong cover bytes passed to helper");

    const std::string directory = fixture.directory();
    const std::string filename = fixture.filename();
    scanner_event update = {2,
                            directory.c_str(),
                            &sourceHandle,
                            filename.c_str(),
                            "01234567-89ab-cdef-0123-456789abcdef",
                            "GL:*.epub"};
    check(handler(&update, context) == 0, "update event failed");
    check(deletedUuid.empty(), "update deleted existing catalogue state");
    check(postedJson.find("\"update\":") != std::string::npos,
          "update did not use the CCAT update operation");
    check(postedJson.find("\"uuid\":\"01234567-89ab-cdef-0123-456789abcdef\"") != std::string::npos,
          "update did not preserve UUID");
    check(postedJson.find("\"location\":\"" + fixture.path() + "\"") != std::string::npos,
          "update did not combine directory and filename");

    resetMocks();
    TemporaryPath previousCover;
    FILE* previousCoverFile = std::fopen(previousCover.path().c_str(), "wb");
    check(previousCoverFile != nullptr, "cannot create previous cover fixture");
    std::fputs("old cover", previousCoverFile);
    std::fclose(previousCoverFile);
    removeThumbnail = true;
    thumbnailToDelete = previousCover.path();
    postStatus = -1;
    check(handler(&update, context) != 0, "rejected update reported success");
    check(deletedUuid.empty(), "rejected update deleted the existing entry");
    check(access(previousCover.path().c_str(), F_OK) == 0,
          "rejected update removed the existing cover");
    postStatus = 1;
    check(handler(&update, context) != 0 && access(previousCover.path().c_str(), F_OK) == 0,
          "nonzero scanner response accepted or removed the previous cover");
    postStatus = 0;
    check(handler(&update, context) == 0, "replacement update failed");
    check(access(previousCover.path().c_str(), F_OK) != 0,
          "accepted replacement left the obsolete cover behind");

    resetMocks();
    TemporaryPath invalid;
    FILE* invalidFile = std::fopen(invalid.path().c_str(), "wb");
    check(invalidFile != nullptr, "cannot create invalid EPUB fixture");
    std::fputs("not an EPUB", invalidFile);
    std::fclose(invalidFile);
    scanner_event invalidUpdate = {2,
                                   invalid.path().c_str(),
                                   &sourceHandle,
                                   nullptr,
                                   "01234567-89ab-cdef-0123-456789abcdef",
                                   "GL:*.epub"};
    check(handler(&invalidUpdate, context) != 0, "invalid replacement EPUB reported success");
    check(deletedUuid.empty() && postCount == 0,
          "invalid replacement changed the existing catalogue entry");

    scanner_event missingUuid = update;
    missingUuid.uuid = nullptr;
    check(handler(&missingUuid, context) != 0 && postCount == 0,
          "update without UUID created a duplicate entry");

    missingUuid.uuid = "invalid-uuid";
    check(handler(&missingUuid, context) != 0 && postCount == 0,
          "update with malformed UUID reached the catalogue");

    TemporaryPath thumbnail;
    FILE* thumbnailFile = std::fopen(thumbnail.path().c_str(), "wb");
    check(thumbnailFile != nullptr, "cannot create thumbnail fixture");
    std::fputs("old", thumbnailFile);
    std::fclose(thumbnailFile);
    removeThumbnail = true;
    thumbnailToDelete = thumbnail.path();
    scanner_event deletion = {1,
                              directory.c_str(),
                              &sourceHandle,
                              filename.c_str(),
                              "fedcba98-7654-3210-fedc-ba9876543210",
                              "GL:*.epub"};
    deleteStatus = -1;
    check(handler(&deletion, context) != 0, "rejected deletion reported success");
    check(access(thumbnail.path().c_str(), F_OK) == 0,
          "rejected deletion removed the existing cover");
    deleteStatus = 0;
    check(handler(&deletion, context) == 0, "delete event failed");
    check(access(thumbnail.path().c_str(), F_OK) != 0,
          "delete event left the stock thumbnail behind");
    check(deletedUuid == "fedcba98-7654-3210-fedc-ba9876543210", "delete used the wrong UUID");

    scanner_event thumbnailAdd = {3, nullptr, nullptr, nullptr, nullptr, nullptr};
    scanner_event thumbnailUpdate = {4, nullptr, nullptr, nullptr, nullptr, nullptr};
    check(handler(&thumbnailAdd, context) == 0 && handler(&thumbnailUpdate, context) == 0,
          "thumbnail notifications were not acknowledged");
}

} // namespace

extern "C" void scanner_gen_uuid(char* output, std::size_t outputSize) {
    const char value[] = "00000000-0000-0000-0000-000000000001";
    if (outputSize != 0) {
        std::strncpy(output, value, outputSize - 1U);
        output[outputSize - 1U] = '\0';
    }
}

extern "C" int scanner_post_string(const char* request) {
    postedJson = request;
    ++postCount;
    return postStatus;
}

extern "C" int scanner_delete_ccat_entry(const char* uuid) {
    deletedUuid = uuid == nullptr ? "" : uuid;
    return deleteStatus;
}

extern "C" char* construct_thumbnail_uri(const void* /*source*/,
                                         const unsigned char* bytes,
                                         std::size_t length,
                                         int /*isCover*/) {
    thumbnailBytes.assign(reinterpret_cast<const char*>(bytes), length);
    return ::strdup("thumbnail://fixture");
}

extern "C" int should_remove_thumbnail_on_delete_entry(const void* /*source*/,
                                                       const char* /*uuid*/) {
    return removeThumbnail ? 1 : 0;
}

extern "C" char* extractor_get_thumbnail_for_uuid(const char* /*uuid*/) {
    return thumbnailToDelete.empty() ? nullptr : ::strdup(thumbnailToDelete.c_str());
}

namespace kindle_epub {
const ScannerApi& scannerApi() {
    if (unavailableApi == 1) {
        throw TestFailure("stock scanner API is missing");
    }
    static const ScannerApi api = {
        &scanner_gen_uuid, &scanner_post_string, &scanner_delete_ccat_entry};
    return api;
}
const ExtractorApi& extractorApi() {
    if (unavailableApi == 2) {
        throw TestFailure("stock thumbnail API is missing");
    }
    static const ExtractorApi api = {&construct_thumbnail_uri,
                                     &should_remove_thumbnail_on_delete_entry,
                                     &extractor_get_thumbnail_for_uuid};
    return api;
}
} // namespace kindle_epub

int main() {
    try {
        testUnavailableApi();
        testEvents();
        std::cout << "kindle_scanner_extractor_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "kindle_scanner_extractor_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
