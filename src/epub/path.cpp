#include "epub/path.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#include "epub/error.hpp"

namespace epub {
namespace {

const int kHexLetterValue = 10;

bool isHex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

unsigned char hexValue(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(kHexLetterValue + value - 'a');
    }
    return static_cast<unsigned char>(kHexLetterValue + value - 'A');
}

bool hasUriScheme(const std::string& value) {
    if (value.empty() || std::isalpha(static_cast<unsigned char>(value[0])) == 0) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char character = value[i];
        if (character == ':') {
            return true;
        }
        if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '+' &&
            character != '-' && character != '.') {
            return false;
        }
    }
    return false;
}

void appendPathComponent(std::vector<std::string>& components, const std::string& component) {
    if (component.empty() || component == ".") {
        return;
    }
    if (component == "..") {
        if (components.empty()) {
            throw Error(ErrorCode::InvalidPath, "archive reference escapes the EPUB root");
        }
        components.pop_back();
        return;
    }
    for (const unsigned char character : component) {
        if (character < ' ') {
            throw Error(ErrorCode::InvalidPath, "archive path contains a control character");
        }
    }
    components.push_back(component);
}

} // namespace

std::string lowercaseAscii(const std::string& value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::string trimAscii(const std::string& value) {
    std::size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

std::string percentDecodePath(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() && isHex(value[i + 1]) && isHex(value[i + 2])) {
            const unsigned char decoded =
                static_cast<unsigned char>((hexValue(value[i + 1]) << 4U) | hexValue(value[i + 2]));
            if (decoded == 0) {
                throw Error(ErrorCode::InvalidPath, "archive reference contains an encoded NUL");
            }
            result.push_back(static_cast<char>(decoded));
            i += 2;
        } else {
            result.push_back(value[i]);
        }
    }
    return result;
}

std::string canonicalArchivePath(const std::string& input) {
    if (input.empty()) {
        throw Error(ErrorCode::InvalidPath, "archive path is empty");
    }

    std::string path = input;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path[0] == '/') {
        throw Error(ErrorCode::InvalidPath, "absolute paths are not valid inside an EPUB");
    }
    if (path.find('\0') != std::string::npos) {
        throw Error(ErrorCode::InvalidPath, "archive path contains a NUL");
    }

    std::vector<std::string> components;
    std::size_t cursor = 0;
    while (cursor <= path.size()) {
        const std::size_t slash = path.find('/', cursor);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        appendPathComponent(components, path.substr(cursor, end - cursor));
        if (slash == std::string::npos) {
            break;
        }
        cursor = slash + 1;
    }

    if (components.empty()) {
        throw Error(ErrorCode::InvalidPath, "archive path resolves to the EPUB root");
    }

    std::string result;
    result.reserve(path.size());
    for (const auto& component : components) {
        if (!result.empty()) {
            result += '/';
        }
        result += component;
    }
    return result;
}

std::string archiveDirectory(const std::string& path) {
    const std::string canonical = canonicalArchivePath(path);
    const std::size_t slash = canonical.rfind('/');
    return slash == std::string::npos ? std::string() : canonical.substr(0, slash);
}

// The public API distinguishes the base document and reference by their parameter names.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
Reference resolveReference(const std::string& baseDocument, const std::string& originalValue) {
    std::string value = trimAscii(originalValue);
    if (value.find('\0') != std::string::npos) {
        throw Error(ErrorCode::InvalidPath, "archive reference contains a NUL");
    }
    if (hasUriScheme(value) || value.compare(0, 2, "//") == 0) {
        return Reference(Reference::Kind::External, value, {}, {});
    }

    std::string fragment;
    const std::size_t hash = value.find('#');
    if (hash != std::string::npos) {
        fragment = percentDecodePath(value.substr(hash + 1));
        value.erase(hash);
    }
    std::string query;
    const std::size_t question = value.find('?');
    if (question != std::string::npos) {
        query = value.substr(question + 1);
        value.erase(question);
    }

    if (value.empty()) {
        return Reference(
            Reference::Kind::Archive, canonicalArchivePath(baseDocument), query, fragment);
    }
    const std::string decoded = percentDecodePath(value);
    const std::string baseDirectory = archiveDirectory(baseDocument);
    const std::string joined = baseDirectory.empty() ? decoded : baseDirectory + "/" + decoded;
    return Reference(Reference::Kind::Archive, canonicalArchivePath(joined), query, fragment);
}

} // namespace epub
