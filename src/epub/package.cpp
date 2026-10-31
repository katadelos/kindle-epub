#include "epub/package.hpp"

#include "epub/xml.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include "epub/error.hpp"
#include "epub/path.hpp"

namespace epub {
namespace {

std::set<std::string> tokenSet(const std::string& value) {
    std::set<std::string> result;
    std::istringstream input(value);
    std::string token;
    while (input >> token) {
        result.insert(lowercaseAscii(token));
    }
    return result;
}

std::string resolvePackagePath(const std::string& packagePath, const std::string& href) {
    const Reference reference = resolveReference(packagePath, href);
    if (reference.isExternal() || reference.path().empty()) {
        throw Error(ErrorCode::InvalidPackage, "manifest item has a non-local href: " + href);
    }
    return reference.path();
}

std::string firstMetadataText(xmlNodePtr metadata, const char* name) {
    const auto nodes = xml::findElements(metadata == nullptr ? nullptr : metadata->children, name);
    for (auto* node : nodes) {
        const std::string value = trimAscii(xml::content(node));
        if (!value.empty()) {
            return value;
        }
    }
    return std::string();
}

std::vector<std::string> metadataTexts(xmlNodePtr metadata, const char* name) {
    std::vector<std::string> result;
    const auto nodes = xml::findElements(metadata == nullptr ? nullptr : metadata->children, name);
    for (auto* node : nodes) {
        const std::string value = trimAscii(xml::content(node));
        if (!value.empty()) {
            result.push_back(value);
        }
    }
    return result;
}

std::string epubType(xmlNodePtr node) {
    for (xmlAttrPtr attr = node == nullptr ? nullptr : node->properties; attr != nullptr;
         attr = attr->next) {
        const std::string attrName =
            attr->name == nullptr ? std::string()
                                  : lowercaseAscii(reinterpret_cast<const char*>(attr->name));
        if (attrName != "type" && attrName != "epub:type") {
            continue;
        }
        const bool epubNamespace = attr->ns != nullptr && attr->ns->href != nullptr &&
                                   std::string(reinterpret_cast<const char*>(attr->ns->href)) ==
                                       "http://www.idpf.org/2007/ops";
        const bool epubPrefix =
            attr->ns != nullptr && attr->ns->prefix != nullptr &&
            lowercaseAscii(reinterpret_cast<const char*>(attr->ns->prefix)) == "epub";
        if (!epubNamespace && !epubPrefix && attrName != "epub:type") {
            continue;
        }
        return xml::attributeValue(attr);
    }
    return std::string();
}

NavigationPoint parseNavListItem(xmlNodePtr item, const std::string& navPath) {
    NavigationPoint point;
    xmlNodePtr link = nullptr;
    for (xmlNodePtr child = item->children; child != nullptr; child = child->next) {
        if (xml::isElement(child, "a") || xml::isElement(child, "span")) {
            link = child;
            break;
        }
    }
    if (link == nullptr) {
        return point;
    }
    point.label = trimAscii(xml::content(link));
    const std::string href = trimAscii(xml::attribute(link, "href"));
    if (!href.empty()) {
        const Reference reference = resolveReference(navPath, href);
        if (!reference.isExternal()) {
            point.path = reference.path();
            point.fragment = reference.fragment();
        }
    }

    for (xmlNodePtr child = item->children; child != nullptr; child = child->next) {
        if (!xml::isElement(child, "ol")) {
            continue;
        }
        for (xmlNodePtr li = child->children; li != nullptr; li = li->next) {
            if (xml::isElement(li, "li")) {
                NavigationPoint nested = parseNavListItem(li, navPath);
                if (!nested.label.empty() || !nested.path.empty()) {
                    point.children.push_back(std::move(nested));
                }
            }
        }
    }
    return point;
}

std::vector<NavigationPoint> parseEpub3Navigation(const Archive& archive,
                                                  const ManifestItem& navItem) {
    auto document = xml::parse(archive.readText(navItem.path),
                               navItem.path,
                               xml::ParseMode::XmlOrHtml,
                               ErrorCode::InvalidPackage);
    const auto navs = xml::findElements(xmlDocGetRootElement(document.get()), "nav");
    xmlNodePtr toc = nullptr;
    for (auto* nav : navs) {
        const std::set<std::string> types = tokenSet(epubType(nav));
        if (types.count("toc") != 0) {
            toc = nav;
            break;
        }
    }
    if (toc == nullptr) {
        return std::vector<NavigationPoint>();
    }

    xmlNodePtr list = xml::findElement(toc->children, "ol");
    std::vector<NavigationPoint> result;
    for (xmlNodePtr item = list == nullptr ? nullptr : list->children; item != nullptr;
         item = item->next) {
        if (!xml::isElement(item, "li")) {
            continue;
        }
        NavigationPoint point = parseNavListItem(item, navItem.path);
        if (!point.label.empty() || !point.path.empty()) {
            result.push_back(std::move(point));
        }
    }
    return result;
}

NavigationPoint parseNcxPoint(xmlNodePtr node, const std::string& ncxPath) {
    NavigationPoint point;
    xmlNodePtr label = xml::findElement(node->children, "text");
    point.label = trimAscii(xml::content(label));
    xmlNodePtr target = xml::findElement(node->children, "content");
    const std::string src = trimAscii(xml::attribute(target, "src"));
    if (!src.empty()) {
        const Reference reference = resolveReference(ncxPath, src);
        if (!reference.isExternal()) {
            point.path = reference.path();
            point.fragment = reference.fragment();
        }
    }
    for (xmlNodePtr child = node->children; child != nullptr; child = child->next) {
        if (xml::isElement(child, "navpoint")) {
            point.children.push_back(parseNcxPoint(child, ncxPath));
        }
    }
    return point;
}

std::vector<NavigationPoint> parseNcxNavigation(const Archive& archive,
                                                const ManifestItem& ncxItem) {
    auto document = xml::parse(archive.readText(ncxItem.path),
                               ncxItem.path,
                               xml::ParseMode::Xml,
                               ErrorCode::InvalidPackage);
    xmlNodePtr map = xml::findElement(xmlDocGetRootElement(document.get()), "navmap");
    std::vector<NavigationPoint> result;
    for (xmlNodePtr node = map == nullptr ? nullptr : map->children; node != nullptr;
         node = node->next) {
        if (xml::isElement(node, "navpoint")) {
            result.push_back(parseNcxPoint(node, ncxItem.path));
        }
    }
    return result;
}

std::string findPackagePath(const Archive& archive) {
    if (!archive.contains("META-INF/container.xml")) {
        throw Error(ErrorCode::MissingContainer, "EPUB is missing META-INF/container.xml");
    }

    auto container = xml::parse(archive.readText("META-INF/container.xml"),
                                "META-INF/container.xml",
                                xml::ParseMode::Xml,
                                ErrorCode::InvalidContainer);
    const auto rootfiles = xml::findElements(xmlDocGetRootElement(container.get()), "rootfile");
    std::string packagePath;
    for (auto* rootfile : rootfiles) {
        const std::string candidate = trimAscii(xml::attribute(rootfile, "full-path"));
        const std::string mediaType =
            lowercaseAscii(trimAscii(xml::attribute(rootfile, "media-type")));
        if (candidate.empty()) {
            continue;
        }
        if (packagePath.empty() || mediaType == "application/oebps-package+xml") {
            packagePath = canonicalArchivePath(percentDecodePath(candidate));
        }
        if (mediaType == "application/oebps-package+xml") {
            break;
        }
    }
    if (packagePath.empty()) {
        throw Error(ErrorCode::InvalidContainer, "EPUB container has no package rootfile");
    }
    if (!archive.contains(packagePath)) {
        throw Error(ErrorCode::MissingPackage,
                    "EPUB package document does not exist: " + packagePath);
    }

    return packagePath;
}

std::map<std::string, ManifestItem>
parseManifest(const Archive& archive, xmlNodePtr manifest, const std::string& packagePath) {
    if (manifest == nullptr) {
        throw Error(ErrorCode::InvalidPackage, "OPF has no manifest");
    }
    std::map<std::string, ManifestItem> result;
    for (xmlNodePtr node = manifest->children; node != nullptr; node = node->next) {
        if (!xml::isElement(node, "item")) {
            continue;
        }
        ManifestItem item;
        item.id = trimAscii(xml::attribute(node, "id"));
        item.href = trimAscii(xml::attribute(node, "href"));
        item.mediaType = lowercaseAscii(trimAscii(xml::attribute(node, "media-type")));
        item.properties = tokenSet(xml::attribute(node, "properties"));
        item.fallback = trimAscii(xml::attribute(node, "fallback"));
        if (item.id.empty() || item.href.empty() || item.mediaType.empty()) {
            throw Error(ErrorCode::InvalidPackage,
                        "OPF manifest item is missing id, href, or media-type");
        }
        item.path = resolvePackagePath(packagePath, item.href);
        if (!archive.contains(item.path)) {
            throw Error(ErrorCode::MissingResource,
                        "OPF manifest resource does not exist: " + item.path);
        }
        if (!result.insert(std::make_pair(item.id, item)).second) {
            throw Error(ErrorCode::InvalidPackage,
                        "OPF manifest contains duplicate id: " + item.id);
        }
    }

    return result;
}

std::vector<SpineItem> parseSpine(xmlNodePtr spine,
                                  const std::map<std::string, ManifestItem>& manifest) {
    std::vector<SpineItem> result;
    for (xmlNodePtr node = spine->children; node != nullptr; node = node->next) {
        if (!xml::isElement(node, "itemref")) {
            continue;
        }
        SpineItem item;
        item.idref = trimAscii(xml::attribute(node, "idref"));
        const std::map<std::string, ManifestItem>::const_iterator manifestItem =
            manifest.find(item.idref);
        if (manifestItem == manifest.end()) {
            throw Error(ErrorCode::InvalidPackage,
                        "OPF spine references unknown manifest id: " + item.idref);
        }
        item.path = manifestItem->second.path;
        item.mediaType = manifestItem->second.mediaType;
        if (item.mediaType != "application/xhtml+xml" && item.mediaType != "text/html" &&
            item.mediaType != "application/xml" && item.mediaType != "text/xml") {
            throw Error(ErrorCode::InvalidDocument,
                        "unsupported EPUB spine media type: " + item.mediaType);
        }
        item.linear = lowercaseAscii(trimAscii(xml::attribute(node, "linear"))) != "no";
        item.properties = tokenSet(xml::attribute(node, "properties"));
        result.push_back(std::move(item));
    }
    if (result.empty()) {
        throw Error(ErrorCode::MissingSpine, "OPF spine is empty");
    }

    return result;
}

} // namespace

Package Package::parse(const Archive& archive) {
    const std::string packagePath = findPackagePath(archive);

    Package package;
    package.packagePath_ = packagePath;

    auto opf = xml::parse(
        archive.readText(packagePath), packagePath, xml::ParseMode::Xml, ErrorCode::InvalidPackage);
    xmlNodePtr root = xmlDocGetRootElement(opf.get());
    if (!xml::isElement(root, "package")) {
        throw Error(ErrorCode::InvalidPackage, "EPUB rootfile is not an OPF package document");
    }
    package.version_ = trimAscii(xml::attribute(root, "version"));

    xmlNodePtr metadata = xml::findElement(root->children, "metadata");
    package.metadata_.title = firstMetadataText(metadata, "title");
    package.metadata_.creators = metadataTexts(metadata, "creator");
    package.metadata_.language = firstMetadataText(metadata, "language");
    package.metadata_.identifier = firstMetadataText(metadata, "identifier");
    package.metadata_.publisher = firstMetadataText(metadata, "publisher");
    package.metadata_.description = firstMetadataText(metadata, "description");
    package.metadata_.publicationDate = firstMetadataText(metadata, "date");
    package.metadata_.rights = firstMetadataText(metadata, "rights");

    const auto metaNodes =
        xml::findElements(metadata == nullptr ? nullptr : metadata->children, "meta");
    std::string epub2CoverId;
    for (auto* metaNode : metaNodes) {
        const std::string property =
            lowercaseAscii(trimAscii(xml::attribute(metaNode, "property")));
        if (property == "dcterms:modified") {
            package.metadata_.modified = trimAscii(xml::content(metaNode));
        }
        if (lowercaseAscii(trimAscii(xml::attribute(metaNode, "name"))) == "cover") {
            epub2CoverId = trimAscii(xml::attribute(metaNode, "content"));
        }
    }

    package.manifest_ =
        parseManifest(archive, xml::findElement(root->children, "manifest"), packagePath);

    xmlNodePtr spine = xml::findElement(root->children, "spine");
    if (spine == nullptr) {
        throw Error(ErrorCode::MissingSpine, "OPF has no spine");
    }
    package.pageProgressionDirection_ =
        lowercaseAscii(trimAscii(xml::attribute(spine, "page-progression-direction")));
    if (package.pageProgressionDirection_.empty()) {
        package.pageProgressionDirection_ = "default";
    }
    const std::string ncxId = trimAscii(xml::attribute(spine, "toc"));
    package.spine_ = parseSpine(spine, package.manifest_);

    const ManifestItem* navItem = nullptr;
    for (const auto& entry : package.manifest_) {
        const ManifestItem& item = entry.second;
        if (item.properties.count("cover-image") != 0) {
            package.coverPath_ = item.path;
        }
        if (item.properties.count("nav") != 0) {
            navItem = &item;
        }
    }
    if (package.coverPath_.empty() && !epub2CoverId.empty()) {
        const std::map<std::string, ManifestItem>::const_iterator cover =
            package.manifest_.find(epub2CoverId);
        if (cover != package.manifest_.end()) {
            package.coverPath_ = cover->second.path;
        }
    }

    if (navItem != nullptr) {
        package.navigation_ = parseEpub3Navigation(archive, *navItem);
    }
    if (package.navigation_.empty() && !ncxId.empty()) {
        const std::map<std::string, ManifestItem>::const_iterator ncx =
            package.manifest_.find(ncxId);
        if (ncx != package.manifest_.end()) {
            package.navigation_ = parseNcxNavigation(archive, ncx->second);
        }
    }
    return package;
}

} // namespace epub
