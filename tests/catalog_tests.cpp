#include <cjson/cJSON.h>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "scanner/catalog.hpp"
#include "scanner/worker_request.hpp"

namespace {

struct TestFailure : public std::runtime_error {
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

void testCatalogInsert() {
    kindle_epub::CatalogRecord record;
    record.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    record.location = "/mnt/us/documents/quote\"test.epub";
    record.bookId = "EPUB-A1B2C3D4";
    record.packageVersion = "3.0";
    record.metadata.title = "A \"native\" EPUB\nTitle";
    record.metadata.creators.push_back("Ada \\ Reader");
    record.metadata.language = "en";
    record.metadata.publisher = "Test Press";
    record.metadata.description = "Direct parser";
    record.metadata.publicationDate = "2026-09-04";
    constexpr std::uint64_t sampleModificationTime = 1788494400;
    constexpr std::uint64_t sampleContentSize = 123456;
    record.modificationTime = sampleModificationTime;
    record.contentSize = sampleContentSize;

    const std::string json =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Insert);
    cJSON* parsed = cJSON_Parse(json.c_str());
    check(parsed != nullptr, "catalog ChangeRequest is not valid JSON");
    cJSON_Delete(parsed);

    check(json.find("\"type\":\"ChangeRequest\"") != std::string::npos,
          "catalog envelope type is absent");
    check(json.find("\"mimeType\":\"application/x-mobi8-ebook\"") != std::string::npos,
          "catalog MIME is not EPUB");
    check(json.find("\"cdeType\":\"PDOC\"") != std::string::npos, "catalog PDOC type is absent");
    check(json.find("\"displayObjects\":[{\"ref\":\"titles\"},"
                    "{\"ref\":\"credits\"}]") != std::string::npos,
          "catalog title/credit display objects are absent");
    check(json.find("\"displayTags\"") == std::string::npos,
          "ordinary sideloaded books must not carry NEW/SAMPLE display tags");
    check(json.find("A \\\"native\\\" EPUB\\nTitle") != std::string::npos,
          "catalog text was not JSON-escaped");
    check(json.find("Ada \\\\ Reader") != std::string::npos, "catalog author was not JSON-escaped");
}

void testCatalogUpdateClearsMetadata() {
    kindle_epub::CatalogRecord record;
    record.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    record.location = "/mnt/us/documents/book.epub";
    record.thumbnail = kindle_epub::ThumbnailChange::clear();
    const std::string json =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Update);
    cJSON* parsed = cJSON_Parse(json.c_str());
    check(parsed != nullptr, "catalog update is not valid JSON");
    cJSON_Delete(parsed);
    check(json.find("\"update\":") != std::string::npos &&
              json.find("\"insert\":") == std::string::npos,
          "metadata refresh must update the existing entry");
    check(json.find("\"languages\":[]") != std::string::npos &&
              json.find("\"credits\":[]") != std::string::npos &&
              json.find("\"publisher\":\"\"") != std::string::npos &&
              json.find("\"publicationDate\":\"\"") != std::string::npos &&
              json.find("\"thumbnail\":\"\"") != std::string::npos,
          "removed EPUB metadata must clear its catalogue values");
    check(json.find("percentFinished") == std::string::npos &&
              json.find("lastAccess") == std::string::npos &&
              json.find("collections") == std::string::npos,
          "metadata refresh must not overwrite reader-owned state");
}

void testCatalogBoundaryValidation() {
    kindle_epub::CatalogRecord record;
    record.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    record.location = "/mnt/us/documents/book.epub";
    record.thumbnail = kindle_epub::ThumbnailChange::replace("thumbnail://cover");
    const std::string replaced =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Update);
    check(replaced.find("thumbnail://cover") != std::string::npos,
          "replacement thumbnail was omitted");

    record.thumbnail = kindle_epub::ThumbnailChange();
    const std::string kept =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Update);
    check(kept.find("thumbnail") == std::string::npos,
          "keeping a thumbnail must leave the catalogue field unchanged");

    bool rejected = false;
    try {
        kindle_epub::ThumbnailChange::replace("");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "empty replacement thumbnail was accepted");

    record.uuid = "invalid-uuid";
    rejected = false;
    try {
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Insert);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "malformed UUID crossed the catalogue boundary");
}

void testWorkerRequestValidation() {
    const char* uuid = "01234567-89ab-cdef-0123-456789abcdef";
    const epub_catalog::DeleteRequest deletion("/mnt/us/documents/missing.epub", uuid);
    check(deletion.uuid() == uuid, "deletion changed the requested UUID");
    const char* invalidPaths[] = {nullptr,
                                  "",
                                  "/tmp/book.epub",
                                  "/mnt/us/documents/../book.epub",
                                  "/mnt/us/documents/./book.epub",
                                  "/mnt/us/documents/book.txt"};
    for (const char* path : invalidPaths) {
        bool rejected = false;
        try {
            const epub_catalog::DeleteRequest invalid(path, uuid);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        check(rejected, "invalid scanner deletion path was accepted");
    }
    bool rejected = false;
    try {
        const epub_catalog::ImportRequest invalid(
            "/missing.epub", "invalid-uuid", epub_catalog::PathScope::Any);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) == "Invalid requested UUID";
    }
    check(rejected, "import did filesystem work before validating its UUID");
}

void testExactIntegersAndEmbeddedNul() {
    kindle_epub::CatalogRecord record;
    record.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    record.location = "/mnt/us/documents/book.epub";
    record.modificationTime = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t firstInexactDoubleInteger = 9007199254740993ULL;
    record.contentSize = firstInexactDoubleInteger;
    const char title[] = "\0A\0\0B\0\"\\\n";
    record.metadata.title.assign(title, sizeof(title) - 1);

    const std::string json =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Insert);
    check(json.find("\"modificationTime\":18446744073709551615,") != std::string::npos,
          "uint64 maximum was rounded during JSON serialization");
    check(json.find("\"contentSize\":9007199254740993,") != std::string::npos &&
              json.find("\"diskUsage\":9007199254740993,") != std::string::npos,
          "integers above double precision were rounded");
    check(json.find(R"("display":"\u0000A\u0000\u0000B\u0000\"\\\n")") != std::string::npos,
          "embedded NUL or adjacent escaped characters were lost");
}

void testMetadataBounds() {
    constexpr std::size_t titleLimit = 8192;
    constexpr std::size_t languageLimit = 128;
    constexpr std::size_t authorLimit = 2048;
    constexpr std::size_t versionLimit = 32;
    constexpr std::size_t dateLimit = 256;
    constexpr int authorCountLimit = 64;
    kindle_epub::CatalogRecord record;
    record.uuid = "01234567-89ab-cdef-0123-456789abcdef";
    record.location = "/mnt/us/documents/book.epub";
    record.metadata.title = std::string(titleLimit - 1, 't') + "\xc3\xa9";
    record.metadata.language = std::string(languageLimit - 1, 'l') + "\xc3\xa9";
    record.metadata.creators.assign(authorCountLimit + 1,
                                    std::string(authorLimit - 1, 'a') + "\xc3\xa9");
    record.packageVersion = std::string(versionLimit - 1, 'v') + "\xc3\xa9";
    record.metadata.publicationDate = std::string(dateLimit - 1, 'd') + "\xc3\xa9";

    const std::string json =
        kindle_epub::buildCatalogChange(record, kindle_epub::CatalogOperation::Insert);
    const std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json.c_str()),
                                                               &cJSON_Delete);
    check(root != nullptr, "bounded catalogue metadata is not valid JSON");
    cJSON* command =
        cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root.get(), "commands"), 0);
    cJSON* entry = cJSON_GetObjectItemCaseSensitive(command, "insert");
    cJSON* title = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(entry, "titles"), 0);
    cJSON* titleText = cJSON_GetObjectItemCaseSensitive(title, "display");
    check(cJSON_IsString(titleText) != 0 &&
              std::string(titleText->valuestring) == std::string(titleLimit - 1, 't'),
          "title limit split a UTF-8 code point");
    cJSON* language = cJSON_GetObjectItemCaseSensitive(title, "language");
    check(cJSON_IsString(language) != 0 &&
              std::string(language->valuestring) == std::string(languageLimit - 1, 'l'),
          "language limit split a UTF-8 code point");
    cJSON* credits = cJSON_GetObjectItemCaseSensitive(entry, "credits");
    check(cJSON_GetArraySize(credits) == authorCountLimit, "author count limit was not preserved");
    cJSON* author =
        cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(credits, authorCountLimit - 1), "name");
    cJSON* authorText = cJSON_GetObjectItemCaseSensitive(author, "display");
    check(cJSON_IsString(authorText) != 0 &&
              std::string(authorText->valuestring) == std::string(authorLimit - 1, 'a'),
          "author limit split a UTF-8 code point");
    cJSON* version = cJSON_GetObjectItemCaseSensitive(entry, "version");
    check(cJSON_IsString(version) != 0 &&
              std::string(version->valuestring) == std::string(versionLimit - 1, 'v'),
          "version limit was not preserved");
    cJSON* date = cJSON_GetObjectItemCaseSensitive(entry, "publicationDate");
    check(cJSON_IsString(date) != 0 &&
              std::string(date->valuestring) == std::string(dateLimit - 1, 'd'),
          "date limit was not preserved");
}

} // namespace

int main() {
    try {
        testCatalogInsert();
        testCatalogUpdateClearsMetadata();
        testCatalogBoundaryValidation();
        testWorkerRequestValidation();
        testExactIntegersAndEmbeddedNul();
        testMetadataBounds();
        std::cout << "kindle_catalog_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "kindle_catalog_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
