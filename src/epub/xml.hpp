#ifndef KINDLE_EPUB_XML_HPP
#define KINDLE_EPUB_XML_HPP

#include "epub/error.hpp"

#include <libxml/tree.h>
#include <memory>
#include <string>
#include <vector>

namespace epub {
namespace xml {

// Preserve the parser API's enum representation.
// NOLINTNEXTLINE(performance-enum-size)
enum class ParseMode : int { Xml, XmlOrHtml };
using Document = std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>;

Document
parse(const std::string& bytes, const std::string& name, ParseMode mode, ErrorCode errorCode);
std::string nodeName(xmlNodePtr node);
bool isElement(xmlNodePtr node, const char* name);
xmlNodePtr findElement(xmlNodePtr node, const char* name);
std::vector<xmlNodePtr> findElements(xmlNodePtr node, const char* name);
std::string attribute(xmlNodePtr node, const char* name);
std::string attributeValue(xmlAttrPtr attribute);
std::string content(xmlNodePtr node);

} // namespace xml
} // namespace epub
#endif
