#include "epub/error.hpp"
#include "providers.hpp"
#include "support.hpp"

#include <climits>
#include <string>
#include <vector>

namespace kindle_epub {
namespace {
using Mobi8SDK::IResource;
using Mobi8SDK::ManagedPtr;

std::string requestedMimeType(const std::string& resourceId) {
    const std::size_t query = resourceId.find('?');
    if (query == std::string::npos) {
        return std::string();
    }
    const std::string prefix = "mime=";
    std::size_t start = query + 1;
    while (start <= resourceId.size()) {
        const std::size_t end = resourceId.find('&', start);
        const std::size_t length =
            end == std::string::npos ? resourceId.size() - start : end - start;
        if (length >= prefix.size() && resourceId.compare(start, prefix.size(), prefix) == 0) {
            return resourceId.substr(start + prefix.size(), length - prefix.size());
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return std::string();
}

class EpubResource final : public IResource {
public:
    EpubResource(const reader::ResourceData& resource, const std::string& requestedResourceId)
        : content_(resource.bytes), resourceId_(requestedResourceId),
          mimeType_(requestedMimeType(requestedResourceId)) {
        if (content_.size() > ULONG_MAX) {
            throw epub::Error(epub::ErrorCode::LimitExceeded,
                              "EPUB resource exceeds the native reader size limit");
        }
    }

    const unsigned char* getContent() const override {
        return content_.empty() ? nullptr : content_.data();
    }
    unsigned long getContentLength() const override {
        return nativePosition(content_.size());
    }
    const unsigned char* getMimeType() const override {
        return mimeType_.empty() ? nullptr : bytes(mimeType_);
    }
    const unsigned char* getResourceId() const override {
        return bytes(resourceId_);
    }

private:
    std::vector<unsigned char> content_;
    std::string resourceId_;
    std::string mimeType_;
};

} // namespace
Mobi8SDK::ManagedPtr<Mobi8SDK::IResource> makeResource(const reader::ResourceData& resource,
                                                       const std::string& requestedId) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::IResource>(new EpubResource(resource, requestedId));
}
} // namespace kindle_epub
