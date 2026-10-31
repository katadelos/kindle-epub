#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mobi8/model.hpp"

namespace {

struct TestFailure : public std::runtime_error {
    explicit TestFailure(const std::string& message) : std::runtime_error(message) {}
};

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw TestFailure(message);
    }
}

template <typename Left, typename Right>
void checkEqual(const Left& actual, const Right& expected, const std::string& message) {
    if (!(actual == expected)) {
        throw TestFailure(message);
    }
}

std::string text(const unsigned char* value) {
    return value == nullptr ? std::string() : std::string(reinterpret_cast<const char*>(value));
}

class TemporaryFile {
public:
    TemporaryFile() {
        char pattern[] = "/tmp/kindle-mobi8-adapter-test-XXXXXX";
        const int descriptor = mkstemp(pattern);
        if (descriptor < 0) {
            throw TestFailure("mkstemp failed");
        }
        close(descriptor);
        std::remove(pattern);
        path_ = pattern;
    }

    ~TemporaryFile() {
        std::remove(path_.c_str());
    }
    const std::string& path() const {
        return path_;
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

private:
    std::string path_;
};

using Entries = std::vector<std::pair<std::string, std::string>>;

void writeZip(const std::string& path, const Entries& entries) {
    struct archive* writer = archive_write_new();
    if (writer == nullptr) {
        throw TestFailure("archive_write_new failed");
    }
    if (archive_write_set_format_zip(writer) != ARCHIVE_OK ||
        archive_write_set_options(writer, "zip:compression=store") != ARCHIVE_OK ||
        archive_write_open_filename(writer, path.c_str()) != ARCHIVE_OK) {
        const std::string detail = archive_error_string(writer) == nullptr
                                       ? "unknown libarchive error"
                                       : archive_error_string(writer);
        archive_write_free(writer);
        throw TestFailure("cannot create fixture: " + detail);
    }
    for (const auto& zipEntry : entries) {
        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname(entry, zipEntry.first.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        constexpr int fixturePermissions = 0644;
        archive_entry_set_perm(entry, fixturePermissions);
        archive_entry_set_size(entry, static_cast<la_int64_t>(zipEntry.second.size()));
        if (archive_write_header(writer, entry) != ARCHIVE_OK ||
            archive_write_data(writer, zipEntry.second.data(), zipEntry.second.size()) !=
                static_cast<la_ssize_t>(zipEntry.second.size())) {
            const std::string detail = archive_error_string(writer) == nullptr
                                           ? "unknown libarchive error"
                                           : archive_error_string(writer);
            archive_entry_free(entry);
            archive_write_close(writer);
            archive_write_free(writer);
            throw TestFailure("cannot write fixture: " + detail);
        }
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
}

Entries epubEntries() {
    Entries entries;
    entries.push_back(std::make_pair("mimetype", "application/epub+zip"));
    entries.push_back(
        std::make_pair("META-INF/container.xml",
                       "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                       "version=\"1.0\"><rootfiles><rootfile full-path=\"OPS/book.opf\" "
                       "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    entries.push_back(
        std::make_pair("OPS/book.opf",
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                       "unique-identifier=\"id\"><metadata "
                       "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                       "<dc:identifier id=\"id\">urn:test:mobi8-adapter</dc:identifier>"
                       "<dc:title>ABI EPUB</dc:title><dc:creator>Native Author</dc:creator>"
                       "<dc:language>en</dc:language></metadata><manifest>"
                       "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
                       "properties=\"nav\"/>"
                       "<item id=\"one\" href=\"one.xhtml\" media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"two\" href=\"two.xhtml\" media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"cover\" href=\"cover.jpg\" media-type=\"image/jpeg\" "
                       "properties=\"cover-image\"/>"
                       "</manifest><spine><itemref idref=\"one\"/><itemref idref=\"two\"/>"
                       "</spine></package>"));
    entries.push_back(
        std::make_pair("OPS/nav.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                       "xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
                       "<nav epub:type=\"toc\"><ol>"
                       "<li><a href=\"missing.xhtml\">Broken</a></li>"
                       "<li><a href=\"one.xhtml#start\">One</a>"
                       "<ol><li><span>Group</span><ol><li><a href=\"two.xhtml#target\">Two</a>"
                       "</li></ol></li></ol>"
                       "</li></ol></nav></body></html>"));
    entries.push_back(
        std::make_pair("OPS/one.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>One</title>"
                       "</head><body><h1 id=\"start\">One</h1><p>Hello native words.</p>"
                       "<img src=\"cover.jpg\"/><a href=\"two.xhtml#target\">Next</a>"
                       "</body></html>"));
    entries.push_back(
        std::make_pair("OPS/two.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Two</title>"
                       "</head><body><h1 id=\"target\">Two</h1><p>Finished.</p>"
                       "</body></html>"));
    entries.push_back(std::make_pair("OPS/cover.jpg", "COVER-BYTES"));
    return entries;
}

void testAdapter() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epubEntries());

    Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager> manager;
    checkEqual(kindle_epub::createBookManager(manager, fixture.path().c_str()),
               Mobi8SDK::SUCCESS,
               "factory rejected a valid EPUB");
    check(manager.get() != nullptr, "factory returned a null manager");
#ifndef KINDLE_EPUB_EARLY_MOBI8_ABI
    check(!manager->bookContainsResourceContainers(),
          "EPUB unexpectedly advertised a resource container");
#endif

    Mobi8SDK::ManagedPtr<Mobi8SDK::IMetadataProvider> metadataProvider;
    checkEqual(manager->getMetadataProvider(metadataProvider),
               Mobi8SDK::SUCCESS,
               "metadata provider failed");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IBookMetadata> metadata;
    checkEqual(
        metadataProvider->getBookMetadata(metadata), Mobi8SDK::SUCCESS, "book metadata failed");
    checkEqual(text(metadata->getTitle()), std::string("ABI EPUB"), "metadata title mismatch");
    std::vector<std::string> authors;
    checkEqual(metadata->getAuthors(authors), Mobi8SDK::SUCCESS, "metadata authors failed");
    checkEqual(authors.size(), std::size_t(1), "metadata author count mismatch");
    checkEqual(authors[0], std::string("Native Author"), "metadata author mismatch");
    check(metadata->getBookSize() > 0, "metadata reports an empty book");
    check(text(metadata->getGUID()).find("EPUB-") == 0, "metadata GUID is not stable EPUB form");
    constexpr std::size_t stableIdentityLength = sizeof("EPUB-") - 1 + 16;
    checkEqual(text(metadata->getGUID()).size(),
               stableIdentityLength,
               "metadata GUID does not contain the 64-bit stable identity");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IResource> cover;
    checkEqual(metadataProvider->getCoverArt(cover), Mobi8SDK::SUCCESS, "cover lookup failed");
    checkEqual(
        std::string(reinterpret_cast<const char*>(cover->getContent()), cover->getContentLength()),
        std::string("COVER-BYTES"),
        "cover bytes mismatch");
    const std::string coverResourceId = text(cover->getResourceId());
    check(coverResourceId.find("kindle:embed:") == 0,
          "cover resource ID is not a Kindle embed URI");
    check(cover->getMimeType() == nullptr,
          "bare cover resource unexpectedly advertised a MIME query");

    Mobi8SDK::BookSecurity security;
    Mobi8SDK::ManagedPtr<Mobi8SDK::IContentProvider> content;
    checkEqual(manager->getContentProvider(content, security),
               Mobi8SDK::SUCCESS,
               "content provider failed");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IResource> bareResource;
    checkEqual(content->getResource(
                   bareResource, reinterpret_cast<const unsigned char*>(coverResourceId.c_str())),
               Mobi8SDK::SUCCESS,
               "bare resource lookup failed");
    checkEqual(text(bareResource->getResourceId()),
               coverResourceId,
               "bare resource did not preserve the requested URI");
    check(bareResource->getMimeType() == nullptr,
          "bare resource unexpectedly advertised a MIME query");

    const std::string queriedResourceId = coverResourceId + "?mime=image/x-epub-probe";
    Mobi8SDK::ManagedPtr<Mobi8SDK::IResource> queriedResource;
    checkEqual(
        content->getResource(queriedResource,
                             reinterpret_cast<const unsigned char*>(queriedResourceId.c_str())),
        Mobi8SDK::SUCCESS,
        "queried resource lookup failed");
    checkEqual(text(queriedResource->getResourceId()),
               queriedResourceId,
               "queried resource did not preserve the requested URI");
    checkEqual(text(queriedResource->getMimeType()),
               std::string("image/x-epub-probe"),
               "queried resource did not preserve the requested MIME");
    checkEqual(std::string(reinterpret_cast<const char*>(queriedResource->getContent()),
                           queriedResource->getContentLength()),
               std::string("COVER-BYTES"),
               "queried cover resource bytes mismatch");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> first;
    checkEqual(content->getFragment(first, 0), Mobi8SDK::SUCCESS, "first fragment lookup failed");
    check(first->getContentLength() > 0, "first fragment is empty");
    check(first->hasNextFragment(), "next fragment was not advertised");
    Mobi8SDK::ManagedPtr<Mobi8SDK::ISkeletonLink> skeletonLink = first->getSkeletonLink();
    check(skeletonLink.get() != nullptr, "fragment skeleton link is null");
    check(text(skeletonLink->getXPath()).find("//*[@aid='0']") != std::string::npos,
          "fragment skeleton XPath mismatch");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> adjacent;
    checkEqual(
        first->getNextFragment(adjacent), Mobi8SDK::SUCCESS, "next fragment traversal failed");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> adjacentAgain;
    checkEqual(first->getNextFragment(adjacentAgain),
               Mobi8SDK::SUCCESS,
               "repeated next fragment traversal failed");
    check(adjacentAgain.get() == adjacent.get(),
          "next fragment traversal did not preserve object identity");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> backToFirst;
    checkEqual(adjacent->getPreviousFragment(backToFirst),
               Mobi8SDK::SUCCESS,
               "reverse fragment traversal failed");
    check(backToFirst.get() != first.get(),
          "reverse fragment traversal unexpectedly reused the originating object");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> backToFirstAgain;
    checkEqual(adjacent->getPreviousFragment(backToFirstAgain),
               Mobi8SDK::SUCCESS,
               "repeated reverse fragment traversal failed");
    check(backToFirstAgain.get() == backToFirst.get(),
          "previous fragment traversal did not preserve object identity");

    const std::string fragment(reinterpret_cast<const char*>(first->getContent()),
                               first->getContentLength());
    const std::size_t linkStart = fragment.find("kindle:epub-link:");
    check(linkStart != std::string::npos, "fragment has no native link");
    const std::size_t linkEnd = fragment.find('\"', linkStart);
    const std::string link = fragment.substr(linkStart, linkEnd - linkStart);
    checkEqual(content->getLinkType(reinterpret_cast<const unsigned char*>(link.c_str())),
               1UL,
               "native link type mismatch");
    unsigned long linkedPosition = 0;
    checkEqual(content->getPositionForLink(linkedPosition,
                                           reinterpret_cast<const unsigned char*>(link.c_str())),
               Mobi8SDK::SUCCESS,
               "native link position lookup failed");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> second;
    checkEqual(
        content->getFragmentForLink(second, reinterpret_cast<const unsigned char*>(link.c_str())),
        Mobi8SDK::SUCCESS,
        "native link fragment lookup failed");
    check(second->getPosition() <= linkedPosition, "linked fragment starts after its target");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> missingLink = second;
    const auto* unknownLink = reinterpret_cast<const unsigned char*>("kindle:epub-link:999999");
    checkEqual(content->getFragmentForLink(missingLink, unknownLink),
               Mobi8SDK::LOOKUP_STRING_NOT_FOUND,
               "unknown link unexpectedly resolved");
    check(!missingLink, "failed link lookup retained the previous fragment");
    checkEqual(content->getLinkType(unknownLink),
               2UL,
               "unregistered link was advertised as a valid internal link");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> suffix;
    const unsigned long finalEnd = second->getPosition() + second->getContentLength();
    checkEqual(content->getFragment(suffix, finalEnd),
               Mobi8SDK::SUCCESS,
               "final skeleton suffix lookup failed");
    check(suffix->getOffset() < suffix->getContentLength(),
          "final skeleton suffix points beyond fragment content");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IFragment> previous;
    checkEqual(second->getPreviousFragment(previous),
               Mobi8SDK::SUCCESS,
               "previous fragment traversal failed");
    checkEqual(
        previous->getOffset(), 0UL, "previous fragment traversal did not enter at its start");

    check(content->isTableOfContentsAvailable(), "TOC was not advertised");
    Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContents> toc;
    checkEqual(content->getTableOfContents(toc), Mobi8SDK::SUCCESS, "TOC lookup failed");
    Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContentsEntry> entry;
    checkEqual(
        toc->getEntryForFirstSection(entry), Mobi8SDK::SUCCESS, "first TOC entry lookup failed");
    checkEqual(text(entry->getLabel()), std::string("One"), "first TOC label mismatch");
    Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContentsEntry> child;
    checkEqual(entry->getFirstChild(child), Mobi8SDK::SUCCESS, "nested TOC entry lookup failed");
    checkEqual(text(child->getLabel()), std::string("Group"), "navigation group label mismatch");
    checkEqual(child->getPosition(),
               linkedPosition,
               "navigation group did not use its first valid descendant");
    Mobi8SDK::ManagedPtr<Mobi8SDK::ITableOfContentsEntry> grandchild;
    checkEqual(
        child->getFirstChild(grandchild), Mobi8SDK::SUCCESS, "group descendant lookup failed");
    checkEqual(text(grandchild->getLabel()), std::string("Two"), "nested TOC label mismatch");
    checkEqual(toc->getEntryFromPosition(grandchild, metadata->getBookSize()),
               Mobi8SDK::POSITION_OUT_OF_BOUND,
               "TOC lookup accepted a position past the book");
    check(!grandchild, "failed TOC lookup retained an old entry");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IWordIterator> words;
    checkEqual(
        content->getWordIterator(words, 0, true), Mobi8SDK::SUCCESS, "word iterator lookup failed");
    check(words->hasNext(), "word iterator is unexpectedly empty");
    Mobi8SDK::ManagedPtr<Mobi8SDK::IWord> word;
    checkEqual(words->next(word), Mobi8SDK::SUCCESS, "word iterator next failed");
    check(!text(word->get()).empty(), "word iterator returned empty text");
    checkEqual(words->getWordIteratorType(), 1UL, "word iterator type mismatch");
    checkEqual(words->gotoPosition(metadata->getBookSize() + 1),
               Mobi8SDK::POSITION_OUT_OF_BOUND,
               "word seek accepted an invalid position");
    checkEqual(content->getWordIterator(words, metadata->getBookSize() + 1, true),
               Mobi8SDK::POSITION_OUT_OF_BOUND,
               "word iterator accepted an invalid position");
    check(!words, "failed iterator lookup retained a previous iterator");

    Mobi8SDK::ManagedPtr<Mobi8SDK::IParsedContent> extracted;
#ifdef KINDLE_EPUB_KPP_ABI
    checkEqual(content->extractExactContent(extracted, 0, metadata->getBookSize()),
               Mobi8SDK::SUCCESS,
               "exact extraction failed");
#else
    checkEqual(content->extractWords(extracted, 0, metadata->getBookSize()),
               Mobi8SDK::SUCCESS,
               "word extraction failed");
#endif
    check(text(extracted->getContent()).find("native") != std::string::npos,
          "word extraction omitted expected text");

    unsigned long location = 0;
    unsigned long roundTrip = 0;
    checkEqual(manager->getUserLocationForPosition(location, linkedPosition),
               Mobi8SDK::SUCCESS,
               "position-to-location conversion failed");
    checkEqual(manager->getPositionForUserLocation(roundTrip, location),
               Mobi8SDK::SUCCESS,
               "location-to-position conversion failed");
    check(roundTrip <= linkedPosition, "location round-trip moved forward");
}

void testFactoryFailures() {
    Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager> manager;
    checkEqual(kindle_epub::createBookManager(manager, nullptr),
               Mobi8SDK::FAILED_LOADING_FILE,
               "null path did not produce a loading error");
    check(manager.get() == nullptr, "failed factory retained a manager");
    checkEqual(kindle_epub::createBookManager(manager, "/does/not/exist.epub"),
               Mobi8SDK::FAILED_LOADING_FILE,
               "missing path did not produce a loading error");
}

} // namespace

int main() {
    try {
        testAdapter();
        testFactoryFailures();
        std::cout << "kindle_mobi8_adapter_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "kindle_mobi8_adapter_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
