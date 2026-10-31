#include "reader/css.hpp"
#include "epub/error.hpp"
#include "epub/path.hpp"
#include <cctype>
#include <cstring>
namespace kindle_epub {
namespace reader {
namespace {

bool matchesIgnoreCase(const std::string& text, std::size_t offset, const char* lowercase) {
    const std::size_t length = std::strlen(lowercase);
    if (length > text.size() - offset) {
        return false;
    }
    for (std::size_t index = 0; index < length; ++index) {
        if (std::tolower(static_cast<unsigned char>(text[offset + index])) != lowercase[index]) {
            return false;
        }
    }
    return true;
}

bool isIdentifierByte(char value) {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-' || value == '_';
}

std::size_t skipQuotedString(const std::string& css, std::size_t cursor) {
    const char quote = css[cursor++];
    while (cursor < css.size()) {
        if (css[cursor] == '\\' && cursor + 1 < css.size()) {
            cursor += 2;
        } else if (css[cursor++] == quote) {
            break;
        }
    }
    return cursor;
}

std::size_t displayValueOffset(const std::string& css, std::size_t cursor) {
    static const char property[] = "display";
    if ((cursor != 0 && isIdentifierByte(css[cursor - 1])) ||
        !matchesIgnoreCase(css, cursor, property)) {
        return std::string::npos;
    }
    std::size_t value = cursor + sizeof(property) - 1;
    if (value < css.size() && isIdentifierByte(css[value])) {
        return std::string::npos;
    }
    while (value < css.size() && std::isspace(static_cast<unsigned char>(css[value])) != 0) {
        ++value;
    }
    if (value == css.size() || css[value] != ':') {
        return std::string::npos;
    }
    ++value;
    while (value < css.size() && std::isspace(static_cast<unsigned char>(css[value])) != 0) {
        ++value;
    }
    return value;
}

} // namespace

// CSS source and its base path are distinct string inputs in the public API.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string rewriteCssUrls(const std::string& css,
                           const std::string& documentPath,
                           const ResourceLinkMapper& mapper) {
    std::string output;
    std::size_t cursor = 0;
    while (cursor < css.size()) {
        std::size_t found = std::string::npos;
        for (std::size_t i = cursor; i + 4 <= css.size(); ++i) {
            if (matchesIgnoreCase(css, i, "url(")) {
                found = i;
                break;
            }
        }
        if (found == std::string::npos) {
            output.append(css, cursor);
            break;
        }
        output.append(css, cursor, found - cursor + 4);
        std::size_t valueStart = found + 4;
        while (valueStart < css.size() &&
               std::isspace(static_cast<unsigned char>(css[valueStart])) != 0) {
            output.push_back(css[valueStart++]);
        }
        char terminator = ')';
        if (valueStart < css.size() && (css[valueStart] == '\'' || css[valueStart] == '\"')) {
            terminator = css[valueStart];
            output.push_back(css[valueStart++]);
        }
        std::size_t valueEnd = css.find(terminator, valueStart);
        if (valueEnd == std::string::npos) {
            valueEnd = css.size();
        }
        const std::string original = epub::trimAscii(css.substr(valueStart, valueEnd - valueStart));
        std::string rewritten = original;
        try {
            const epub::Reference reference = epub::resolveReference(documentPath, original);
            if (!reference.isExternal()) {
                const std::string mapped = mapper(reference.path());
                if (!mapped.empty()) {
                    rewritten = mapped;
                }
            }
        } catch (const epub::Error&) {
            // Invalid URLs stay inert because only indexed IDs reach the provider.
            rewritten = original;
        }
        output += rewritten;
        cursor = valueEnd;
    }
    return output;
}

std::string rewritePaginationCss(const std::string& css) {
    std::string result = css;
    std::size_t cursor = 0;
    while (cursor < result.size()) {
        if (cursor + 1 < result.size() && result[cursor] == '/' && result[cursor + 1] == '*') {
            const std::size_t end = result.find("*/", cursor + 2);
            cursor = end == std::string::npos ? result.size() : end + 2;
            continue;
        }
        if (result[cursor] == '\'' || result[cursor] == '"') {
            cursor = skipQuotedString(result, cursor);
            continue;
        }

        const std::size_t value = displayValueOffset(result, cursor);
        if (value == std::string::npos) {
            ++cursor;
            continue;
        }

        struct Replacement {
            const char* source;
            const char* target;
        };
        static const Replacement replacements[] = {
            {"-webkit-inline-flex", "inline-block"},
            {"inline-flex", "inline-block"},
            {"-webkit-flex", "block"},
            {"flex", "block"},
        };
        cursor = value;
        for (auto replacement : replacements) {
            const std::size_t length = std::strlen(replacement.source);
            if (!matchesIgnoreCase(result, value, replacement.source)) {
                continue;
            }
            const std::size_t end = value + length;
            if (end < result.size() && isIdentifierByte(result[end])) {
                continue;
            }
            result.replace(value, length, replacement.target);
            cursor = value + std::strlen(replacement.target);
            break;
        }
    }
    return result;
}

} // namespace reader

} // namespace kindle_epub
