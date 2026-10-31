#include "elf_image.hpp"
#include "kpp/discovery.hpp"
#include "mobi8/k4_discovery.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cxxabi.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

namespace {
using kindle_epub::ElfImage;
using Classes = std::map<std::string, std::vector<std::string>>;
using Contracts = std::map<std::string, Classes>;
const char* factory =
    "_ZN8Mobi8SDK18BookManagerFactory14getBookManagerERNS_10ManagedPtrINS_12IBookManagerEEEPKh";
const char* authorsOld = "_ZNK8Mobi8SDK12BookMetadata10getAuthorsERSt6vectorISsSaISsEE";
const char* authorsNew = "_ZNK8Mobi8SDK12BookMetadata10getAuthorsERSt6vectorINSt7__cxx1112basic_"
                         "stringIcSt11char_traitsIcESaIcEEESaIS7_EE";

bool exists(const std::string& path) {
    return access(path.c_str(), R_OK) == 0;
}
void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

Contracts readContracts(const std::string& path) {
    std::ifstream file(path.c_str());
    require(file.good(), "Cannot read native method contracts");
    Contracts contracts;
    std::string line;
    constexpr unsigned maxContractLines = 1024;
    constexpr std::size_t maxContractLineLength = 1024;
    constexpr std::size_t minContractClasses = 10;
    unsigned count = 0;
    while (std::getline(file, line)) {
        require(++count < maxContractLines && line.size() < maxContractLineLength,
                "Oversized native contract");
        const std::size_t first = line.find('\t');
        const std::size_t second = line.find('\t', first == std::string::npos ? 0 : first + 1);
        require(first != std::string::npos && second != std::string::npos &&
                    second + 1 < line.size(),
                "Malformed native contract");
        const std::string profile = line.substr(0, first);
        require(profile == "early" || profile == "mobi8", "Unknown native contract");
        contracts[profile][line.substr(first + 1, second - first - 1)].push_back(
            line.substr(second + 1));
    }
    require(file.eof() && contracts.size() == 2 &&
                contracts["early"].size() >= minContractClasses &&
                contracts["mobi8"].size() >= minContractClasses,
            "Incomplete native contracts");
    return contracts;
}

void replaceAll(std::string& text, const std::string& from, const std::string& replacement) {
    for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos;
         at += replacement.size()) {
        text.replace(at, from.size(), replacement);
    }
}
std::string normalized(const std::string& mangled) {
    int status = 0;
    const std::unique_ptr<char, decltype(&std::free)> raw(
        abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status), &std::free);
    if (!raw) {
        return mangled;
    }
    std::string value(raw.get());
    replaceAll(value, "std::__cxx11::", "std::");
    replaceAll(value, " ", "");
    replaceAll(value,
               "std::basic_string<char,std::char_traits<char>,std::allocator<char>>",
               "std::string");
    return value;
}

bool hiddenIteratorType(const ElfImage& image, std::uint32_t slot) {
    const std::uint32_t address = image.word(slot);
    // Some stock libraries hide this final method. Accept the known leaf
    // bodies returning iterator type 1, never an arbitrary unnamed function.
    const std::uint32_t framed[] = {0xe1a0c00d, 0xe3a00001, 0xe92dd800, 0xe24cb004, 0xe89da800};
    const std::uint32_t arm[] = {0xe3a00001, 0xe12fff1e};
    const std::uint16_t thumb[] = {0x2001, 0x4770};
    if ((address & 1) != 0) {
        return std::memcmp(image.at(address & ~1U, sizeof(thumb), true), thumb, sizeof(thumb)) == 0;
    }
    return std::memcmp(image.at(address, sizeof(arm), true), arm, sizeof(arm)) == 0 ||
           std::memcmp(image.at(address, sizeof(framed), true), framed, sizeof(framed)) == 0;
}

bool validateVtables(const ElfImage& image, const Classes& classes, std::string* failure) {
    constexpr std::size_t vtablePrefixBytes = 16;
    for (const auto& contract : classes) {
        std::ostringstream name;
        name << "_ZTVN8Mobi8SDK" << contract.first.size() << contract.first << 'E';
        Elf32_Sym table;
        if (!image.symbol(name.str(), &table) ||
            table.st_size != vtablePrefixBytes + sizeof(std::uint32_t) * contract.second.size()) {
            *failure = "vtable shape: " + contract.first;
            return false;
        }
        for (std::size_t slot = 0; slot < contract.second.size(); ++slot) {
            const std::uint32_t address =
                table.st_value + vtablePrefixBytes + slot * sizeof(std::uint32_t);
            const std::vector<std::string> actual = image.slotNames(address);
            const std::string expected = normalized(contract.second[slot]);
            bool match = std::any_of(actual.begin(), actual.end(), [&](const std::string& method) {
                return normalized(method) == expected;
            });
            if (!match && actual.empty() && contract.first == "Mobi8WordIterator" && slot == 4) {
                match = hiddenIteratorType(image, address);
            }
            if (!match) {
                std::ostringstream reason;
                reason << contract.first << " slot " << slot + 2 << ": "
                       << (actual.empty() ? "unnamed method" : actual[0]);
                *failure = reason.str();
                return false;
            }
        }
    }
    return true;
}

void requireSymbol(const ElfImage& image, const char* name, bool defined = true) {
    Elf32_Sym symbol;
    require(image.symbol(name, &symbol, defined),
            std::string(defined ? "Missing native export: " : "Missing native import: ") + name);
}

std::string libraryPath(const std::string& root, const char* versioned, const char* linker) {
    const std::string path = root + versioned;
    return exists(path) ? path : root + linker;
}
void probeKpp(const ElfImage& kpp) {
    std::vector<kindle_epub::kpp::ImageRange> image;
    const std::vector<Elf32_Phdr>& segments = kpp.loadSegments();
    for (const auto& segment : segments) {
        if (segment.p_type == PT_LOAD && segment.p_filesz != 0) {
            kindle_epub::kpp::ImageRange range = {segment.p_vaddr,
                                                  segment.p_filesz,
                                                  (segment.p_flags & PF_X) != 0,
                                                  kpp.span(segment.p_offset, segment.p_filesz)};
            image.push_back(range);
        }
    }
    const kindle_epub::kpp::HookSites sites = kindle_epub::kpp::discover(image);
    require(sites.extensionCount == 1 && sites.factoryCount == 1 && sites.stringAbi <= 1,
            "KPP native hooks are unsupported or ambiguous");
    const std::size_t trampolineSize = kindle_epub::kpp::kTrampolineSize;
    alignas(4) unsigned char trampoline[2 * trampolineSize];
    require(kindle_epub::kpp::makeTrampoline(
                image, sites.factory, false, sites.stringAbi, trampoline) != 0 &&
                kindle_epub::kpp::makeTrampoline(
                    image, sites.extension, true, sites.stringAbi, trampoline + trampolineSize) !=
                    0,
            "KPP native hooks cannot be safely relocated");
    std::printf("epub_kpp_abi=%u\n", sites.stringAbi);
}

void validateScanner(const std::string& root, bool classic) {
    const ElfImage scanner(libraryPath(root, "/usr/lib/libscanner.so.1", "/usr/lib/libscanner.so"));
    requireSymbol(scanner, "scanner_gen_uuid");
    requireSymbol(scanner, "scanner_post_string");
    requireSymbol(scanner, "scanner_delete_ccat_entry");
    if (classic) {
        const ElfImage extractor(
            libraryPath(root, "/usr/lib/libextractor_util.so.1", "/usr/lib/libextractor_util.so"));
        requireSymbol(extractor, "construct_thumbnail_uri");
        requireSymbol(extractor, "should_remove_thumbnail_on_delete_entry");
        requireSymbol(extractor, "extractor_get_thumbnail_for_uuid");
    }
}

void probeMobi(const std::string& root, const Contracts& contracts) {
    const bool javaFree = exists(root + "/etc/JAVA_FREE");
    require(!(exists(root + "/usr/lib/libMobi.so.1") && exists(root + "/usr/lib/libMobi.so.0")),
            "Multiple Mobi8 library generations are installed");
    std::string mobiPath;
    const char* candidates[] = {
        "/usr/lib/libMobi.so.1", "/usr/lib/libMobi.so.0", "/usr/lib/libMobi.so"};
    for (const char* candidate : candidates) {
        if (exists(root + candidate)) {
            mobiPath = candidate;
            break;
        }
    }
    const bool embedded = mobiPath.empty();
    if (embedded) {
        mobiPath = "/usr/java/lib/libmobi8sdkjni.so";
    }
    const ElfImage mobi(root + mobiPath);
    const std::string soname = mobi.soname();
    const bool modern = !embedded && soname == "libMobi.so.1";
    const bool classic = !embedded && (soname == "libMobi.so.0" || soname == "libMobi.so");
    require(embedded || modern || classic, "Unsupported Mobi8 SONAME: " + soname);
    requireSymbol(mobi, factory);
    Elf32_Sym symbol;
    const bool oldStrings = mobi.symbol(authorsOld, &symbol);
    const bool newStrings = mobi.symbol(authorsNew, &symbol);
    require(oldStrings != newStrings, "Unknown or ambiguous Mobi8 string ABI");
    std::string earlyFailure;
    std::string modernFailure;
    const bool earlySlots = validateVtables(mobi, contracts.find("early")->second, &earlyFailure);
    const bool modernSlots = validateVtables(mobi, contracts.find("mobi8")->second, &modernFailure);
    require(earlySlots != modernSlots,
            "Unsupported Mobi8 interfaces (" + earlyFailure + "; " + modernFailure + ")");
    require(!embedded || (earlySlots && oldStrings && !mobi.hardFloat()),
            "Unsupported embedded Mobi8 ABI");
    require(!classic || (oldStrings && !mobi.hardFloat()), "Unsupported classic Mobi8 ABI");
    require(!modern || modernSlots, "Unsupported extended Mobi8 ABI");
    require(!javaFree || modern, "JAVA_FREE requires the native reader interfaces");
    if (embedded) {
        const ElfImage webreader(root + "/usr/bin/webreader");
        require(!webreader.hardFloat(), "Embedded reader uses an incompatible calling convention");
        kindle_epub::discoverK4Factory(webreader);
        std::string error;
        require(validateVtables(webreader, contracts.find("early")->second, &error),
                "Embedded reader interfaces: " + error);
    } else {
        if (!javaFree) {
            const ElfImage webreader(root + "/usr/bin/webreader");
            require(webreader.hardFloat() == mobi.hardFloat(),
                    "Reader and Mobi8 use different calling conventions");
            requireSymbol(webreader, factory, false);
            requireSymbol(webreader, "g_str_has_suffix", false);
            const ElfImage jni(root + "/usr/java/lib/libmobi8sdkjni.so");
            requireSymbol(jni, factory, false);
        }
        validateScanner(root, classic);
    }
    const char* profile = "k4";
    const char* catalogBackend = "java";
    if (classic) {
        profile = earlySlots ? "kt1" : "pw1";
        catalogBackend = "plugin";
    } else if (modern) {
        profile = mobi.hardFloat() ? "mobi8" : "mobi8-unsized";
        catalogBackend = "worker";
    }
    std::printf("epub_native_profile=%s\nepub_reader_abi=%u\nepub_catalog_backend=%s\n",
                profile,
                static_cast<unsigned>(newStrings),
                catalogBackend);
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::string root;
        std::string contractFile;
        std::string kppFile;
        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);
            require(i + 1 < argc &&
                        (option == "--root" || option == "--contracts" || option == "--kpp"),
                    "Usage: kindle-epub-probe (--contracts FILE | --kpp ELF) [--root ROOTFS]");
            const char* value = argv[++i];
            if (option == "--root") {
                root = value;
            } else if (option == "--kpp") {
                kppFile = value;
            } else {
                contractFile = value;
            }
        }
        while (!root.empty() && root[root.size() - 1] == '/') {
            root.erase(root.size() - 1);
        }
        if (!kppFile.empty()) {
            require(contractFile.empty(), "Choose one probe mode");
            probeKpp(ElfImage(root + kppFile));
            return 0;
        }
        require(!contractFile.empty(), "Native contracts path is required");
        probeMobi(root, readContracts(contractFile));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Native capability check failed: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "Native capability check failed unexpectedly\n");
        return 1;
    }
}
