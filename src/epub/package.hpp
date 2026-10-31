#ifndef KINDLE_EPUB_PACKAGE_HPP
#define KINDLE_EPUB_PACKAGE_HPP

#include <map>
#include <set>
#include <string>
#include <vector>

#include "epub/archive.hpp"

namespace epub {

struct Metadata {
    std::string title;
    std::vector<std::string> creators;
    std::string language;
    std::string identifier;
    std::string publisher;
    std::string description;
    std::string publicationDate;
    std::string rights;
    std::string modified;
};

struct ManifestItem {
    std::string id;
    std::string href;
    std::string path;
    std::string mediaType;
    std::set<std::string> properties;
    std::string fallback;
};

struct SpineItem {
    std::string idref;
    std::string path;
    std::string mediaType;
    bool linear = true;
    std::set<std::string> properties;
};

struct NavigationPoint {
    std::string label;
    std::string path;
    std::string fragment;
    std::vector<NavigationPoint> children;
};

class Package {
public:
    static Package parse(const Archive& archive);

    const std::string& packagePath() const {
        return packagePath_;
    }
    const std::string& version() const {
        return version_;
    }
    const Metadata& metadata() const {
        return metadata_;
    }
    const std::map<std::string, ManifestItem>& manifest() const {
        return manifest_;
    }
    const std::vector<SpineItem>& spine() const {
        return spine_;
    }
    const std::vector<NavigationPoint>& navigation() const {
        return navigation_;
    }
    const std::string& coverPath() const {
        return coverPath_;
    }
    const std::string& pageProgressionDirection() const {
        return pageProgressionDirection_;
    }

private:
    Package() = default;

    std::string packagePath_;
    std::string version_;
    Metadata metadata_;
    std::map<std::string, ManifestItem> manifest_;
    std::vector<SpineItem> spine_;
    std::vector<NavigationPoint> navigation_;
    std::string coverPath_;
    std::string pageProgressionDirection_;
};

} // namespace epub

#endif
