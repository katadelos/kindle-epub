#include <archive.h>
#include <archive_entry.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "epub/error.hpp"
#include "epub/path.hpp"
#include "reader/book.hpp"
#include "reader/css.hpp"
#include "reader/text.hpp"

namespace reader = kindle_epub::reader;

namespace {

const int kConcurrentReadCount = 20;

static_assert(!std::is_default_constructible<epub::Package>::value,
              "a package must be created by parsing a validated archive");
static_assert(!std::is_default_constructible<epub::Reference>::value,
              "a reference must be created by resolving a URI");

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

class TemporaryFile {
public:
    TemporaryFile() {
        char pattern[] = "/tmp/kindle-epub-test-XXXXXX";
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

void writeZip(const std::string& path,
              const Entries& entries,
              const char* compression = "store",
              bool forceZip64 = false) {
    struct archive* writer = archive_write_new();
    if (writer == nullptr) {
        throw TestFailure("archive_write_new failed");
    }
    const std::string options =
        std::string("zip:compression=") + compression + (forceZip64 ? ",zip:zip64" : "");
    if (archive_write_set_format_zip(writer) != ARCHIVE_OK ||
        archive_write_set_options(writer, options.c_str()) != ARCHIVE_OK ||
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
        // Fixture permissions are the ZIP regular-file mode.
        archive_entry_set_perm(entry, 0644); // NOLINT(readability-magic-numbers)
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

Entries epub3Entries() {
    Entries entries;
    entries.push_back(std::make_pair("mimetype", "application/epub+zip"));
    entries.push_back(
        std::make_pair("META-INF/container.xml",
                       "<?xml version=\"1.0\"?>"
                       "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                       "version=\"1.0\"><rootfiles><rootfile full-path=\"OPS/package.opf\" "
                       "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    entries.push_back(std::make_pair(
        "OPS/package.opf",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
        "unique-identifier=\"book-id\"><metadata "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<dc:identifier id=\"book-id\">urn:test:direct-epub</dc:identifier>"
        "<dc:title>Direct EPUB</dc:title><dc:creator>Ada Reader</dc:creator>"
        "<dc:language>en</dc:language><dc:publisher>Test Press</dc:publisher>"
        "<dc:description>Native model fixture</dc:description>"
        "<meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>"
        "</metadata><manifest>"
        "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
        "properties=\"nav\"/>"
        "<item id=\"c1\" href=\"Text/ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"c2\" href=\"Text/ch2.xhtml\" media-type=\"application/xhtml+xml\"/>"
        "<item id=\"css\" href=\"Styles/main.css\" media-type=\"text/css\"/>"
        "<item id=\"cover\" href=\"Images/cover%20art.jpg\" media-type=\"image/jpeg\" "
        "properties=\"cover-image\"/>"
        "</manifest><spine page-progression-direction=\"ltr\">"
        "<itemref idref=\"c1\"/><itemref idref=\"c2\"/>"
        "</spine></package>"));
    entries.push_back(
        std::make_pair("OPS/nav.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                       "xmlns:epub=\"http://www.idpf.org/2007/ops\"><body>"
                       "<nav epub:type=\"toc\"><ol><li><a href=\"Text/ch1.xhtml#start\">"
                       "Chapter One</a><ol><li><a href=\"Text/ch2.xhtml#t&amp;arget\">"
                       "Chapter Two</a></li></ol></li></ol></nav></body></html>"));
    entries.push_back(
        std::make_pair("OPS/Text/ch1.xhtml",
                       "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>"
                       "<title>One</title><link rel=\"stylesheet\" href=\"../Styles/main.css\"/>"
                       "<style>.chapter { display: -webkit-flex; }</style>"
                       "<script>danger()</script></head><body class=\"chapter\" onclick=\"bad()\">"
                       "<h1 id=\"start\">Chapter One</h1>"
                       "<p style=\"display: inline-flex\">Hello &amp; native EPUB.</p>"
                       "<br/><img src=\"../Images/cover%20art.jpg\"/>"
                       "<a href=\"ch2.xhtml#t&amp;arget\">Continue</a></body></html>"));
    entries.push_back(std::make_pair(
        "OPS/Text/ch2.xhtml",
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Two</title>"
        "</head><body><h1 id=\"t&amp;arget\">Chapter Two</h1><p>Caf\xC3\xA9 finish.</p>"
        "</body></html>"));
    entries.push_back(std::make_pair("OPS/Styles/main.css",
                                     "body { background-image: url('../Images/cover%20art.jpg'); }"
                                     "@supports (display: FLEX) { main { display: flex; } }"
                                     ".badge { display : -webkit-inline-flex !important; }"));
    entries.push_back(std::make_pair("OPS/Images/cover art.jpg", "JPEGDATA"));
    return entries;
}

Entries epub2Entries() {
    Entries entries;
    entries.push_back(std::make_pair("mimetype", "application/epub+zip"));
    entries.push_back(
        std::make_pair("META-INF/container.xml",
                       "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" "
                       "version=\"1.0\"><rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                       "media-type=\"application/oebps-package+xml\"/></rootfiles></container>"));
    entries.push_back(
        std::make_pair("OEBPS/content.opf",
                       "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"2.0\">"
                       "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                       "<dc:identifier>urn:test:epub2</dc:identifier><dc:title>EPUB Two</dc:title>"
                       "<dc:language>fr</dc:language><meta name=\"cover\" content=\"cover\"/>"
                       "</metadata><manifest>"
                       "<item id=\"chapter\" href=\"chapter.xhtml\" "
                       "media-type=\"application/xhtml+xml\"/>"
                       "<item id=\"ncx\" href=\"toc.ncx\" "
                       "media-type=\"application/x-dtbncx+xml\"/>"
                       "<item id=\"cover\" href=\"cover.jpg\" media-type=\"image/jpeg\"/>"
                       "</manifest><spine toc=\"ncx\"><itemref idref=\"chapter\"/>"
                       "</spine></package>"));
    entries.push_back(std::make_pair("OEBPS/toc.ncx",
                                     "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\"><navMap>"
                                     "<navPoint><navLabel><text>French Chapter</text></navLabel>"
                                     "<content src=\"chapter.xhtml#start\"/><navPoint>"
                                     "<navLabel><text>Nested</text></navLabel>"
                                     "<content src=\"chapter.xhtml#nested\"/></navPoint></navPoint>"
                                     "</navMap></ncx>"));
    entries.push_back(std::make_pair("OEBPS/chapter.xhtml",
                                     "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
                                     "<h1 id=\"start\">Bonjour</h1><p id=\"nested\">Monde</p>"
                                     "</body></html>"));
    entries.push_back(std::make_pair("OEBPS/cover.jpg", "EPUB2-COVER"));
    return entries;
}

void testPaths() {
    checkEqual(epub::canonicalArchivePath("OPS/./Text/../Text/ch1.xhtml"),
               std::string("OPS/Text/ch1.xhtml"),
               "canonical path mismatch");
    const epub::Reference reference =
        epub::resolveReference("OPS/Text/ch1.xhtml", "../Images/cover%20art.jpg#image");
    check(!reference.isExternal(), "local reference was marked external");
    checkEqual(
        reference.path(), std::string("OPS/Images/cover art.jpg"), "relative reference mismatch");
    checkEqual(reference.fragment(), std::string("image"), "fragment decoding mismatch");

    bool rejected = false;
    try {
        epub::resolveReference("OPS/chapter.xhtml", "../../escape");
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidPath;
    }
    check(rejected, "archive-root traversal was not rejected");
}

void testEpub3Model() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub3Entries());
    const std::shared_ptr<reader::Book> book = reader::Book::open(fixture.path());

    checkEqual(book->package().metadata().title, std::string("Direct EPUB"), "title mismatch");
    checkEqual(
        book->package().metadata().creators.size(), std::size_t(1), "creator count mismatch");
    checkEqual(book->package().coverPath(),
               std::string("OPS/Images/cover art.jpg"),
               "cover path mismatch");
    checkEqual(book->package().pageProgressionDirection(),
               std::string("ltr"),
               "page progression mismatch");
    checkEqual(
        book->package().navigation().size(), std::size_t(1), "top-level navigation count mismatch");
    checkEqual(book->package().navigation()[0].children.size(),
               std::size_t(1),
               "nested navigation count mismatch");
    checkEqual(book->documents().size(), std::size_t(2), "spine document count mismatch");
    check(book->size() > 0, "book byte-position space is empty");

    const reader::ContentDocument& first = book->documents()[0];
    const reader::ContentDocument& second = book->documents()[1];
    check(first.skeleton.find("<body aid=\"0\"") != std::string::npos,
          "skeleton placeholder is absent");
    check(first.skeleton.find("kindle:flow:") != std::string::npos,
          "stylesheet URL was not mapped to a native resource ID");
    check(first.fragment.find("kindle:embed:") != std::string::npos,
          "image URL was not mapped to a native resource ID");
    check(first.fragment.find("kindle:epub-link:") != std::string::npos,
          "internal link was not mapped to a native link ID");
    const std::size_t breakStart = first.fragment.find("<br");
    const std::size_t breakEnd = first.fragment.find("/>", breakStart);
    const std::size_t imageStart = first.fragment.find("<img");
    const std::size_t imageEnd = first.fragment.find("/>", imageStart);
    const std::size_t anchorStart = first.fragment.find("<a ");
    const std::size_t anchorEnd = first.fragment.find('>', anchorStart);
    check(breakStart != std::string::npos && breakEnd != std::string::npos &&
              first.fragment.substr(breakStart, breakEnd - breakStart).find("aid=") ==
                  std::string::npos,
          "void br element consumed an AID");
    check(imageStart != std::string::npos && imageEnd != std::string::npos &&
              first.fragment.substr(imageStart, imageEnd - imageStart).find("aid=") ==
                  std::string::npos,
          "void img element consumed an AID");
    check(anchorStart != std::string::npos && anchorEnd != std::string::npos &&
              first.fragment.substr(anchorStart, anchorEnd - anchorStart).find("aid=\"3\"") !=
                  std::string::npos,
          "void elements shifted the next addressable AID");
    checkEqual(second.placeholderId,
               std::string("4"),
               "spine documents did not share one addressable element sequence");
    check(first.fragment.find("danger") == std::string::npos,
          "script content survived sanitization");
    check(first.skeleton.find("onclick") == std::string::npos,
          "event handler survived sanitization");
    check(first.skeleton.find("display: block") != std::string::npos,
          "embedded stylesheet retained a pagination-hostile flex display");
    check(first.fragment.find("display: inline-block") != std::string::npos,
          "inline style retained a pagination-hostile inline-flex display");
    check(first.endPosition < second.startPosition,
          "fixture did not retain its inter-fragment skeleton gap");
    check(book->documentForPosition(first.endPosition).path == second.path,
          "fragment-end position did not advance to the next spine document");
    check(book->documentForPosition(first.endPosition + 1).path == second.path,
          "skeleton-gap position did not advance to the next spine document");
    check(book->documentForPosition(second.startPosition).path == second.path,
          "position did not select the second spine document");

    check(second.fragment.find("id=\"t&amp;arget\"") != std::string::npos,
          "fixture did not retain the escaped anchor attribute");
    const std::uint64_t target = book->positionForPath("OPS/Text/ch2.xhtml", "t&arget");
    check(target >= second.startPosition && target < second.endPosition,
          "anchor position is outside its document");
    const std::size_t linkStart = first.fragment.find("kindle:epub-link:");
    const std::size_t linkEnd = first.fragment.find('\"', linkStart);
    const std::string linkUri = first.fragment.substr(linkStart, linkEnd - linkStart);
    checkEqual(
        book->positionForLink(linkUri), target, "native link did not resolve to its byte position");
    bool missingLinkRejected = false;
    try {
        book->positionForLink("kindle:epub-link:missing");
    } catch (const epub::Error& error) {
        missingLinkRejected = error.code() == epub::ErrorCode::InvalidDocument;
    }
    check(missingLinkRejected, "an unknown link resolved to a fabricated position");

    std::string cssId;
    std::string imageId;
    for (const auto& resource : book->resources()) {
        if (resource.second.mediaType == "text/css") {
            cssId = resource.first;
        }
        if (resource.second.mediaType == "image/jpeg") {
            imageId = resource.first;
        }
    }
    check(!cssId.empty() && !imageId.empty(), "resource IDs were not assigned");
    const reader::ResourceData css = book->readResource(cssId);
    const std::string cssText(css.bytes.begin(), css.bytes.end());
    check(cssText.find(imageId) != std::string::npos, "nested CSS resource URL was not rewritten");
    check(cssText.find("display: block") != std::string::npos &&
              cssText.find("display : inline-block") != std::string::npos &&
              epub::lowercaseAscii(cssText).find("display: flex") == std::string::npos &&
              epub::lowercaseAscii(cssText).find("display : -webkit-inline-flex") ==
                  std::string::npos,
          "stylesheet retained a pagination-hostile flex display");
    const reader::ResourceData image = book->readResource(imageId + "?mime=image/jpeg");
    checkEqual(std::string(image.bytes.begin(), image.bytes.end()),
               std::string("JPEGDATA"),
               "resource bytes mismatch");

    bool sawNative = false;
    for (std::size_t i = 0; i < first.words.size(); ++i) {
        if (first.words[i].text == "native") {
            sawNative = true;
            check(first.words[i].position >= first.startPosition, "word position is not global");
        }
    }
    check(sawNative, "word iterator model did not extract expected text");
    const std::uint64_t samplePosition = 300;
    check(book->positionForUserLocation(book->userLocationForPosition(samplePosition)) <=
              samplePosition,
          "user-location conversion is not monotonic");
}

void testTextOffsetsAndCssBoundaries() {
    reader::ContentDocument document;
    document.fragment = "<p id=\"a&amp;b\"> one&amp;two &#160; caf\xc3\xa9&#x21; end.</p>";
    reader::indexDocumentText(document);
    checkEqual(document.plainText,
               std::string("one&two caf\xc3\xa9! end."),
               "text indexing changed decoded words or spacing");
    checkEqual(document.anchorOffsets.at("a&b"),
               std::uint64_t(0),
               "decoded anchor lost its serialized tag offset");
    checkEqual(document.words.size(), std::size_t(3), "entity text changed word boundaries");
    const reader::Word& first = document.words[0];
    checkEqual(first.position,
               document.fragment.find("one&amp;two"),
               "word start is not a serialized byte offset");
    checkEqual(first.endPosition,
               document.fragment.find(" &#160;"),
               "entity expansion changed the word end offset");
    checkEqual(document.words[1].endPosition,
               document.fragment.find(" end."),
               "UTF-8 or numeric entity changed the word end offset");
    check(document.words[1].leadingSpace, "entity whitespace did not separate words");

    checkEqual(reader::rewritePaginationCss(
                   "/* display:flex */ .x{DISPLAY:FLEX;display:-WEBKIT-INLINE-FLEX;"
                   "xdisplay:flex;display:flexbox;content:'display:flex'}"),
               std::string("/* display:flex */ .x{DISPLAY:block;display:inline-block;"
                           "xdisplay:flex;display:flexbox;content:'display:flex'}"),
               "pagination rewrite changed a comment, string, or partial identifier");
    checkEqual(reader::rewriteCssUrls("a{background:URL( '../images/cover.jpg' )}",
                                      "OPS/Styles/main.css",
                                      [](const std::string& path) { return "resource:" + path; }),
               std::string("a{background:URL( 'resource:OPS/images/cover.jpg' )}"),
               "CSS URL rewrite changed quoting, case, or path resolution");
}

void testUnsafeArchiveEntry() {
    TemporaryFile fixture;
    Entries entries;
    entries.push_back(std::make_pair("mimetype", "application/epub+zip"));
    entries.push_back(std::make_pair("../escape", "bad"));
    writeZip(fixture.path(), entries);
    bool rejected = false;
    try {
        epub::Archive::open(fixture.path());
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidArchive;
    }
    check(rejected, "unsafe ZIP entry was not rejected");
}

void testAddressableElementLimit() {
    std::uint64_t nextAid = 0;
    const auto empty = reader::parseContentDocument(
        "chapter.xhtml",
        "<html><body/></html>",
        0,
        nextAid,
        [](const std::string&, const std::string&) { return std::string(); },
        [](const std::string&) { return std::string(); });
    checkEqual(empty.placeholderId, std::string("0"), "first placeholder did not start at zero");
    checkEqual(empty.fragment, std::string("<span/>"), "empty body lost its renderable fragment");
    checkEqual(empty.skeleton.substr(empty.fragmentInsertionOffset),
               std::string("</body></html>"),
               "empty body insertion offset changed");
    checkEqual(nextAid, std::uint64_t(1), "empty body consumed extra addressable IDs");

    nextAid = std::numeric_limits<std::uint64_t>::max();
    bool rejected = false;
    try {
        reader::parseContentDocument(
            "chapter.xhtml",
            "<html><body/></html>",
            0,
            nextAid,
            [](const std::string&, const std::string&) { return std::string(); },
            [](const std::string&) { return std::string(); });
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::LimitExceeded;
    }
    check(rejected, "addressable element IDs were allowed to wrap");
    checkEqual(nextAid,
               std::numeric_limits<std::uint64_t>::max(),
               "exhausted addressable element counter wrapped");
}

void testArchiveInputValidation() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub3Entries());
    bool rejected = false;
    try {
        epub::Archive::open(fixture.path() + std::string("\0ignored", sizeof("\0ignored") - 1));
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidPath;
    }
    check(rejected, "embedded NUL filename was silently truncated");

    epub::ArchiveLimits limits;
    limits.maxEntryBytes = std::numeric_limits<std::uint32_t>::max();
    rejected = false;
    try {
        epub::Archive::open(fixture.path(), limits);
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::LimitExceeded;
    }
    check(rejected, "archive limits allowed buffers too large for a 32-bit device");
}

void testZip64AndConcurrentReads() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub3Entries(), "deflate", true);
    std::ifstream input(fixture.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    check(bytes.find(std::string("PK\x06\x06", 4)) != std::string::npos,
          "fixture did not contain a ZIP64 end record");
    auto archive = epub::Archive::open(fixture.path());
    const epub::Archive copy = *archive;
    auto readOriginal = std::async(std::launch::async, [archive] {
        for (int read = 0; read < kConcurrentReadCount; ++read) {
            checkEqual(archive->readText("OPS/Images/cover art.jpg"),
                       std::string("JPEGDATA"),
                       "concurrent ZIP resource read failed");
        }
    });
    archive.reset();
    for (int read = 0; read < kConcurrentReadCount; ++read) {
        checkEqual(copy.readText("mimetype"),
                   std::string("application/epub+zip"),
                   "copied archive lost its backing ZIP");
    }
    readOriginal.get();
}

void testArchiveCorruptionAndDuplicates() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub3Entries());
    {
        std::fstream file(fixture.path(), std::ios::binary | std::ios::in | std::ios::out);
        const std::string bytes((std::istreambuf_iterator<char>(file)), {});
        const auto payload = bytes.find("JPEGDATA");
        check(payload != std::string::npos, "stored fixture payload was not found");
        file.clear();
        file.seekp(static_cast<std::streamoff>(payload));
        file.put('X');
        check(file.good(), "could not corrupt the ZIP fixture payload");
    }
    const auto archive = epub::Archive::open(fixture.path());
    bool rejected = false;
    try {
        archive->readText("OPS/Images/cover art.jpg");
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidArchive;
    }
    check(rejected, "ZIP CRC mismatch was not rejected");

    Entries entries{{"mimetype", "application/epub+zip"}, {"a/b", "one"}, {"a/./b", "two"}};
    writeZip(fixture.path(), entries);
    rejected = false;
    try {
        epub::Archive::open(fixture.path());
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidArchive;
    }
    check(rejected, "duplicate normalized ZIP paths were not rejected");
}

void testPackageInputValidation() {
    TemporaryFile fixture;
    Entries entries = epub3Entries();
    for (auto& entry : entries) {
        if (entry.first == "META-INF/container.xml") {
            entry.second.clear();
        }
    }
    writeZip(fixture.path(), entries);
    bool rejected = false;
    try {
        epub::Package::parse(*epub::Archive::open(fixture.path()));
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidContainer;
    }
    check(rejected, "empty container XML did not report a container error");

    entries = epub3Entries();
    for (auto& entry : entries) {
        if (entry.first == "OPS/package.opf") {
            const std::string type = "application/xhtml+xml";
            const auto chapter = entry.second.find("id=\"c1\"");
            const auto position = entry.second.find(type, chapter);
            check(position != std::string::npos, "fixture has no XHTML manifest item");
            entry.second.replace(position, type.size(), "application/octet-stream");
        }
    }
    writeZip(fixture.path(), entries);
    rejected = false;
    try {
        epub::Package::parse(*epub::Archive::open(fixture.path()));
    } catch (const epub::Error& error) {
        rejected = error.code() == epub::ErrorCode::InvalidDocument;
    }
    check(rejected, "unsupported spine content escaped the package boundary");
}

void testDeflatedArchive() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub3Entries(), "deflate");
    const std::shared_ptr<reader::Book> book = reader::Book::open(fixture.path());
    checkEqual(book->package().metadata().title,
               std::string("Direct EPUB"),
               "deflated EPUB title mismatch");

    std::string coverId;
    for (const auto& resource : book->resources()) {
        if (resource.second.mediaType == "image/jpeg") {
            coverId = resource.first;
            break;
        }
    }
    check(!coverId.empty(), "deflated EPUB cover was not indexed");
    const reader::ResourceData cover = book->readResource(coverId);
    checkEqual(std::string(cover.bytes.begin(), cover.bytes.end()),
               std::string("JPEGDATA"),
               "deflated EPUB resource mismatch");
}

void testEpub2Ncx() {
    TemporaryFile fixture;
    writeZip(fixture.path(), epub2Entries());
    const std::shared_ptr<reader::Book> book = reader::Book::open(fixture.path());
    checkEqual(
        book->package().metadata().title, std::string("EPUB Two"), "EPUB2 metadata title mismatch");
    checkEqual(book->package().coverPath(),
               std::string("OEBPS/cover.jpg"),
               "EPUB2 meta cover lookup failed");
    checkEqual(
        book->package().navigation().size(), std::size_t(1), "NCX root navigation count mismatch");
    checkEqual(
        book->package().navigation()[0].label, std::string("French Chapter"), "NCX label mismatch");
    checkEqual(book->package().navigation()[0].children.size(),
               std::size_t(1),
               "nested NCX point missing");
    const std::uint64_t nested = book->positionForPath("OEBPS/chapter.xhtml", "nested");
    check(nested < book->size(), "NCX target is outside the content model");
}

} // namespace

int main() {
    try {
        testPaths();
        testEpub3Model();
        testUnsafeArchiveEntry();
        testTextOffsetsAndCssBoundaries();
        testAddressableElementLimit();
        testArchiveInputValidation();
        testZip64AndConcurrentReads();
        testArchiveCorruptionAndDuplicates();
        testPackageInputValidation();
        testDeflatedArchive();
        testEpub2Ncx();
        std::cout << "epub_core_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "epub_core_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
