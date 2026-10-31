#ifndef KINDLE_EPUB_PATH_HPP
#define KINDLE_EPUB_PATH_HPP

#include <string>

namespace epub {

class Reference {
public:
    bool isExternal() const {
        return kind_ == Kind::External;
    }
    // For external references, path is the complete URI; local paths are canonical.
    const std::string& path() const {
        return path_;
    }
    const std::string& query() const {
        return query_;
    }
    const std::string& fragment() const {
        return fragment_;
    }

private:
    // Preserve Reference's object layout.
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Kind : int { Archive, External };
    // These URI components remain strings to preserve the established constructor contract.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    Reference(Kind kind,
              const std::string& path,
              const std::string& query,
              const std::string& fragment)
        : kind_(kind), path_(path), query_(query), fragment_(fragment) {}
    // NOLINTEND(bugprone-easily-swappable-parameters)
    friend Reference resolveReference(const std::string& baseDocument, const std::string& value);

    Kind kind_;
    std::string path_;
    std::string query_;
    std::string fragment_;
};

// Canonical archive paths never start with '/', contain '.', '..', a NUL,
// or a backslash component. Invalid or escaping paths throw epub::Error.
std::string canonicalArchivePath(const std::string& input);
std::string archiveDirectory(const std::string& path);
Reference resolveReference(const std::string& baseDocument, const std::string& value);
std::string percentDecodePath(const std::string& value);
std::string lowercaseAscii(const std::string& value);
std::string trimAscii(const std::string& value);

} // namespace epub

#endif
