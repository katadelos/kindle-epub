#ifndef KINDLE_EPUB_ERROR_HPP
#define KINDLE_EPUB_ERROR_HPP

#include <stdexcept>
#include <string>

namespace epub {

// Preserve the existing exception layout and enum representation.
// NOLINTNEXTLINE(performance-enum-size)
enum class ErrorCode : int {
    Io,
    InvalidArchive,
    InvalidPath,
    LimitExceeded,
    MissingContainer,
    InvalidContainer,
    MissingPackage,
    InvalidPackage,
    MissingSpine,
    UnsupportedEncryption,
    InvalidDocument,
    MissingResource
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}

    ErrorCode code() const {
        return code_;
    }

private:
    ErrorCode code_;
};

} // namespace epub

#endif
