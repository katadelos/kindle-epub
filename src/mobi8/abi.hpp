#ifndef KINDLE_EPUB_MOBI8_ABI_HPP
#define KINDLE_EPUB_MOBI8_ABI_HPP

#include <cstdint>
#include <string>
#include <vector>

// This is a clean-room ABI declaration reconstructed from libMobi.so.1.0 in
// Kindle firmware 5.18.1.1.1. It intentionally contains only the public
// virtual boundary used by webreader and mobi8rendererjni. No Amazon header or
// implementation is included.
namespace Mobi8SDK {

using Result = std::uint32_t;

// Firmware uses a word-sized result code; narrowing the enum would change its ABI.
// NOLINTNEXTLINE(performance-enum-size)
enum ErrorCode {
    SUCCESS = 0,
    UNEXPECTED_ERROR = 1,
    INSTANTIATION_ERROR = 2,
    FRAGMENT_NOT_AVAILABLE = 4,
    FAILED_LOADING_FILE = 6,
    SKELETON_NOT_AVAILABLE = 9,
    LOOKUP_STRING_NOT_FOUND = 11,
    INVALID_INDEX = 15,
    OPERATION_NOT_SUPPORTED = 18,
    EXTRA_HEADER_TYPE_NOT_FOUND = 21,
    BIG_COVER_DOES_NOT_EXIST = 22,
    GUIDE_ITEM_NOT_FOUND = 23,
    INVALID_FRAGMENT = 25,
    INVALID_SKELETON_LINK = 26,
    RESOURCE_NOT_AVAILABLE = 27,
    RESOURCE_URL_NOT_VALID = 28,
    POSITION_OUT_OF_BOUND = 32,
    TOC_ITEM_NOT_FOUND = 33,
    NCX_TOC_NOT_AVAILABLE = 34,
    TOC_PARENT_NOT_FOUND = 36,
    TOC_FIRST_CHILD_NOT_FOUND = 37,
    TOC_LAST_CHILD_NOT_FOUND = 38,
    WORD_END = 42,
    TOC_NEXT_ENTRY_NOT_FOUND = 45,
    TOC_PREV_ENTRY_NOT_FOUND = 46,
    RESOURCE_ID_NOT_FOUND = 48,
    NCX_SECONDARY_TOC_NOT_AVAILABLE = 68
};

class ManagedObject {
public:
    ManagedObject() = default;
    virtual ~ManagedObject() = default;
    ManagedObject(const ManagedObject&) = delete;
    ManagedObject& operator=(const ManagedObject&) = delete;

    void retain() {
        __sync_add_and_fetch(&referenceCount_, 1U);
    }

    void release() {
        if (__sync_sub_and_fetch(&referenceCount_, 1U) == 0U) {
            delete this;
        }
    }

private:
    volatile std::uint32_t referenceCount_{0};
};

template <typename T>
// Stock code owns and copies this exact intrusive-pointer ABI. Its virtual
// destructor and pointer member must retain their order and layout.
class ManagedPtr {
public:
    ManagedPtr() : object_(nullptr) {}

    explicit ManagedPtr(T* object) : object_(object) {
        retain(object_);
    }

    ManagedPtr(const ManagedPtr& other) : object_(other.object_) {
        retain(object_);
    }

    virtual ~ManagedPtr() {
        release(object_);
    }

    // reset() handles both self-assignment and another pointer to the same object.
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment)
    ManagedPtr& operator=(const ManagedPtr& other) {
        reset(other.object_);
        return *this;
    }

    void reset(T* replacement = nullptr) {
        if (replacement == object_) {
            return;
        }
        retain(replacement);
        T* old = object_;
        object_ = replacement;
        release(old);
    }

    T* get() const {
        return object_;
    }
    T* operator->() const {
        return object_;
    }
    explicit operator bool() const {
        return object_ != nullptr;
    }

private:
    static void retain(T* object) {
        if (object != nullptr) {
            static_cast<ManagedObject*>(object)->retain();
        }
    }

    static void release(T* object) {
        if (object != nullptr) {
            static_cast<ManagedObject*>(object)->release();
        }
    }

    T* object_;
};

struct BookSecurity {};

class IBookManager;
class IBookMetadata;
class IBookSecurityManager;
class IContentProvider;
class IFragment;
class IGuideItem;
class IMetadataProvider;
class IParsedContent;
class IResource;
class IResourceContainer;
class ISecondaryTableOfContents;
class ISecondaryTOCProvider;
class ISkeleton;
class ISkeletonLink;
class ITableOfContents;
class ITableOfContentsEntry;
class IWord;
class IWordIterator;

// Only ownership crosses this module for these two stock-defined interfaces.
// Their methods are never invoked by the EPUB implementation.
class IBookSecurityManager : public ManagedObject {};
class IResourceContainer : public ManagedObject {};

class IBookManager : public ManagedObject {
public:
    virtual Result getContentProvider(ManagedPtr<IContentProvider>& output,
                                      const BookSecurity& security) = 0;
    virtual Result getContentProvider(ManagedPtr<IContentProvider>& output,
                                      ManagedPtr<IBookSecurityManager> securityManager) = 0;
    virtual Result getUserLocationForPosition(unsigned long& output, unsigned long position) = 0;
    virtual Result getPositionForUserLocation(unsigned long& output, unsigned long location) = 0;
    virtual Result getMetadataProvider(ManagedPtr<IMetadataProvider>& output) = 0;
#ifndef KINDLE_EPUB_EARLY_MOBI8_ABI
    virtual bool bookContainsResourceContainers() = 0;
    virtual Result
    getResourceContainersInfo(std::vector<ManagedPtr<IResourceContainer>>& output) = 0;
    virtual Result attachResourceContainer(const unsigned char* path,
                                           ManagedPtr<IResourceContainer>& output) = 0;
#endif
};

class IBookMetadata : public ManagedObject {
public:
    virtual const unsigned char* getTitle() const = 0;
    virtual unsigned long getBookSize() const = 0;
    virtual Result getAuthors(std::vector<std::string>& output) const = 0;
    virtual const unsigned char* getPublicationDate() const = 0;
    virtual const unsigned char* getRights() const = 0;
    virtual const unsigned char* getASIN() const = 0;
    virtual const unsigned char* getCdeKey() const = 0;
    virtual const unsigned char* getCdeType() const = 0;
    virtual const unsigned char* getPublisher() const = 0;
    virtual const unsigned char* getGUID() const = 0;
    virtual const unsigned char* getBookDescription() const = 0;
    virtual bool isSample() const = 0;
    virtual unsigned long getClippingLimit() const = 0;
    virtual bool isPublisherLimit() const = 0;
    virtual unsigned long getContentLanguage() const = 0;
    virtual bool isTTSMetadataPresent() const = 0;
    virtual bool isTTSAllowed() const = 0;
    virtual const unsigned char* getWatermark() const = 0;
    virtual bool hasDRM() const = 0;
    virtual bool isTSC() const = 0;
    virtual bool isLocationsMatch() const = 0;
    virtual unsigned long getSampleStartLocation() const = 0;
    virtual unsigned long getSampleEndLocation() const = 0;
    virtual unsigned long getVersion() const = 0;
    virtual unsigned long getStartReadingLocation() const = 0;
    virtual unsigned long getCreatorSoftware() const = 0;
    virtual unsigned long getCreatorMajorVersion() const = 0;
    virtual unsigned long getCreatorMinorVersion() const = 0;
    virtual unsigned long getCreatorBuildNumber() const = 0;
    virtual unsigned long getContentType() const = 0;
    virtual bool hasSpecialFeature(unsigned long feature) const = 0;
};

class IContentProvider : public ManagedObject {
public:
    virtual Result getFragment(ManagedPtr<IFragment>& output, unsigned long position) = 0;
    virtual Result getSkeleton(ManagedPtr<ISkeleton>& output, unsigned long position) = 0;
    virtual Result getResource(ManagedPtr<IResource>& output, const unsigned char* resourceId) = 0;
    virtual unsigned long getLinkType(const unsigned char* link) = 0;
    virtual Result getPositionForLink(unsigned long& output, const unsigned char* link) = 0;
    virtual Result getFragmentForLink(ManagedPtr<IFragment>& output, const unsigned char* link) = 0;
    virtual Result
    getWordIterator(ManagedPtr<IWordIterator>& output, unsigned long position, bool forward) = 0;
    virtual bool isTableOfContentsAvailable() = 0;
    virtual Result getTableOfContents(ManagedPtr<ITableOfContents>& output) = 0;
    virtual bool isSecondaryTableOfContentsAvailable() = 0;
    virtual Result getSecondaryTOCProvider(ManagedPtr<ISecondaryTOCProvider>& output) = 0;
#ifndef KINDLE_EPUB_KPP_ABI
    virtual Result
    extractContent(ManagedPtr<IParsedContent>& output, unsigned long start, unsigned long end) = 0;
    virtual Result
    extractWords(ManagedPtr<IParsedContent>& output, unsigned long start, unsigned long end) = 0;
#endif
    virtual Result extractExactContent(ManagedPtr<IParsedContent>& output,
                                       unsigned long start,
                                       unsigned long end) = 0;
#ifndef KINDLE_EPUB_EARLY_MOBI8_ABI
    virtual Result getContainerResourceList(unsigned long containerId,
                                            std::vector<std::string>& output) = 0;
    virtual Result getResourceFromContainer(ManagedPtr<IResource>& output,
                                            const unsigned char* path,
                                            unsigned long containerId) = 0;
#endif
};

class IFragment : public ManagedObject {
public:
    virtual unsigned long id() const = 0;
    virtual unsigned long getPosition() const = 0;
    virtual unsigned long getOffset() const = 0;
    virtual unsigned long getSkeletonId() const = 0;
    virtual ManagedPtr<ISkeletonLink> getSkeletonLink() = 0;
    virtual const unsigned char* getContent() const = 0;
    virtual unsigned long getContentLength() const = 0;
    virtual bool hasPreviousFragment() = 0;
    virtual bool hasNextFragment() = 0;
    virtual Result getPreviousFragment(ManagedPtr<IFragment>& output) = 0;
    virtual Result getNextFragment(ManagedPtr<IFragment>& output) = 0;
};

class IGuideItem : public ManagedObject {
public:
    virtual const unsigned char* getTitle() const = 0;
    virtual const unsigned char* getType() const = 0;
    virtual const unsigned char* getResourceId() const = 0;
};

class IMetadataProvider : public ManagedObject {
public:
    virtual Result getBookMetadata(ManagedPtr<IBookMetadata>& output) = 0;
    virtual Result getExtraHeaderValueAsString(std::string& output, unsigned long key) = 0;
    virtual Result getExtraHeaderValueAsUINT32(unsigned long& output, unsigned long key) = 0;
    virtual Result getExtraHeaderStringValueList(std::vector<std::string>& output,
                                                 unsigned long key) = 0;
    virtual Result getExtraHeaderValueAsResource(ManagedPtr<IResource>& output,
                                                 unsigned long key) = 0;
    virtual Result getExtraHeaderUINT32ValueList(std::vector<unsigned long>& output,
                                                 unsigned long key) = 0;
    virtual bool hasBigCoverArt() = 0;
    virtual Result getBigCoverArt(ManagedPtr<IResource>& output) = 0;
    virtual Result getBigCoverArtResourceIdOnly(std::string& output) = 0;
    virtual Result getCoverArt(ManagedPtr<IResource>& output) = 0;
    virtual Result getGuideItems(std::vector<ManagedPtr<IGuideItem>>& output) = 0;
    virtual Result getGuideItem(ManagedPtr<IGuideItem>& output, const unsigned char* type) = 0;
    virtual Result getMetadataResource(ManagedPtr<IResource>& output,
                                       unsigned long& offset,
                                       unsigned long& length) = 0;
    virtual Result getMetadataValueAsString(std::string& output, unsigned long key) = 0;
    virtual Result getMetadataValueAsString(std::string& output, const unsigned char* key) = 0;
    virtual Result getMetadataValueAsUINT32(unsigned long& output, unsigned long key) = 0;
    virtual Result getMetadataValueAsUINT32(unsigned long& output, const unsigned char* key) = 0;
    virtual Result getMetadataStringValueList(std::vector<std::string>& output,
                                              unsigned long key) = 0;
    virtual Result getMetadataStringValueList(std::vector<std::string>& output,
                                              const unsigned char* key) = 0;
    virtual Result getMetadataUINT32ValueList(std::vector<unsigned long>& output,
                                              unsigned long key) = 0;
    virtual Result getMetadataUINT32ValueList(std::vector<unsigned long>& output,
                                              const unsigned char* key) = 0;
    virtual Result getMetadataValueAsResource(ManagedPtr<IResource>& output, unsigned long key) = 0;
    virtual Result getMetadataValueAsResource(ManagedPtr<IResource>& output,
                                              const unsigned char* key) = 0;
};

class IParsedContent : public ManagedObject {
public:
    virtual const unsigned char* getContent() const = 0;
    virtual unsigned long getStartPosition() const = 0;
    virtual unsigned long getEndPosition() const = 0;
    virtual unsigned long getContentLength() const = 0;
};

class IResource : public ManagedObject {
public:
    virtual const unsigned char* getContent() const = 0;
    virtual unsigned long getContentLength() const = 0;
    virtual const unsigned char* getMimeType() const = 0;
    virtual const unsigned char* getResourceId() const = 0;
};

class ISecondaryTableOfContents : public ManagedObject {
public:
    virtual Result getFirstEntry(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual const unsigned char* getLabel() const = 0;
    virtual const unsigned char* getClass() const = 0;
};

class ISecondaryTOCProvider : public ManagedObject {
public:
    virtual Result getSecondaryTOCCount(unsigned long& output) = 0;
    virtual Result getSecondaryTOCByIndex(ManagedPtr<ISecondaryTableOfContents>& output,
                                          unsigned long index) = 0;
};

class ISkeleton : public ManagedObject {
public:
    virtual unsigned long id() const = 0;
    virtual bool hasParentSkeleton() const = 0;
    virtual unsigned long getParentSkeletonId() const = 0;
    virtual ManagedPtr<ISkeletonLink> getParentSkeletonLink() const = 0;
    virtual unsigned long getFragmentCount() const = 0;
    virtual unsigned long getSubSkeletonCount() const = 0;
    virtual const unsigned char* getContent() const = 0;
    virtual unsigned long getContentLength() const = 0;
    virtual bool isPropertySet(const unsigned char* property) = 0;
    virtual Result getPropertyList(std::vector<std::string>& output) = 0;
    virtual Result getMetadata(std::string& output, const unsigned char* key) = 0;
};

class ISkeletonLink : public ManagedObject {
public:
    virtual const unsigned char* getXPath() const = 0;
    virtual unsigned long getLinkType() const = 0;
};

class ITableOfContents : public ManagedObject {
public:
    virtual Result getEntryFromPosition(ManagedPtr<ITableOfContentsEntry>& output,
                                        unsigned long position) = 0;
    virtual Result getEntryForFirstSection(ManagedPtr<ITableOfContentsEntry>& output) = 0;
};

class ITableOfContentsEntry : public ManagedObject {
public:
    virtual unsigned long getLevel() const = 0;
    virtual unsigned long getLength() const = 0;
    virtual unsigned long getPosition() const = 0;
    virtual const unsigned char* getNodeClass() const = 0;
    virtual const unsigned char* getLabel() const = 0;
    virtual Result getFirstChild(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual Result getLastChild(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual Result getChildren(std::vector<ManagedPtr<ITableOfContentsEntry>>& output) = 0;
    virtual Result getParent(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual Result getNextEntry(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual Result getPreviousEntry(ManagedPtr<ITableOfContentsEntry>& output) = 0;
    virtual Result getImageResourcesByTagName(std::vector<ManagedPtr<IResource>>& output,
                                              const unsigned char* tagName) = 0;
    virtual Result getStringsByTagName(std::vector<std::string>& output,
                                       const unsigned char* tagName) = 0;
#ifdef KINDLE_EPUB_KPP_ABI
    // These two slots are appended by the Mobi8 ABI embedded in KPP 5.19.2.
    virtual Result getStringMetadataList(std::vector<std::string>& names,
                                         std::vector<std::string>& values) = 0;
    virtual Result getImageMetadataList(std::vector<std::string>& names,
                                        std::vector<ManagedPtr<IResource>>& values) = 0;
#endif
};

class IWord : public ManagedObject {
public:
    virtual const unsigned char* get() const = 0;
    virtual unsigned long getPosition() const = 0;
    virtual unsigned long getEndPosition() const = 0;
    virtual bool hasLeadingSpace() const = 0;
};

class IWordIterator : public ManagedObject {
public:
    virtual bool hasNext() = 0;
    virtual Result next(ManagedPtr<IWord>& output) = 0;
    virtual Result next(std::vector<ManagedPtr<IWord>>& output, int count) = 0;
    virtual Result gotoPosition(unsigned long position) = 0;
    virtual unsigned long getWordIteratorType() = 0;
};

} // namespace Mobi8SDK

#endif
