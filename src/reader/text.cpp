#include "reader/text.hpp"
#include "epub/path.hpp"
#include <cctype>
#include <cstdlib>
#include <utility>
namespace kindle_epub {
namespace reader {
namespace {
// Unicode scalar limits and UTF-8 masks below follow the encoding specification.
// NOLINTBEGIN(readability-magic-numbers)
bool decodeEntity(const std::string& entity, std::string& output, bool& whitespace) {
    whitespace = false;
    if (entity == "amp") {
        output = "&";
    } else if (entity == "lt") {
        output = "<";
    } else if (entity == "gt") {
        output = ">";
    } else if (entity == "quot") {
        output = "\"";
    } else if (entity == "apos") {
        output = "'";
    } else if (entity == "nbsp") {
        output = " ";
        whitespace = true;
    } else if (!entity.empty() && entity[0] == '#') {
        char* end = nullptr;
        const int base = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X') ? 16 : 10;
        const char* first = entity.c_str() + (base == 16 ? 2 : 1);
        const unsigned long codepoint = std::strtoul(first, &end, base);
        if (end == first || *end != '\0' || codepoint > 0x10ffffUL ||
            (codepoint >= 0xd800UL && codepoint <= 0xdfffUL)) {
            return false;
        }
        output.clear();
        if (codepoint <= 0x7fUL) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffUL) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffUL) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
        whitespace = codepoint == 0x20UL || codepoint == 0x09UL || codepoint == 0x0aUL ||
                     codepoint == 0x0dUL || codepoint == 0xa0UL;
    } else {
        return false;
    }
    return true;
}

// NOLINTEND(readability-magic-numbers)

std::string decodeAttributeEntities(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    std::size_t cursor = 0;
    while (cursor < value.size()) {
        if (value[cursor] == '&') {
            const std::size_t semicolon = value.find(';', cursor + 1);
            if (semicolon != std::string::npos) {
                std::string decoded;
                bool whitespace = false;
                if (decodeEntity(
                        value.substr(cursor + 1, semicolon - cursor - 1), decoded, whitespace)) {
                    result += decoded;
                    cursor = semicolon + 1;
                    continue;
                }
            }
        }
        result.push_back(value[cursor++]);
    }
    return result;
}

bool asciiWordByte(unsigned char value) {
    return std::isalnum(value) != 0 || value == '_' || value == '\'' || value == '-';
}

void extractTagAnchors(const std::string& tag,
                       std::uint64_t tagOffset,
                       std::map<std::string, std::uint64_t>& anchors) {
    std::size_t cursor = 0;
    while (cursor < tag.size()) {
        while (cursor < tag.size() && std::isalpha(static_cast<unsigned char>(tag[cursor])) == 0 &&
               tag[cursor] != '_') {
            ++cursor;
        }
        const std::size_t nameStart = cursor;
        while (cursor < tag.size() &&
               (std::isalnum(static_cast<unsigned char>(tag[cursor])) != 0 || tag[cursor] == '_' ||
                tag[cursor] == '-' || tag[cursor] == ':')) {
            ++cursor;
        }
        const std::string name = epub::lowercaseAscii(tag.substr(nameStart, cursor - nameStart));
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if ((name != "id" && name != "name") || cursor == tag.size() || tag[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor])) != 0) {
            ++cursor;
        }
        if (cursor == tag.size() || (tag[cursor] != '\'' && tag[cursor] != '"')) {
            continue;
        }
        const char quote = tag[cursor++];
        const std::size_t valueEnd = tag.find(quote, cursor);
        if (valueEnd == std::string::npos) {
            continue;
        }
        // Link targets use decoded XML values, while native positions refer
        // to bytes in the serialized fragment.
        anchors.emplace(decodeAttributeEntities(tag.substr(cursor, valueEnd - cursor)), tagOffset);
        cursor = valueEnd + 1;
    }
}

void extractAnchors(const std::string& html, std::map<std::string, std::uint64_t>& anchors) {
    std::size_t cursor = 0;
    while (cursor < html.size()) {
        const std::size_t start = html.find('<', cursor);
        if (start == std::string::npos) {
            break;
        }
        const std::size_t end = html.find('>', start + 1);
        if (end == std::string::npos) {
            break;
        }
        extractTagAnchors(html.substr(start, end - start + 1), start, anchors);
        cursor = end + 1;
    }
}

std::size_t decodeTextEntity(const std::string& html,
                             std::size_t cursor,
                             std::string& decoded,
                             bool& whitespace) {
    const std::size_t maxEntityLength = 16;
    const std::size_t semicolon = html.find(';', cursor + 1);
    if (semicolon == std::string::npos || semicolon - cursor > maxEntityLength ||
        !decodeEntity(html.substr(cursor + 1, semicolon - cursor - 1), decoded, whitespace)) {
        return std::string::npos;
    }
    return semicolon + 1;
}

Word readWord(const std::string& html, std::size_t& cursor) {
    Word word;
    word.position = cursor;
    while (cursor < html.size() && html[cursor] != '<') {
        const unsigned char character = static_cast<unsigned char>(html[cursor]);
        if (std::isspace(character) != 0) {
            break;
        }
        if (character == '&') {
            std::string decoded;
            bool whitespace = false;
            const std::size_t entityEnd = decodeTextEntity(html, cursor, decoded, whitespace);
            if (entityEnd != std::string::npos) {
                if (whitespace) {
                    break;
                }
                word.text += decoded;
                cursor = entityEnd;
                continue;
            }
        }
        const unsigned char firstUtf8Byte = 0x80;
        if (character < firstUtf8Byte && !asciiWordByte(character)) {
            break;
        }
        word.text.push_back(static_cast<char>(character));
        ++cursor;
    }
    word.endPosition = cursor;
    return word;
}

void extractText(ContentDocument& result) {
    const std::string& html = result.fragment;
    bool pendingSpace = false;
    std::size_t cursor = 0;
    while (cursor < html.size()) {
        if (html[cursor] == '<') {
            const std::size_t end = html.find('>', cursor + 1);
            if (end == std::string::npos) {
                break;
            }
            pendingSpace = !result.plainText.empty();
            cursor = end + 1;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(html[cursor])) != 0) {
            pendingSpace = !result.plainText.empty();
            ++cursor;
            continue;
        }

        if (html[cursor] == '&') {
            std::string decoded;
            bool whitespace = false;
            const std::size_t entityEnd = decodeTextEntity(html, cursor, decoded, whitespace);
            if (entityEnd != std::string::npos && whitespace) {
                pendingSpace = !result.plainText.empty();
                cursor = entityEnd;
                continue;
            }
        }

        Word word = readWord(html, cursor);
        if (!word.text.empty()) {
            word.leadingSpace = pendingSpace;
            if (pendingSpace && !result.plainText.empty()) {
                result.plainText.push_back(' ');
            }
            pendingSpace = false;
            result.plainText += word.text;
            result.words.push_back(std::move(word));
            continue;
        }

        if (pendingSpace && !result.plainText.empty()) {
            result.plainText.push_back(' ');
            pendingSpace = false;
        }
        result.plainText.push_back(html[cursor]);
        ++cursor;
    }
}

} // namespace
void indexDocumentText(ContentDocument& document) {
    document.anchorOffsets.clear();
    document.plainText.clear();
    document.words.clear();
    extractAnchors(document.fragment, document.anchorOffsets);
    extractText(document);
}
} // namespace reader

} // namespace kindle_epub
