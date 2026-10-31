#include "epub/error.hpp"
#include "epub/identity.hpp"
#include "epub/path.hpp"
#include "providers.hpp"
#include "support.hpp"

#include <algorithm>
#include <climits>
#include <memory>
#include <string>
#include <vector>

namespace kindle_epub {
namespace {
using Mobi8SDK::BIG_COVER_DOES_NOT_EXIST;
using Mobi8SDK::EXTRA_HEADER_TYPE_NOT_FOUND;
using Mobi8SDK::GUIDE_ITEM_NOT_FOUND;
using Mobi8SDK::IBookMetadata;
using Mobi8SDK::IGuideItem;
using Mobi8SDK::IMetadataProvider;
using Mobi8SDK::IResource;
using Mobi8SDK::ManagedPtr;
using Mobi8SDK::OPERATION_NOT_SUPPORTED;
using Mobi8SDK::RESOURCE_NOT_AVAILABLE;
using Mobi8SDK::Result;
using Mobi8SDK::SUCCESS;

unsigned long mobiLanguage(const std::string& language) {
    const std::string lower = epub::lowercaseAscii(language);
    const std::size_t separator = lower.find_first_of("-_");
    const std::string primary = lower.substr(0, separator);
    struct LanguageCode {
        const char* tag;
        unsigned long code;
    };
    static const LanguageCode languages[] = {
        {"ar", 1},  {"bg", 2},  {"ca", 3},  {"zh", 4},  {"cs", 5},  {"da", 6},  {"de", 7},
        {"el", 8},  {"en", 9},  {"es", 10}, {"fi", 11}, {"fr", 12}, {"he", 13}, {"hu", 14},
        {"is", 15}, {"it", 16}, {"ja", 17}, {"ko", 18}, {"nl", 19}, {"no", 20}, {"pl", 21},
        {"pt", 22}, {"ro", 24}, {"ru", 25}, {"hr", 26}, {"sk", 27}, {"sq", 28}, {"sv", 29},
        {"th", 30}, {"tr", 31}, {"id", 33}, {"uk", 34}, {"sl", 36}, {"et", 37}, {"lv", 38},
        {"lt", 39}, {"vi", 42},
    };
    for (const auto& languageCode : languages) {
        if (primary == languageCode.tag) {
            return languageCode.code;
        }
    }
    return 0;
}

bool hasEmbeddedFonts(const epub::Package& package) {
    return std::any_of(package.manifest().begin(),
                       package.manifest().end(),
                       [](const std::pair<const std::string, epub::ManifestItem>& item) {
                           const std::string& type = item.second.mediaType;
                           return type == "application/vnd.ms-opentype" || type == "font/ttf" ||
                                  type == "font/otf" || type == "font/woff" || type == "font/woff2";
                       });
}

class CoverGuideItem final : public IGuideItem {
public:
    explicit CoverGuideItem(const std::string& resourceId) : resourceId_(resourceId) {}

    const unsigned char* getTitle() const override {
        return reinterpret_cast<const unsigned char*>("Cover");
    }
    const unsigned char* getType() const override {
        return reinterpret_cast<const unsigned char*>("cover");
    }
    const unsigned char* getResourceId() const override {
        return bytes(resourceId_);
    }

private:
    std::string resourceId_;
};

class EpubBookMetadata final : public IBookMetadata {
public:
    explicit EpubBookMetadata(const std::shared_ptr<const reader::Book>& book)
        : book_(book), metadata_(book->package().metadata()),
          guid_(epub::stableBookId(metadata_, book->archive().filename())) {
        if (metadata_.title.empty()) {
            metadata_.title = "Untitled EPUB";
        }
    }

    const unsigned char* getTitle() const override {
        return bytes(metadata_.title);
    }
    unsigned long getBookSize() const override {
        return nativePosition(book_->size());
    }
    Result getAuthors(std::vector<std::string>& output) const override {
        output = metadata_.creators;
        return SUCCESS;
    }
    const unsigned char* getPublicationDate() const override {
        return bytes(metadata_.publicationDate);
    }
    const unsigned char* getRights() const override {
        return bytes(metadata_.rights);
    }
    const unsigned char* getASIN() const override {
        return reinterpret_cast<const unsigned char*>("");
    }
    const unsigned char* getCdeKey() const override {
        return bytes(guid_);
    }
    const unsigned char* getCdeType() const override {
        return reinterpret_cast<const unsigned char*>("PDOC");
    }
    const unsigned char* getPublisher() const override {
        return bytes(metadata_.publisher);
    }
    const unsigned char* getGUID() const override {
        return bytes(guid_);
    }
    const unsigned char* getBookDescription() const override {
        return bytes(metadata_.description);
    }
    bool isSample() const override {
        return false;
    }
    unsigned long getClippingLimit() const override {
        return 0;
    }
    bool isPublisherLimit() const override {
        return false;
    }
    unsigned long getContentLanguage() const override {
        return mobiLanguage(metadata_.language);
    }
    bool isTTSMetadataPresent() const override {
        return true;
    }
    bool isTTSAllowed() const override {
        return true;
    }
    const unsigned char* getWatermark() const override {
        return reinterpret_cast<const unsigned char*>("");
    }
    bool hasDRM() const override {
        return false;
    }
    bool isTSC() const override {
        return false;
    }
    bool isLocationsMatch() const override {
        return true;
    }
    unsigned long getSampleStartLocation() const override {
        return 0;
    }
    unsigned long getSampleEndLocation() const override {
        return 0;
    }
    unsigned long getVersion() const override {
        return ULONG_MAX;
    }
    unsigned long getStartReadingLocation() const override {
        return 0;
    }
    unsigned long getCreatorSoftware() const override {
        return 0;
    }
    unsigned long getCreatorMajorVersion() const override {
        return 0;
    }
    unsigned long getCreatorMinorVersion() const override {
        return 0;
    }
    unsigned long getCreatorBuildNumber() const override {
        return 0;
    }
    unsigned long getContentType() const override {
        return 2;
    }
    bool hasSpecialFeature(unsigned long feature) const override {
        constexpr unsigned long embeddedFontsFeature = 12;
        return feature == embeddedFontsFeature && hasEmbeddedFonts(book_->package());
    }

private:
    std::shared_ptr<const reader::Book> book_;
    epub::Metadata metadata_;
    std::string guid_;
};

class EpubMetadataProvider final : public IMetadataProvider {
public:
    explicit EpubMetadataProvider(const std::shared_ptr<const reader::Book>& book)
        : book_(book), coverId_(book->resourceIdForPath(book->package().coverPath())) {}

    Result getBookMetadata(ManagedPtr<IBookMetadata>& output) override {
        output.reset(new EpubBookMetadata(book_));
        return SUCCESS;
    }
    Result getExtraHeaderValueAsString(std::string& output, unsigned long key) override {
        output.clear();
        constexpr unsigned long writingModeHeader = 525;
        constexpr unsigned long embeddedFontsHeader = 528;
        if (key == writingModeHeader) {
            output = book_->package().pageProgressionDirection() == "rtl" ? "horizontal-rl"
                                                                          : "horizontal-lr";
            return SUCCESS;
        }
        if (key == embeddedFontsHeader) {
            output = hasEmbeddedFonts(book_->package()) ? "true" : "false";
            return SUCCESS;
        }
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getExtraHeaderValueAsUINT32(unsigned long& output, unsigned long /*key*/) override {
        output = 0;
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getExtraHeaderStringValueList(std::vector<std::string>& output,
                                         unsigned long /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getExtraHeaderValueAsResource(ManagedPtr<IResource>& output,
                                         unsigned long /*key*/) override {
        output.reset();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getExtraHeaderUINT32ValueList(std::vector<unsigned long>& output,
                                         unsigned long /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    bool hasBigCoverArt() override {
        return !coverId_.empty();
    }
    Result getBigCoverArt(ManagedPtr<IResource>& output) override {
        return cover(output);
    }
    Result getBigCoverArtResourceIdOnly(std::string& output) override {
        output = coverId_;
        return coverId_.empty() ? BIG_COVER_DOES_NOT_EXIST : SUCCESS;
    }
    Result getCoverArt(ManagedPtr<IResource>& output) override {
        return cover(output);
    }
    Result getGuideItems(std::vector<ManagedPtr<IGuideItem>>& output) override {
        output.clear();
        if (!coverId_.empty()) {
            output.push_back(ManagedPtr<IGuideItem>(new CoverGuideItem(coverId_)));
        }
        return SUCCESS;
    }
    Result getGuideItem(ManagedPtr<IGuideItem>& output, const unsigned char* type) override {
        if (epub::lowercaseAscii(valueOf(type)) == "cover" && !coverId_.empty()) {
            output.reset(new CoverGuideItem(coverId_));
            return SUCCESS;
        }
        output.reset();
        return GUIDE_ITEM_NOT_FOUND;
    }
    // The firmware interface fixes the order of these output parameters.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    Result getMetadataResource(ManagedPtr<IResource>& output,
                               unsigned long& offset,
                               unsigned long& length) override {
        output.reset();
        offset = 0;
        length = 0;
        return OPERATION_NOT_SUPPORTED;
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)
    Result getMetadataValueAsString(std::string& output, unsigned long /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataValueAsString(std::string& output, const unsigned char* key) override {
        const std::string name = epub::lowercaseAscii(valueOf(key));
        const epub::Metadata& metadata = book_->package().metadata();
        if (name == "title") {
            output = metadata.title;
        } else if (name == "language") {
            output = metadata.language;
        } else if (name == "identifier") {
            output = metadata.identifier;
        } else if (name == "publisher") {
            output = metadata.publisher;
        } else if (name == "description") {
            output = metadata.description;
        } else if (name == "date") {
            output = metadata.publicationDate;
        } else if (name == "rights") {
            output = metadata.rights;
        } else {
            output.clear();
            return EXTRA_HEADER_TYPE_NOT_FOUND;
        }
        return SUCCESS;
    }
    Result getMetadataValueAsUINT32(unsigned long& output, unsigned long /*key*/) override {
        output = 0;
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataValueAsUINT32(unsigned long& output, const unsigned char* /*key*/) override {
        output = 0;
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataStringValueList(std::vector<std::string>& output,
                                      unsigned long /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataStringValueList(std::vector<std::string>& output,
                                      const unsigned char* key) override {
        output.clear();
        if (epub::lowercaseAscii(valueOf(key)) == "creator") {
            output = book_->package().metadata().creators;
            return SUCCESS;
        }
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataUINT32ValueList(std::vector<unsigned long>& output,
                                      unsigned long /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataUINT32ValueList(std::vector<unsigned long>& output,
                                      const unsigned char* /*key*/) override {
        output.clear();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataValueAsResource(ManagedPtr<IResource>& output,
                                      unsigned long /*key*/) override {
        output.reset();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }
    Result getMetadataValueAsResource(ManagedPtr<IResource>& output,
                                      const unsigned char* key) override {
        if (epub::lowercaseAscii(valueOf(key)) == "cover") {
            return cover(output);
        }
        output.reset();
        return EXTRA_HEADER_TYPE_NOT_FOUND;
    }

private:
    Result cover(ManagedPtr<IResource>& output) {
        if (coverId_.empty()) {
            output.reset();
            return BIG_COVER_DOES_NOT_EXIST;
        }
        try {
            output = makeResource(book_->readResource(coverId_), coverId_);
            return SUCCESS;
        } catch (const epub::Error&) {
            output.reset();
            return RESOURCE_NOT_AVAILABLE;
        }
    }

    std::shared_ptr<const reader::Book> book_;
    std::string coverId_;
};

} // namespace
Mobi8SDK::ManagedPtr<Mobi8SDK::IMetadataProvider>
makeMetadataProvider(const std::shared_ptr<const reader::Book>& book) {
    return Mobi8SDK::ManagedPtr<Mobi8SDK::IMetadataProvider>(new EpubMetadataProvider(book));
}
} // namespace kindle_epub
