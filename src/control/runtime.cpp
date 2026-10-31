#include "control/runtime.hpp"
#include "control/system.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace epubctl {

const char kLibraryDirectory[] = "/var/run/eink-sdk/usr/lib/kindle-epub";
const char kDataDirectory[] = "/var/run/eink-sdk/usr/share/kindle-epub";
const char kLibexecDirectory[] = "/var/run/eink-sdk/usr/libexec/kindle-epub";
const char kStateDirectory[] = "/mnt/us/.eink-sdk/var/lib/kindle-epub";
const char kRunDirectory[] = "/var/run/kindle-epub";
const char kLockPath[] = "/var/run/kindle-epub.lock";

namespace {

using Records = std::map<std::string, std::string>;

Records parseRecords(const std::string& text, const std::set<std::string>& keys, bool quoted) {
    if (text.find('\0') != std::string::npos) {
        throw std::runtime_error("State data contains a NUL");
    }
    Records records;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || line.find('\r') != std::string::npos) {
            throw std::runtime_error("Malformed state record");
        }
        const std::string key = line.substr(0, separator);
        if (keys.count(key) == 0 || records.count(key) != 0) {
            throw std::runtime_error("Unknown or duplicate state field: " + key);
        }
        std::string value = line.substr(separator + 1);
        if (quoted) {
            if (value.size() < 2 || value.front() != '\'' || value.back() != '\'') {
                throw std::runtime_error("Malformed quoted state field: " + key);
            }
            value = value.substr(1, value.size() - 2);
        }
        if (value.find('\'') != std::string::npos) {
            throw std::runtime_error("Unexpected quote in state field: " + key);
        }
        records.emplace(key, value);
    }
    if (records.size() != keys.size()) {
        throw std::runtime_error("Incomplete state data");
    }
    return records;
}

bool pathCharacter(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '/' || character == '_' ||
           character == '.' || character == '-';
}

void requirePayloadPath(const std::string& path) {
    if (path.empty() || path.front() != '/' ||
        !std::all_of(path.begin(), path.end(), pathCharacter)) {
        throw std::runtime_error(
            "Payload path must be absolute and contain only letters, digits, /, _, . and -");
    }
}

void requireFile(const std::string& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) {
        throw std::runtime_error("Missing or empty runtime file: " + path);
    }
}

unsigned parseAbi(const std::string& value) {
    if (value != "0" && value != "1") {
        throw std::runtime_error("Unsupported reader string ABI");
    }
    return value == "0" ? 0U : 1U;
}

CatalogueBackend parseBackend(const std::string& value) {
    if (value == "java") {
        return CatalogueBackend::Java;
    }
    if (value == "plugin") {
        return CatalogueBackend::Plugin;
    }
    if (value == "worker") {
        return CatalogueBackend::Worker;
    }
    throw std::runtime_error("Unsupported catalogue backend");
}

JavaMode parseJavaMode(const std::string& value) {
    if (value == "none") {
        return JavaMode::None;
    }
    if (value == "classpath") {
        return JavaMode::Classpath;
    }
    if (value == "agent") {
        return JavaMode::Agent;
    }
    throw std::runtime_error("Unsupported Java activation mode");
}

void validateNativeSelection(const RuntimeSelection& selection) {
    static const std::set<std::string> supported = {"kindle5:k4:0:java",
                                                    "kindle5:kt1:0:plugin",
                                                    "kindle5:pw1:0:plugin",
                                                    "kindlepw2:mobi8-unsized:0:worker",
                                                    "kindlepw2:mobi8-unsized:1:worker",
                                                    "kindlehf:mobi8:0:worker",
                                                    "kindlehf:mobi8:1:worker"};
    const std::string combination = selection.architecture + ":" + selection.nativeProfile + ":" +
                                    std::to_string(selection.readerAbi) + ":" +
                                    catalogueName(selection.backend);
    if (supported.count(combination) == 0) {
        throw std::runtime_error("Package has no adapter for the detected native interfaces");
    }
}

void setNativePaths(RuntimeSelection& selection) {
    selection.nativeDir = selection.payload + "/native/" + selection.nativeProfile;
    selection.readerLibrary =
        selection.nativeDir + "/abi" + std::to_string(selection.readerAbi) + "/libkindle-epub.so";
}

const char* managerName(ServiceManager manager) {
    switch (manager) {
    case ServiceManager::Upstart:
        return "upstart";
    case ServiceManager::Pmond:
        return "pmond";
    }
    throw std::runtime_error("Unsupported service manager");
}

void validateSelection(const RuntimeSelection& selection) {
    requirePayloadPath(selection.payload);
    validateNativeSelection(selection);
    RuntimeSelection expected = selection;
    setNativePaths(expected);
    if (expected.nativeDir != selection.nativeDir ||
        expected.readerLibrary != selection.readerLibrary) {
        throw std::runtime_error("Selection contains inconsistent native paths");
    }
    if (!selection.kppLibrary.empty() &&
        selection.kppLibrary != selection.nativeDir + "/abi0/libkindle-epub-kpp.so" &&
        selection.kppLibrary != selection.nativeDir + "/abi1/libkindle-epub-kpp.so") {
        throw std::runtime_error("Selection contains an invalid KPP path");
    }
    if (selection.backend == CatalogueBackend::Java && selection.javaMode == JavaMode::None) {
        throw std::runtime_error("Java catalogue requires a Java runtime");
    }
    javaModeName(selection.javaMode);
    managerName(selection.manager);
}

int unhealthy(const char* component) {
    std::cout << "runtime=unhealthy (" << component << ")\n";
    return 1;
}

} // namespace

const char* catalogueName(CatalogueBackend backend) {
    switch (backend) {
    case CatalogueBackend::Java:
        return "java";
    case CatalogueBackend::Plugin:
        return "plugin";
    case CatalogueBackend::Worker:
        return "worker";
    }
    throw std::runtime_error("Unsupported catalogue backend");
}

const char* javaModeName(JavaMode mode) {
    switch (mode) {
    case JavaMode::None:
        return "none";
    case JavaMode::Classpath:
        return "classpath";
    case JavaMode::Agent:
        return "agent";
    }
    throw std::runtime_error("Unsupported Java activation mode");
}

RuntimeSelection validatePayload(const std::string& path) {
    requirePayloadPath(path);
    RuntimeSelection selection;
    selection.payload = canonicalPath(path);
    requirePayloadPath(selection.payload);
    const Records configuration =
        parseRecords(readText(std::string(kDataDirectory) + "/runtime.conf"),
                     {"epub_package_format", "epub_architecture"},
                     false);
    if (configuration.at("epub_package_format") != "2") {
        throw std::runtime_error("Unsupported runtime package format");
    }
    selection.architecture = configuration.at("epub_architecture");
    const bool hardFloat = isFile("/lib/ld-linux-armhf.so.3");
    if ((hardFloat && selection.architecture != "kindlehf") ||
        (!hardFloat && selection.architecture != "kindle5" &&
         selection.architecture != "kindlepw2")) {
        throw std::runtime_error("Package architecture does not match the installed userspace ABI");
    }
    const std::string probe = std::string(kLibexecDirectory) + "/kindle-epub-probe";
    const std::string contracts = std::string(kDataDirectory) + "/native-contracts.tsv";
    if (!executable(probe)) {
        throw std::runtime_error("Missing native capability probe");
    }
    requireFile(contracts);
    const Records detected =
        parseRecords(checkedCommand({probe, "--contracts", contracts}, "", {{"LD_PRELOAD", ""}}),
                     {"epub_native_profile", "epub_reader_abi", "epub_catalog_backend"},
                     false);
    selection.nativeProfile = detected.at("epub_native_profile");
    selection.readerAbi = parseAbi(detected.at("epub_reader_abi"));
    selection.backend = parseBackend(detected.at("epub_catalog_backend"));
    validateNativeSelection(selection);
    setNativePaths(selection);
    requireFile(selection.readerLibrary);
    if (!isFile("/etc/JAVA_FREE")) {
        requireFile(std::string(kDataDirectory) + "/kindle-epub-java.jar");
        for (const char* name : {"libkindle-epub-framework-exec.so",
                                 "libkindle-epub-java-agent.so"}) {
            requireFile(selection.payload + "/" + name);
        }
    }
    if (selection.backend == CatalogueBackend::Plugin) {
        requireFile(selection.nativeDir + "/libkindle-epub-extractor.so");
    }
    if (selection.backend == CatalogueBackend::Worker) {
        const std::string worker = std::string(kLibexecDirectory) + "/kindle-epub-import";
        if (!executable(worker) || !isFile("/usr/lib/ccat/libEBridge.so")) {
            throw std::runtime_error("Missing catalogue worker or stock bridge");
        }
        checkedCommand({worker, "--check"}, "", {{"LD_PRELOAD", ""}});
    }
    return selection;
}

RuntimeSelection readSelection() {
    const Records records = parseRecords(readText(std::string(kRunDirectory) + "/selection"),
                                         {"epub_payload",
                                          "epub_architecture",
                                          "epub_native_profile",
                                          "epub_reader_abi",
                                          "epub_lifecycle",
                                          "epub_catalog_backend",
                                          "epub_reader",
                                          "epub_kpp",
                                          "epub_webreader_job",
                                          "epub_java_mode"},
                                         true);
    RuntimeSelection selection;
    selection.payload = records.at("epub_payload");
    selection.architecture = records.at("epub_architecture");
    selection.nativeProfile = records.at("epub_native_profile");
    selection.readerAbi = parseAbi(records.at("epub_reader_abi"));
    selection.backend = parseBackend(records.at("epub_catalog_backend"));
    selection.javaMode = parseJavaMode(records.at("epub_java_mode"));
    const std::string& manager = records.at("epub_lifecycle");
    if (manager != "upstart" && manager != "pmond") {
        throw std::runtime_error("Unsupported service manager in selection");
    }
    selection.manager = manager == "upstart" ? ServiceManager::Upstart : ServiceManager::Pmond;
    const std::string& webreader = records.at("epub_webreader_job");
    if (webreader != "0" && webreader != "1") {
        throw std::runtime_error("Invalid WebReader selection");
    }
    selection.hasWebreader = webreader == "1";
    setNativePaths(selection);
    selection.readerLibrary = records.at("epub_reader");
    selection.kppLibrary = records.at("epub_kpp");
    validateSelection(selection);
    return selection;
}

void writeSelection(const RuntimeSelection& selection) {
    validateSelection(selection);
    std::ostringstream output;
    output << "epub_payload='" << selection.payload << "'\n"
           << "epub_architecture='" << selection.architecture << "'\n"
           << "epub_native_profile='" << selection.nativeProfile << "'\n"
           << "epub_reader_abi='" << selection.readerAbi << "'\n"
           << "epub_lifecycle='" << managerName(selection.manager) << "'\n"
           << "epub_catalog_backend='" << catalogueName(selection.backend) << "'\n"
           << "epub_reader='" << selection.readerLibrary << "'\n"
           << "epub_kpp='" << selection.kppLibrary << "'\n"
           << "epub_webreader_job='" << (selection.hasWebreader ? 1 : 0) << "'\n"
           << "epub_java_mode='" << javaModeName(selection.javaMode) << "'\n";
    writeText(std::string(kRunDirectory) + "/selection", output.str());
}

bool jobExists(const std::string& job) {
    return isFile("/etc/upstart/" + job + ".conf");
}

pid_t jobPid(const std::string& job) {
    const CommandResult result = runCommand({"initctl", "status", job});
    const std::size_t marker = result.output.rfind("process ");
    if (result.status != 0 || marker == std::string::npos) {
        return 0;
    }
    const std::string prefix = "process ";
    return parsePid(result.output.substr(marker + prefix.size()));
}

// The service is restricted to two known names before it becomes a marker path.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool hookReady(const std::string& job, const std::string& library) {
    std::vector<std::string> names;
    if (job == "webreader") {
        names = {"webreader"};
    } else if (job == "kppmainapp") {
        names = {"KPPMainAppV2", "KPPMainApp"};
    } else {
        throw std::invalid_argument("Unknown native hook service");
    }
    const pid_t confirmed =
        parsePid(readText(std::string(kRunDirectory) + "/" + job + ".hook-ready", true));
    if (confirmed == 0) {
        return false;
    }
    const std::vector<pid_t> processes = processPids(names);
    return std::find(processes.begin(), processes.end(), confirmed) != processes.end() &&
           processMaps(confirmed, library);
}

int printStatus() {
    const std::string runtime(kRunDirectory);
    if (!isFile(runtime + "/active")) {
        if (isFile(runtime + "/selection") ||
            isDirectory(std::string(kStateDirectory) + "/activation")) {
            std::cout << "runtime=recovery-required; run epubctl stop or start\n";
            return 1;
        }
        std::cout << "runtime=stopped\n";
        return 0;
    }
    const RuntimeSelection selection = readSelection();
    if (selection.javaMode != JavaMode::None && !mapped({"cvm", "java"}, selection.readerLibrary)) {
        return unhealthy("Java");
    }
    if (selection.javaMode == JavaMode::Agent &&
        (!isFile(runtime + "/prepare/java/agent-applied") ||
         isFile(runtime + "/prepare/java/agent-rejected"))) {
        return unhealthy("Java adaptation");
    }
    if (selection.hasWebreader && !mapped({"webreader"}, selection.readerLibrary)) {
        return unhealthy("WebReader");
    }
    if (selection.nativeProfile == "k4" && !hookReady("webreader", selection.readerLibrary)) {
        return unhealthy("WebReader hooks");
    }
    if (!selection.kppLibrary.empty() && !hookReady("kppmainapp", selection.kppLibrary)) {
        return unhealthy("KPP hooks");
    }
    if (selection.backend == CatalogueBackend::Worker) {
        const CommandResult ready =
            runCommand({"lipc-get-prop", "org.kindle.epub.scanner", "ready"});
        if (ready.status != 0 || trim(ready.output) != "1") {
            return unhealthy("catalogue");
        }
    }
    std::cout << "runtime=active native=" << selection.nativeProfile
              << " abi=" << selection.readerAbi << " java=" << javaModeName(selection.javaMode)
              << " catalogue=" << catalogueName(selection.backend) << '\n';
    return 0;
}

} // namespace epubctl
