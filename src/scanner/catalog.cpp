#include "scanner/catalog.hpp"

#include <algorithm>
#include <cjson/cJSON.h>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>

namespace kindle_epub {
namespace {

const std::size_t kMaxScalarBytes = 8192;
const std::size_t kMaxAuthorBytes = 2048;
const std::size_t kMaxAuthors = 64;
const std::size_t kMaxLanguageBytes = 128;
const std::size_t kMaxIdentifierBytes = 128;
const std::size_t kMaxVersionBytes = 32;
const std::size_t kMaxDateBytes = 256;
constexpr unsigned char kUtf8ContinuationMask = 0xc0U;
constexpr unsigned char kUtf8ContinuationBits = 0x80U;

std::string bounded(const std::string& value, std::size_t limit) {
    if (value.size() <= limit) {
        return value;
    }
    std::size_t length = limit;
    // Avoid cutting in the middle of a UTF-8 continuation sequence.
    while (length != 0 && (static_cast<unsigned char>(value[length]) & kUtf8ContinuationMask) ==
                              kUtf8ContinuationBits) {
        --length;
    }
    return value.substr(0, length);
}

using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

cJSON* required(cJSON* value) {
    if (value == nullptr) {
        throw std::bad_alloc();
    }
    return value;
}

std::string printJson(const cJSON* value) {
    const std::unique_ptr<char, decltype(&cJSON_free)> text(cJSON_PrintUnformatted(value),
                                                            &cJSON_free);
    if (!text) {
        throw std::bad_alloc();
    }
    return text.get();
}

cJSON* createText(const std::string& raw, std::size_t limit) {
    const std::string value = bounded(raw, limit);
    if (value.find('\0') == std::string::npos) {
        return required(cJSON_CreateString(value.c_str()));
    }

    // cJSON strings are NUL-terminated. Escape each span with the library,
    // preserving embedded NULs as JSON escapes instead of truncating metadata.
    std::string encoded = "\"";
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = value.find('\0', start);
        const std::string span = value.substr(start, end - start);
        const Json text(required(cJSON_CreateString(span.c_str())), &cJSON_Delete);
        const std::string escaped = printJson(text.get());
        encoded.append(escaped, 1, escaped.size() - 2);
        if (end == std::string::npos) {
            break;
        }
        encoded += "\\u0000";
        start = end + 1;
    }
    encoded += '"';
    return required(cJSON_CreateRaw(encoded.c_str()));
}

void addText(cJSON* object, const char* key, const std::string& value, std::size_t limit) {
    cJSON* text = createText(value, limit);
    if (cJSON_AddItemToObject(object, key, text) == 0) {
        cJSON_Delete(text);
        throw std::bad_alloc();
    }
}

void populateLocalized(cJSON* object,
                       const std::string& display,
                       const std::string& language,
                       std::size_t displayLimit = kMaxScalarBytes) {
    addText(object, "display", display, displayLimit);
    if (!language.empty()) {
        addText(object, "language", language, kMaxLanguageBytes);
    }
}

} // namespace

bool validUuid(const char* uuid) {
    if (uuid == nullptr || std::strlen(uuid) != kUuidLength) {
        return false;
    }
    const char format[] = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
    for (std::size_t index = 0; index < kUuidLength; ++index) {
        if (format[index] == '-') {
            if (uuid[index] != '-') {
                return false;
            }
        } else if (std::strchr("0123456789abcdefABCDEF", uuid[index]) == nullptr) {
            return false;
        }
    }
    return true;
}

ThumbnailChange ThumbnailChange::replace(const std::string& uri) {
    if (uri.empty() || uri.size() > kMaxScalarBytes || uri.find('\0') != std::string::npos) {
        throw std::invalid_argument("Thumbnail URI must be nonempty and fit the catalogue field");
    }
    return ThumbnailChange(Action::Replace, uri);
}

ThumbnailChange ThumbnailChange::clear() {
    return ThumbnailChange(Action::Clear, {});
}

std::string buildCatalogChange(const CatalogRecord& record, CatalogOperation operation) {
    if (!validUuid(record.uuid.c_str()) || record.uuid.size() != kUuidLength) {
        throw std::invalid_argument("Catalogue record requires a valid UUID");
    }
    if (record.location.empty() || record.location.size() > kMaxScalarBytes ||
        record.location.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            "Catalogue location must be nonempty and fit the catalogue field");
    }
    const bool update = operation == CatalogOperation::Update;
    const std::string title =
        record.metadata.title.empty() ? "Untitled EPUB" : record.metadata.title;
    const Json root(required(cJSON_CreateObject()), &cJSON_Delete);
    required(cJSON_AddStringToObject(root.get(), "type", "ChangeRequest"));
    cJSON* commands = required(cJSON_AddArrayToObject(root.get(), "commands"));
    cJSON* command = required(cJSON_CreateObject());
    cJSON_AddItemToArray(commands, command);
    cJSON* entry = required(cJSON_AddObjectToObject(command, update ? "update" : "insert"));

    addText(entry, "uuid", record.uuid, kUuidLength);
    addText(entry, "location", record.location, kMaxScalarBytes);
    required(cJSON_AddStringToObject(entry, "type", "Entry:Item"));
    // cJSON numbers use double. Raw decimal integers retain all 64 bits.
    required(cJSON_AddRawToObject(
        entry, "modificationTime", std::to_string(record.modificationTime).c_str()));
    required(cJSON_AddRawToObject(entry, "diskUsage", std::to_string(record.contentSize).c_str()));
    required(
        cJSON_AddRawToObject(entry, "contentSize", std::to_string(record.contentSize).c_str()));
    addText(entry,
            "version",
            record.packageVersion.empty() ? "3.0" : record.packageVersion,
            kMaxVersionBytes);
    // The Java reader requires Mobi8's content class. Keep the real EPUB path
    // so the reader hooks select this backend.
    required(cJSON_AddStringToObject(entry, "mimeType", "application/x-mobi8-ebook"));
    required(cJSON_AddTrueToObject(entry, "isVisibleInHome"));
    required(cJSON_AddFalseToObject(entry, "isArchived"));
    required(cJSON_AddFalseToObject(entry, "isDRMProtected"));

    if (update || !record.metadata.language.empty()) {
        cJSON* languages = required(cJSON_AddArrayToObject(entry, "languages"));
        if (!record.metadata.language.empty()) {
            cJSON_AddItemToArray(languages,
                                 createText(record.metadata.language, kMaxLanguageBytes));
        }
    }

    cJSON* titles = required(cJSON_AddArrayToObject(entry, "titles"));
    cJSON* localizedTitle = required(cJSON_CreateObject());
    cJSON_AddItemToArray(titles, localizedTitle);
    populateLocalized(localizedTitle, title, record.metadata.language);

    if (update || !record.metadata.creators.empty()) {
        cJSON* credits = required(cJSON_AddArrayToObject(entry, "credits"));
        const std::size_t count = std::min(record.metadata.creators.size(), kMaxAuthors);
        for (std::size_t index = 0; index < count; ++index) {
            cJSON* credit = required(cJSON_CreateObject());
            cJSON_AddItemToArray(credits, credit);
            required(cJSON_AddStringToObject(credit, "kind", "Author"));
            cJSON* name = required(cJSON_AddObjectToObject(credit, "name"));
            populateLocalized(
                name, record.metadata.creators[index], record.metadata.language, kMaxAuthorBytes);
        }
    }

    // Stock normal-book presentation excludes transient NEW/SAMPLE displayTags.
    cJSON* displayObjects = required(cJSON_AddArrayToObject(entry, "displayObjects"));
    for (const char* reference : {"titles", "credits"}) {
        cJSON* display = required(cJSON_CreateObject());
        cJSON_AddItemToArray(displayObjects, display);
        required(cJSON_AddStringToObject(display, "ref", reference));
    }
    if (update || !record.metadata.publisher.empty()) {
        addText(entry, "publisher", record.metadata.publisher, kMaxScalarBytes);
    }
    if (update || !record.metadata.publicationDate.empty()) {
        addText(entry, "publicationDate", record.metadata.publicationDate, kMaxDateBytes);
    }
    addText(entry, "cdeKey", record.bookId, kMaxIdentifierBytes);
    required(cJSON_AddStringToObject(entry, "cdeType", "PDOC"));
    addText(entry, "guid", record.bookId, kMaxIdentifierBytes);
    if (record.thumbnail.action() == ThumbnailChange::Action::Replace) {
        addText(entry, "thumbnail", record.thumbnail.uri(), kMaxScalarBytes);
    } else if (record.thumbnail.action() == ThumbnailChange::Action::Clear) {
        // CCAT uses an empty string, rather than null, to clear its image field.
        required(cJSON_AddStringToObject(entry, "thumbnail", ""));
    }
    return printJson(root.get());
}

} // namespace kindle_epub
