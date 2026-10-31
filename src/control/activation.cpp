#include "activation.hpp"
#include "system.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <pwd.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace epubctl {
namespace {

constexpr unsigned kOwnerTimeout = 30;
constexpr unsigned kKppTimeout = 60;
constexpr unsigned kReaderTimeout = 90;
constexpr unsigned kFrameworkTimeout = 120;
constexpr mode_t kPublicDirectoryMode = 0755;
constexpr mode_t kPrepareDirectoryMode = 0777;
constexpr mode_t kHookFileMode = 0600;
const char kFrameworkPolicy[] = "/opt/amazon/ebook/security/framework.policy";
const char kPmondWebreader[] = "/etc/kdb.src/yoshi/system/daemon/pmond/webreader";
const char kWebreaderCommand[] = "system/daemon/pmond/webreader/start_cmd";
const std::array<const char*, 4> kJobs{{"framework", "webreader", "kppmainapp", "appmgrd"}};
const std::array<const char*, 2> kRoutingToggles{
    {"/var/local/ENABLE_KPPREADER", "/var/local/kft/field_test_toggles/LegacyFormatMigration"}};

std::string runPath(const std::string& name) {
    return std::string(kRunDirectory) + '/' + name;
}

bool nonemptyFile(const std::string& path) {
    struct stat information {};
    return stat(path.c_str(), &information) == 0 && S_ISREG(information.st_mode) &&
           information.st_size > 0;
}

void waitUntil(unsigned timeout, const std::string& error, const std::function<bool()>& ready) {
    for (unsigned elapsed = 0; elapsed <= timeout; ++elapsed) {
        if (ready()) {
            return;
        }
        if (elapsed != timeout) {
            sleepSeconds(1);
        }
    }
    throw std::runtime_error(error);
}

bool jobRunning(const std::string& job) {
    const auto result = runCommand({"initctl", "status", job});
    return result.status == 0 && result.output.find("start/running") != std::string::npos;
}

void waitJob(const std::string& job) {
    waitUntil(kFrameworkTimeout, "Stock service did not start: " + job, [&job] {
        return jobRunning(job);
    });
}

void selectKpp(RuntimeSelection& selection) {
    if (!jobExists("kppmainapp")) {
        return;
    }
    std::string readerExecutable;
    for (unsigned elapsed = 0; elapsed < kKppTimeout; ++elapsed) {
        const auto readers = processPids({"KPPMainAppV2", "KPPMainApp"});
        if (readers.size() > 1) {
            throw std::runtime_error("More than one KPP reader is running.");
        }
        if (!readers.empty()) {
            readerExecutable = processExecutable(readers.front());
            break;
        }
        sleepSeconds(1);
    }
    if (!readerExecutable.empty()) {
        const auto result =
            runCommand({selection.payload + "/kindle-epub-probe", "--kpp", readerExecutable},
                       {},
                       {{"LD_PRELOAD", ""}});
        if (result.status == 0) {
            const auto value = trim(result.output);
            if (value != "epub_kpp_abi=0" && value != "epub_kpp_abi=1") {
                throw std::runtime_error("Invalid KPP native probe result.");
            }
            selection.kppLibrary =
                selection.nativeDir + "/abi" + value.back() + "/libkindle-epub-kpp.so";
            if (!nonemptyFile(selection.kppLibrary)) {
                throw std::runtime_error("Missing " + selection.kppLibrary);
            }
            return;
        }
    }
    for (const auto* toggle : kRoutingToggles) {
        if (exists(toggle)) {
            throw std::runtime_error(
                "Native reader routing is already enabled but its KPP interfaces are "
                "unsupported.");
        }
    }
}

RuntimeSelection selectRuntime(const std::string& payload) {
    auto selection = validatePayload(payload);
    const bool javaFree = isFile("/etc/JAVA_FREE");
    if (jobExists("framework") || (javaFree && jobExists("kppmainapp"))) {
        selection.manager = ServiceManager::Upstart;
    } else if (executable("/etc/init.d/framework") && executable("/etc/init.d/pmond") &&
               isDirectory(kPmondWebreader) && access(kFrameworkPolicy, R_OK) == 0) {
        selection.manager = ServiceManager::Pmond;
        selection.hasWebreader = true;
    } else {
        throw std::runtime_error("No supported framework and reader supervision interface.");
    }
    selection.hasWebreader = selection.hasWebreader || jobExists("webreader");
    selectKpp(selection);
    if (!javaFree) {
        selection.javaExecutable =
            executable("/usr/java/bin/cvm") ? "/usr/java/bin/cvm" : "/usr/java/bin/java";
        if (!executable(selection.javaExecutable)) {
            throw std::runtime_error("Java firmware has no supported VM.");
        }
        selection.javaMode = JavaMode::Classpath;
    } else if (selection.kppLibrary.empty()) {
        throw std::runtime_error("JAVA_FREE firmware requires the native KPP reader.");
    } else if (selection.backend == CatalogueBackend::Java) {
        throw std::runtime_error("JAVA_FREE firmware cannot use the Java catalogue.");
    }
    selectCatalogue(selection);
    return selection;
}

void prepareJava(RuntimeSelection& selection) {
    if (selection.javaMode == JavaMode::None) {
        return;
    }
    if (selection.manager == ServiceManager::Pmond) {
        // The stock launcher uses pidof while publishing cvm.pid. Discovery must wait for it.
        waitUntil(kOwnerTimeout, "Stock Java launcher did not publish its VM PID.", [] {
            return processAlive(parsePid(readText("/var/run/cvm.pid", true)));
        });
    }
    ensureDirectory(runPath("prepare"), kPrepareDirectoryMode);
    checkedCommand({"chmod", "0777", runPath("prepare")});
    const auto jar = selection.payload + "/kindle-epub-java.jar";
    const auto discovery = checkedCommand({selection.javaExecutable,
                                           "-Xmx32m",
                                           "-cp",
                                           jar,
                                           "org.katadelos.epub.runtime.Main",
                                           "prepare",
                                           runPath("prepare/java"),
                                           runPath("prepare/discovery.log"),
                                           "/opt/amazon/ebook/lib"},
                                          {},
                                          {{"LD_PRELOAD", ""}});
    std::cout << discovery;
    if (!nonemptyFile(runPath("prepare/java/replacement.class"))) {
        throw std::runtime_error("Java discovery did not produce a replacement.");
    }
    const auto probe =
        runCommand({selection.javaExecutable,
                    "-agentpath:" + selection.payload +
                        "/libkindle-epub-java-agent.so=probe:" + runPath("prepare/probe"),
                    "-Xmx16m",
                    "-cp",
                    jar,
                    "org.katadelos.epub.runtime.Probe"},
                   {},
                   {{"LD_PRELOAD", ""}});
    writeText(runPath("prepare/probe.log"), probe.output);
    if (probe.status == 0 && nonemptyFile(runPath("prepare/probe"))) {
        selection.javaMode = JavaMode::Agent;
    }
    checkedCommand({"chmod", "-R", "a+rX", runPath("prepare")});
    checkedCommand({"chmod", "0755", runPath("prepare")});
    if (selection.javaMode == JavaMode::Agent) {
        const auto* account = getpwnam("framework");
        if (account == nullptr ||
            chown(runPath("prepare/java").c_str(), account->pw_uid, static_cast<gid_t>(-1)) != 0) {
            throw std::runtime_error(
                "Cannot give the framework access to Java adaptation markers.");
        }
    }
}

pid_t servicePid(const std::string& job) {
    if (job == "framework") {
        const auto virtualMachines = processPids({"cvm", "java"});
        if (!virtualMachines.empty()) {
            return virtualMachines.front();
        }
    }
    if (jobExists(job)) {
        const auto process = jobPid(job);
        if (process > 0) {
            return process;
        }
    }
    if (job == "webreader") {
        const auto readers = processPids({"webreader"});
        if (!readers.empty()) {
            return readers.front();
        }
    }
    return 0;
}

void snapshotProcesses() {
    for (const auto* job : kJobs) {
        const auto process = servicePid(job);
        const auto preload = process > 0 ? processEnvironment(process, "LD_PRELOAD") : "";
        writeText(runPath(std::string(job) + ".preload"), preload + '\n');
    }
}

void snapshotToggles(const RuntimeSelection& selection) {
    if (selection.kppLibrary.empty()) {
        return;
    }
    const auto marker = std::string(kStateDirectory) + "/activation/created-toggles";
    auto created = readText(marker, true);
    for (const auto* toggle : kRoutingToggles) {
        if (!exists(toggle)) {
            created += std::string(toggle) + '\n';
            writeText(marker, created);
            const std::string path(toggle);
            ensureDirectory(path.substr(0, path.find_last_of('/')));
            writeText(path, "");
        }
    }
}

void restoreToggles() {
    const auto marker = std::string(kStateDirectory) + "/activation/created-toggles";
    std::istringstream created(readText(marker, true));
    std::string path;
    while (std::getline(created, path)) {
        if (std::find(kRoutingToggles.begin(), kRoutingToggles.end(), path) ==
            kRoutingToggles.end()) {
            throw std::runtime_error("Invalid native routing recovery state.");
        }
        if (isFile(path) && !nonemptyFile(path) && !isSymlink(path)) {
            removeFile(path);
        }
    }
    removeFile(marker);
}

std::string preload(const std::string& library, const std::string& oldValue) {
    return oldValue.empty() ? library : library + ':' + oldValue;
}

std::map<std::string, std::string> frameworkEnvironment(const RuntimeSelection& selection) {
    const auto libraries =
        preload(selection.payload + "/libkindle-epub-framework-exec.so:" + selection.readerLibrary,
                trim(readText(runPath("framework.preload"))));
    std::map<std::string, std::string> environment{{"LD_PRELOAD", libraries},
                                                   {"KINDLE_EPUB_FRAMEWORK_PRELOAD", libraries},
                                                   {"KINDLE_EPUB_JAVA_CLASSPATH", ""},
                                                   {"KINDLE_EPUB_JAVA_BOOTCLASSPATH", ""},
                                                   {"KINDLE_EPUB_JAVA_AGENT", ""}};
    if (selection.javaMode == JavaMode::Classpath) {
        environment["KINDLE_EPUB_JAVA_CLASSPATH"] = runPath("prepare/java/shadow");
    } else if (selection.javaMode == JavaMode::Agent) {
        environment["KINDLE_EPUB_JAVA_BOOTCLASSPATH"] = runPath("prepare/java/helper");
        environment["KINDLE_EPUB_JAVA_AGENT"] =
            selection.payload + "/libkindle-epub-java-agent.so=replace:" + runPath("prepare/java");
    }
    return environment;
}

void waitFramework() {
    waitUntil(kFrameworkTimeout, "Framework did not become ready.", [] {
        const auto result = runCommand({"lipc-get-prop", "com.lab126.kaf", "frameworkStarted"});
        return result.status == 0 && trim(result.output) == "1";
    });
}

void pauseAppmgr(const RuntimeSelection& selection) {
    if (!selection.kppLibrary.empty() && jobExists("appmgrd") && jobRunning("appmgrd")) {
        // KPP's stock post-stop hook reports intentional exits as crashes.
        writeText(runPath("appmgrd-paused"), "");
        checkedCommand({"initctl", "stop", "appmgrd"});
    }
}

void resumeAppmgr(const std::string& savedPreload) {
    if (!isFile(runPath("appmgrd-paused"))) {
        return;
    }
    if (!jobRunning("appmgrd")) {
        checkedCommand(
            {"initctl", "start", "appmgrd", "LD_PRELOAD=" + trim(readText(savedPreload))});
    }
    waitUntil(kKppTimeout, "KPP application service did not become ready.", [] {
        const auto result = runCommand({"lipc-probe", "com.lab126.KPPMainApp"});
        std::istringstream lines(result.output);
        std::string line;
        while (std::getline(lines, line)) {
            const auto end = line.find_last_not_of(" \t\r");
            if (end != std::string::npos && end >= 2 && line.compare(end - 1, 2, "go") == 0 &&
                (line[end - 2] == ' ' || line[end - 2] == '\t')) {
                return result.status == 0;
            }
        }
        return false;
    });
    checkedCommand({"lipc-set-prop",
                    "com.lab126.appmgrd",
                    "start",
                    "app://com.lab126.KPPMainApp/?view=KPP_HOME"});
    removeFile(runPath("appmgrd-paused"));
}

void prepareHook(const std::string& job) {
    const std::vector<std::string> names =
        job == "webreader" ? std::vector<std::string>{"webreader"}
                           : std::vector<std::string>{"KPPMainAppV2", "KPPMainApp"};
    uid_t owner = 0;
    waitUntil(
        kOwnerTimeout, "Cannot identify the stock " + job + " service owner.", [&names, &owner] {
            const auto processes = processPids(names);
            if (processes.empty()) {
                return false;
            }
            std::istringstream status(
                readText("/proc/" + std::to_string(processes.front()) + "/status", true));
            std::string line;
            while (std::getline(status, line)) {
                std::istringstream fields(line);
                std::string name;
                uid_t real = 0;
                uid_t effective = 0;
                uid_t saved = 0;
                if ((fields >> name) && name == "Uid:" &&
                    (fields >> real >> effective >> saved >> owner)) {
                    return true;
                }
            }
            return false;
        });
    const auto marker = runPath(job + ".hook-ready");
    writeText(marker, "", kHookFileMode);
    if (chown(marker.c_str(), owner, static_cast<gid_t>(-1)) != 0) {
        throw std::runtime_error("Cannot prepare native hook marker: " +
                                 std::string(std::strerror(errno)));
    }
}

void stopPmondReader() {
    checkedCommand({"lipc-set-prop", "com.lab126.pmond", "stop", "webreader"});
    waitUntil(kOwnerTimeout, "WebReader did not stop.", [] {
        return processPids({"webreader"}).empty();
    });
}

void activatePmond(const RuntimeSelection& selection) {
    if (!isFile(kFrameworkPolicy) || !isDirectory(kPmondWebreader)) {
        throw std::runtime_error("Unknown policy or pmond supervision layout.");
    }
    writeText(runPath("framework.policy"),
              readText(kFrameworkPolicy) +
                  "\ngrant codeBase \"file:" + runPath("prepare/java/shadow/-") +
                  "\" {\n    permission java.security.AllPermission;\n};\n");
    ensureDirectory(runPath("pmond-webreader"));
    checkedCommand(
        {"cp", "-a", std::string(kPmondWebreader) + "/.", runPath("pmond-webreader") + '/'});
    const auto stockCommand = trim(checkedCommand({"kdb", "get", kWebreaderCommand}));
    writeText(runPath("webreader-command"), stockCommand + '\n');
    std::string command = "/usr/bin/env ";
    if (selection.nativeProfile == "k4") {
        prepareHook("webreader");
        command += "KINDLE_EPUB_HOOK_READY=" + runPath("webreader.hook-ready") + ' ';
    }
    const auto libraries =
        preload(selection.readerLibrary, trim(readText(runPath("webreader.preload"))));
    // The stock supervisor stores a command string. Quote only the injected environment value.
    std::string quoted = "'";
    for (const char character : libraries) {
        quoted += character == '\'' ? "'\\''" : std::string(1, character);
    }
    command += "LD_PRELOAD=" + quoted + "' " + stockCommand;
    checkedCommand({"/etc/init.d/framework", "stop"});
    stopPmondReader();
    checkedCommand({"/etc/init.d/pmond", "stop"});
    checkedCommand({"mount", "--bind", runPath("framework.policy"), kFrameworkPolicy});
    writeText(runPath("policy-mounted"), "");
    checkedCommand({"mount", "--bind", runPath("pmond-webreader"), kPmondWebreader});
    writeText(runPath("pmond-mounted"), "");
    checkedCommand({"kdb", "set", kWebreaderCommand, command});
    checkedCommand({"/etc/init.d/pmond", "start"});
    checkedCommand({"/etc/init.d/framework", "start"}, {}, frameworkEnvironment(selection));
}

void activateUpstart(const RuntimeSelection& selection) {
    pauseAppmgr(selection);
    if (selection.javaMode != JavaMode::None) {
        waitJob("framework");
        writeText(runPath("framework-restarted"), "");
        std::vector<std::string> restart{"initctl", "restart", "framework"};
        for (const auto& variable : frameworkEnvironment(selection)) {
            restart.push_back(variable.first + '=' + variable.second);
        }
        checkedCommand(restart);
        if (!selection.kppLibrary.empty()) {
            waitFramework();
        }
    }
    for (const auto* jobName : {"webreader", "kppmainapp"}) {
        const std::string job(jobName);
        const bool webreader = job == "webreader";
        if ((webreader && !selection.hasWebreader) ||
            (!webreader && selection.kppLibrary.empty())) {
            continue;
        }
        waitJob(job);
        std::string hookMarker;
        if (!webreader || selection.nativeProfile == "k4") {
            prepareHook(job);
            hookMarker = runPath(job + ".hook-ready");
        }
        writeText(runPath(job + "-restarted"), "");
        const auto& library = webreader ? selection.readerLibrary : selection.kppLibrary;
        checkedCommand({"initctl",
                        "restart",
                        job,
                        "LD_PRELOAD=" + preload(library, trim(readText(runPath(job + ".preload")))),
                        "KINDLE_EPUB_HOOK_READY=" + hookMarker});
    }
    if (selection.javaMode == JavaMode::None && jobExists("scanner")) {
        waitJob("scanner");
        checkedCommand({"initctl", "restart", "scanner"});
    }
    resumeAppmgr(runPath("appmgrd.preload"));
}

void verifyJava(const RuntimeSelection& selection) {
    waitUntil(kFrameworkTimeout, "Java reader did not load its native EPUB library.", [&selection] {
        return mapped({"cvm", "java"}, selection.readerLibrary);
    });
    if (selection.javaMode == JavaMode::Agent) {
        waitUntil(kFrameworkTimeout, "The Java replacement callback did not run.", [] {
            if (isFile(runPath("prepare/java/agent-rejected"))) {
                throw std::runtime_error("Agent rejected the loaded stock class.");
            }
            return isFile(runPath("prepare/java/agent-applied"));
        });
        if (isFile(runPath("prepare/java/agent-rejected"))) {
            throw std::runtime_error("Agent rejected a loaded stock class.");
        }
        return;
    }
    const auto machines = processPids({"cvm", "java"});
    if (machines.empty() || readText("/proc/" + std::to_string(machines.front()) + "/cmdline", true)
                                    .find(runPath("prepare/java/shadow")) == std::string::npos) {
        throw std::runtime_error("Java classpath activation was not applied.");
    }
}

void verifyActivation(const RuntimeSelection& selection) {
    if (selection.javaMode != JavaMode::None) {
        verifyJava(selection);
    }
    if (selection.hasWebreader) {
        waitUntil(kReaderTimeout, "WebReader did not load the EPUB library.", [&selection] {
            return mapped({"webreader"}, selection.readerLibrary);
        });
    }
    if (selection.nativeProfile == "k4") {
        waitUntil(kReaderTimeout,
                  "webreader did not confirm native hook installation.",
                  [&selection] { return hookReady("webreader", selection.readerLibrary); });
    }
    if (!selection.kppLibrary.empty()) {
        waitUntil(kReaderTimeout,
                  "kppmainapp did not confirm native hook installation.",
                  [&selection] { return hookReady("kppmainapp", selection.kppLibrary); });
    }
    if (selection.backend != CatalogueBackend::Java) {
        waitUntil(kReaderTimeout, "Scanner did not become ready.", [] {
            return runCommand({"lipc-get-prop", "com.lab126.scanner", "fullScanStatus"}).status ==
                   0;
        });
        checkedCommand({"lipc-set-prop", "com.lab126.scanner", "doFullScan", "1"});
    }
}

void snapshotRestoredPreloads(const RuntimeSelection& selection) {
    for (const auto* job : kJobs) {
        const auto process = servicePid(job);
        auto current = process > 0 ? processEnvironment(process, "LD_PRELOAD")
                                   : trim(readText(runPath(std::string(job) + ".preload")));
        std::replace(current.begin(), current.end(), ':', ' ');
        std::istringstream libraries(current);
        std::string library;
        std::string restored;
        while (libraries >> library) {
            if (library != selection.readerLibrary && library != selection.kppLibrary &&
                library != selection.payload + "/libkindle-epub-framework-exec.so") {
                if (!restored.empty()) {
                    restored += ':';
                }
                restored += library;
            }
        }
        writeText(runPath(std::string(job) + ".restore-preload"), restored + '\n');
    }
}

void deactivatePmond() {
    checkedCommand({"/etc/init.d/framework", "stop"});
    stopPmondReader();
    checkedCommand({"/etc/init.d/pmond", "stop"});
    if (isFile(runPath("pmond-mounted"))) {
        checkedCommand({"umount", kPmondWebreader});
        removeFile(runPath("pmond-mounted"));
    }
    if (isFile(runPath("policy-mounted"))) {
        checkedCommand({"umount", kFrameworkPolicy});
        removeFile(runPath("policy-mounted"));
    }
    checkedCommand({"/etc/init.d/pmond", "start"});
    checkedCommand({"/etc/init.d/framework", "start"},
                   {},
                   {{"LD_PRELOAD", trim(readText(runPath("framework.restore-preload")))}});
}

void deactivateUpstart(const RuntimeSelection& selection) {
    pauseAppmgr(selection);
    if (isFile(runPath("framework-restarted"))) {
        checkedCommand({"initctl",
                        "restart",
                        "framework",
                        "LD_PRELOAD=" + trim(readText(runPath("framework.restore-preload"))),
                        "KINDLE_EPUB_FRAMEWORK_PRELOAD=",
                        "KINDLE_EPUB_JAVA_CLASSPATH=",
                        "KINDLE_EPUB_JAVA_BOOTCLASSPATH=",
                        "KINDLE_EPUB_JAVA_AGENT="});
        if (!selection.kppLibrary.empty()) {
            waitFramework();
        }
    }
    for (const auto* jobName : {"webreader", "kppmainapp"}) {
        const std::string job(jobName);
        if (isFile(runPath(job + "-restarted"))) {
            waitJob(job);
            checkedCommand({"initctl",
                            "restart",
                            job,
                            "LD_PRELOAD=" + trim(readText(runPath(job + ".restore-preload"))),
                            "KINDLE_EPUB_HOOK_READY="});
        }
    }
    if (selection.javaMode == JavaMode::None && jobExists("scanner")) {
        checkedCommand({"initctl", "restart", "scanner"});
    }
    resumeAppmgr(runPath("appmgrd.restore-preload"));
}

} // namespace

void activate(const std::string& payload) {
    auto selection = selectRuntime(payload);
    ensureDirectory(kRunDirectory, kPublicDirectoryMode);
    if (chmod(kRunDirectory, kPublicDirectoryMode) != 0) {
        throw std::runtime_error("Cannot set runtime directory permissions.");
    }
    ensureDirectory(std::string(kStateDirectory) + "/activation");
    prepareJava(selection);
    writeSelection(selection);
    snapshotProcesses();
    startWorker(selection);
    registerCatalogue(selection);
    snapshotToggles(selection);
    if (selection.manager == ServiceManager::Pmond) {
        activatePmond(selection);
    } else {
        activateUpstart(selection);
    }
    verifyActivation(selection);
    writeText(runPath("active"), "");
    std::cout << "Runtime active: Java=" << javaModeName(selection.javaMode)
              << " catalogue=" << catalogueName(selection.backend) << '\n';
}

void deactivate() {
    std::string catalogueError;
    try {
        restoreCatalogue();
    } catch (const std::exception& error) {
        catalogueError = error.what();
    }
    restoreToggles();
    stopWorker();
    if (isFile(runPath("selection"))) {
        const auto selection = readSelection();
        snapshotRestoredPreloads(selection);
        if (selection.manager == ServiceManager::Pmond) {
            deactivatePmond();
        } else {
            deactivateUpstart(selection);
        }
        waitUntil(kReaderTimeout,
                  "A reader still has the EPUB library loaded; activation state retained.",
                  [&selection] {
                      return !mapped({"cvm", "java", "webreader"}, selection.readerLibrary) &&
                             (selection.kppLibrary.empty() ||
                              !mapped({"KPPMainAppV2", "KPPMainApp"}, selection.kppLibrary));
                  });
    }
    removeTree(kRunDirectory);
    if (!catalogueError.empty()) {
        throw std::runtime_error("Runtime stopped, but " + catalogueError);
    }
    removeTree(std::string(kStateDirectory) + "/activation");
}

} // namespace epubctl
