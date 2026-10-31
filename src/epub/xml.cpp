#include "epub/xml.hpp"
#include "epub/path.hpp"

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xmlstring.h>
#include <limits>

namespace epub {
namespace xml {
namespace {

struct XmlStringDeleter {
    void operator()(xmlChar* value) const {
        xmlFree(value);
    }
};

using XmlString = std::unique_ptr<xmlChar, XmlStringDeleter>;

void collectElements(xmlNodePtr node, const char* name, std::vector<xmlNodePtr>& result) {
    for (xmlNodePtr current = node; current != nullptr; current = current->next) {
        if (isElement(current, name)) {
            result.push_back(current);
        }
        collectElements(current->children, name, result);
    }
}

} // namespace

Document
parse(const std::string& bytes, const std::string& name, ParseMode mode, ErrorCode errorCode) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw Error(ErrorCode::LimitExceeded, "XML document exceeds parser size limit: " + name);
    }
    if (bytes.empty()) {
        throw Error(errorCode, "XML document is empty: " + name);
    }
    const int length = static_cast<int>(bytes.size());
    const int xmlOptions = XML_PARSE_NONET | XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                           XML_PARSE_NOWARNING | XML_PARSE_COMPACT;
    Document document(xmlReadMemory(bytes.data(), length, name.c_str(), nullptr, xmlOptions),
                      &xmlFreeDoc);
    if (!document && mode == ParseMode::XmlOrHtml) {
        const int htmlOptions = HTML_PARSE_NONET | HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                HTML_PARSE_NOWARNING | HTML_PARSE_COMPACT;
        document.reset(htmlReadMemory(bytes.data(), length, name.c_str(), "UTF-8", htmlOptions));
    }
    if (!document) {
        throw Error(errorCode, "cannot parse XML document: " + name);
    }
    return document;
}

std::string nodeName(xmlNodePtr node) {
    if (node == nullptr || node->name == nullptr) {
        return {};
    }
    return lowercaseAscii(reinterpret_cast<const char*>(node->name));
}

bool isElement(xmlNodePtr node, const char* name) {
    return node != nullptr && node->type == XML_ELEMENT_NODE && node->name != nullptr &&
           xmlStrcasecmp(node->name, reinterpret_cast<const xmlChar*>(name)) == 0;
}

xmlNodePtr findElement(xmlNodePtr node, const char* name) {
    for (xmlNodePtr current = node; current != nullptr; current = current->next) {
        if (isElement(current, name)) {
            return current;
        }
        xmlNodePtr child = findElement(current->children, name);
        if (child != nullptr) {
            return child;
        }
    }
    return nullptr;
}

std::vector<xmlNodePtr> findElements(xmlNodePtr node, const char* name) {
    std::vector<xmlNodePtr> result;
    collectElements(node, name, result);
    return result;
}

std::string attributeValue(xmlAttrPtr attribute) {
    if (attribute == nullptr) {
        return {};
    }
    const XmlString value(xmlNodeListGetString(attribute->doc, attribute->children, 1));
    return value ? reinterpret_cast<const char*>(value.get()) : std::string();
}

std::string attribute(xmlNodePtr node, const char* name) {
    for (xmlAttrPtr attr = node == nullptr ? nullptr : node->properties; attr != nullptr;
         attr = attr->next) {
        if (attr->name != nullptr &&
            xmlStrcasecmp(attr->name, reinterpret_cast<const xmlChar*>(name)) == 0) {
            return attributeValue(attr);
        }
    }
    return {};
}

std::string content(xmlNodePtr node) {
    if (node == nullptr) {
        return {};
    }
    const XmlString value(xmlNodeGetContent(node));
    return value ? reinterpret_cast<const char*>(value.get()) : std::string();
}

} // namespace xml
} // namespace epub
