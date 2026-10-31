#include "reader/document.hpp"
#include "reader/css.hpp"
#include "reader/text.hpp"

#include "epub/xml.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

#include "epub/error.hpp"
#include "epub/path.hpp"

namespace kindle_epub {
namespace reader {
namespace {

void setAttribute(xmlNodePtr node, xmlAttrPtr attr, const std::string& value) {
    if (attr->ns != nullptr) {
        xmlSetNsProp(node, attr->ns, attr->name, reinterpret_cast<const xmlChar*>(value.c_str()));
    } else {
        xmlSetProp(node, attr->name, reinterpret_cast<const xmlChar*>(value.c_str()));
    }
}

std::string serializeChildren(xmlDocPtr document, xmlNodePtr parent) {
    if (parent == nullptr || parent->children == nullptr) {
        return {};
    }
    std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)> buffer(xmlBufferCreate(), &xmlBufferFree);
    if (buffer == nullptr) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "cannot allocate an XML serialization buffer");
    }
    for (xmlNodePtr child = parent->children; child != nullptr; child = child->next) {
        if (xmlNodeDump(buffer.get(), document, child, 0, 0) < 0) {
            throw epub::Error(epub::ErrorCode::InvalidDocument,
                              "cannot serialize EPUB content document");
        }
    }
    return {reinterpret_cast<const char*>(xmlBufferContent(buffer.get())),
            static_cast<std::size_t>(xmlBufferLength(buffer.get()))};
}

std::string escapeAttribute(const std::string& value) {
    std::string result;
    for (const char character : value) {
        switch (character) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\"':
            result += "&quot;";
            break;
        default:
            result.push_back(character);
            break;
        }
    }
    return result;
}

std::string allocateAid(std::uint64_t& nextAid) {
    if (nextAid == std::numeric_limits<std::uint64_t>::max()) {
        throw epub::Error(epub::ErrorCode::LimitExceeded,
                          "EPUB addressable element ID space is exhausted");
    }
    std::uint64_t value = nextAid++;
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";
    const std::size_t radix = sizeof(digits) - 1;
    std::string result;
    do {
        result.push_back(digits[value % radix]);
        value /= radix;
    } while (value != 0);
    std::reverse(result.begin(), result.end());
    return result;
}

bool isHtmlVoidElement(xmlNodePtr node) {
    static const char* const names[] = {"area",
                                        "base",
                                        "br",
                                        "col",
                                        "embed",
                                        "hr",
                                        "img",
                                        "input",
                                        "link",
                                        "meta",
                                        "param",
                                        "source",
                                        "track",
                                        "wbr"};
    const std::string name = epub::xml::nodeName(node);
    return std::any_of(std::begin(names), std::end(names), [&name](const char* candidate) {
        return name == candidate;
    });
}

void assignAidAttributes(xmlNodePtr node, std::uint64_t& nextAid) {
    for (xmlNodePtr current = node; current != nullptr; current = current->next) {
        // Stock KF8 does not make HTML void elements addressable and does not
        // consume an AID ordinal for them. Keeping them in the sequence shifts
        // every later AID and corrupts WebReader's cross-fragment page graph.
        if (current->type == XML_ELEMENT_NODE && !isHtmlVoidElement(current)) {
            const std::string aid = allocateAid(nextAid);
            xmlSetProp(current,
                       reinterpret_cast<const xmlChar*>("aid"),
                       reinterpret_cast<const xmlChar*>(aid.c_str()));
        }
        assignAidAttributes(current->children, nextAid);
    }
}

bool isUnsafeElement(xmlNodePtr node) {
    const std::string name = epub::xml::nodeName(node);
    if (name == "script" || name == "iframe" || name == "object" || name == "embed" ||
        name == "applet") {
        return true;
    }
    return name == "meta" &&
           epub::lowercaseAscii(epub::trimAscii(epub::xml::attribute(node, "http-equiv"))) ==
               "refresh";
}

bool isLinkAttribute(const std::string& element, const std::string& attribute) {
    return attribute == "href" && (element == "a" || element == "area");
}

bool isResourceAttribute(const std::string& element, const std::string& attribute) {
    if (attribute == "src" || attribute == "poster" || attribute == "data") {
        return true;
    }
    return attribute == "href" && (element == "link" || element == "image" || element == "use");
}

void rewriteLinkAttribute(xmlNodePtr node,
                          xmlAttrPtr attribute,
                          const std::string& documentPath,
                          const InternalLinkMapper& mapper) {
    try {
        const epub::Reference reference =
            epub::resolveReference(documentPath, epub::xml::attributeValue(attribute));
        if (!reference.isExternal()) {
            setAttribute(node, attribute, mapper(reference.path(), reference.fragment()));
        }
    } catch (const epub::Error&) {
        xmlRemoveProp(attribute);
    }
}

void rewriteResourceAttribute(xmlNodePtr node,
                              xmlAttrPtr attribute,
                              const std::string& documentPath,
                              const ResourceLinkMapper& mapper) {
    const std::string value = epub::xml::attributeValue(attribute);
    try {
        const epub::Reference reference = epub::resolveReference(documentPath, value);
        if (reference.isExternal()) {
            static const char dataScheme[] = "data:";
            if (epub::lowercaseAscii(value).compare(0, sizeof(dataScheme) - 1, dataScheme) != 0) {
                xmlRemoveProp(attribute);
            }
            return;
        }
        const std::string mapped = mapper(reference.path());
        if (mapped.empty()) {
            xmlRemoveProp(attribute);
        } else {
            setAttribute(node, attribute, mapped);
        }
    } catch (const epub::Error&) {
        xmlRemoveProp(attribute);
    }
}

void sanitizeAttributes(xmlNodePtr node,
                        const std::string& documentPath,
                        const InternalLinkMapper& internalLinkMapper,
                        const ResourceLinkMapper& resourceLinkMapper) {
    const std::string element = epub::xml::nodeName(node);
    for (xmlAttrPtr attribute = node->properties; attribute != nullptr;) {
        // Rewriting can remove an attribute, so save the next one first.
        xmlAttrPtr next = attribute->next;
        const std::string name =
            attribute->name == nullptr
                ? std::string()
                : epub::lowercaseAscii(reinterpret_cast<const char*>(attribute->name));
        const bool epubAttribute = attribute->ns != nullptr && attribute->ns->href != nullptr &&
                                   std::strcmp(reinterpret_cast<const char*>(attribute->ns->href),
                                               "http://www.idpf.org/2007/ops") == 0;
        const bool eventHandler = name.size() > 2 && name.compare(0, 2, "on") == 0;
        if (epubAttribute || eventHandler) {
            xmlRemoveProp(attribute);
        } else if (name == "style") {
            const std::string rewritten = rewritePaginationCss(rewriteCssUrls(
                epub::xml::attributeValue(attribute), documentPath, resourceLinkMapper));
            setAttribute(node, attribute, rewritten);
        } else if (isLinkAttribute(element, name)) {
            rewriteLinkAttribute(node, attribute, documentPath, internalLinkMapper);
        } else if (isResourceAttribute(element, name)) {
            rewriteResourceAttribute(node, attribute, documentPath, resourceLinkMapper);
        }
        attribute = next;
    }
}

void rewriteStyleElement(xmlNodePtr node,
                         const std::string& documentPath,
                         const ResourceLinkMapper& resourceLinkMapper) {
    for (xmlNodePtr child = node->children; child != nullptr; child = child->next) {
        if (child->type != XML_TEXT_NODE && child->type != XML_CDATA_SECTION_NODE) {
            continue;
        }
        const std::string source = child->content == nullptr
                                       ? std::string()
                                       : reinterpret_cast<const char*>(child->content);
        const std::string rewritten =
            rewritePaginationCss(rewriteCssUrls(source, documentPath, resourceLinkMapper));
        xmlNodeSetContent(child, reinterpret_cast<const xmlChar*>(rewritten.c_str()));
    }
}

void sanitizeAndRewrite(xmlNodePtr node,
                        const std::string& documentPath,
                        const InternalLinkMapper& internalLinkMapper,
                        const ResourceLinkMapper& resourceLinkMapper) {
    for (xmlNodePtr current = node; current != nullptr;) {
        xmlNodePtr next = current->next;
        if (current->type == XML_ELEMENT_NODE && isUnsafeElement(current)) {
            xmlUnlinkNode(current);
            xmlFreeNode(current);
            current = next;
            continue;
        }
        if (current->type == XML_ELEMENT_NODE) {
            sanitizeAttributes(current, documentPath, internalLinkMapper, resourceLinkMapper);
            if (epub::xml::isElement(current, "style")) {
                rewriteStyleElement(current, documentPath, resourceLinkMapper);
            }
        }
        sanitizeAndRewrite(current->children, documentPath, internalLinkMapper, resourceLinkMapper);
        current = next;
    }
}

} // namespace

// The existing API names the path and source separately without changing their string types.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ContentDocument parseContentDocument(const std::string& archivePath,
                                     const std::string& bytes,
                                     std::uint32_t index,
                                     std::uint64_t& nextAid,
                                     const InternalLinkMapper& internalLinkMapper,
                                     const ResourceLinkMapper& resourceLinkMapper) {
    const std::string canonicalPath = epub::canonicalArchivePath(archivePath);
    if (!internalLinkMapper || !resourceLinkMapper) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "content rewriting requires both link mappers");
    }
    auto document = epub::xml::parse(
        bytes, canonicalPath, epub::xml::ParseMode::XmlOrHtml, epub::ErrorCode::InvalidDocument);
    xmlNodePtr root = xmlDocGetRootElement(document.get());
    xmlNodePtr head = epub::xml::findElement(root, "head");
    xmlNodePtr body = epub::xml::findElement(root, "body");
    if (body == nullptr) {
        throw epub::Error(epub::ErrorCode::InvalidDocument,
                          "EPUB spine document has no body: " + archivePath);
    }

    sanitizeAndRewrite(root, archivePath, internalLinkMapper, resourceLinkMapper);

    ContentDocument result;
    result.path = canonicalPath;
    result.index = index;
    result.placeholderId = allocateAid(nextAid);
    xmlSetProp(body,
               reinterpret_cast<const xmlChar*>("aid"),
               reinterpret_cast<const xmlChar*>(result.placeholderId.c_str()));
    assignAidAttributes(body->children, nextAid);

    result.fragment = serializeChildren(document.get(), body);
    if (epub::trimAscii(result.fragment).empty()) {
        result.fragment = "<span/>";
    }

    std::ostringstream skeleton;
    skeleton << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             << "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>";
    if (head != nullptr) {
        skeleton << serializeChildren(document.get(), head);
    }
    skeleton << "</head><body aid=\"" << escapeAttribute(result.placeholderId) << "\"";
    const char* copiedAttributes[] = {"class", "style", "lang", "dir"};
    for (const char* attribute : copiedAttributes) {
        const std::string value = epub::xml::attribute(body, attribute);
        if (!value.empty()) {
            skeleton << ' ' << attribute << "=\"" << escapeAttribute(value) << "\"";
        }
    }
    skeleton << '>';
    result.skeleton = skeleton.str();
    result.fragmentInsertionOffset = result.skeleton.size();
    result.skeleton += "</body></html>";

    indexDocumentText(result);
    return result;
}

} // namespace reader

} // namespace kindle_epub
