#include "boot.hpp"
#include "runtime.hpp"
#include "system.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace epubctl {
namespace {

const char kBeginMarker[] = "# BEGIN kindle-epub runtime";
const char kEndMarker[] = "# END kindle-epub runtime";
const char kBootGate[] = "/var/run/kindle-epub-boot";
constexpr mode_t kExecutableMode = 0755;
constexpr mode_t kPermissionBits = 07777;
constexpr unsigned kBootAttempts = 180;
constexpr unsigned kBootIntervalSeconds = 2;

struct BootHook {
    std::string shebang;
    std::string body;
    struct stat metadata = {};
};

BootHook readBootHook(const std::string& path) {
    if (isSymlink(path)) {
        throw std::runtime_error("Boot hook became a symlink; leaving it untouched.");
    }
    BootHook hook;
    if (stat(path.c_str(), &hook.metadata) != 0 || !S_ISREG(hook.metadata.st_mode)) {
        throw std::runtime_error("Cannot read boot hook: " + path);
    }
    std::istringstream input(readText(path));
    std::string line;
    bool first = true;
    bool seen = false;
    bool owned = false;
    while (std::getline(input, line)) {
        if (line == kBeginMarker && !seen) {
            seen = true;
            owned = true;
        } else if (line == kEndMarker && owned) {
            owned = false;
        } else if (line.find(kBeginMarker) != std::string::npos ||
                   line.find(kEndMarker) != std::string::npos) {
            throw std::runtime_error("Boot hook markers were modified; leaving it untouched.");
        } else if (!owned) {
            if (first && line.compare(0, 2, "#!") == 0) {
                hook.shebang = line;
            } else {
                hook.body += line + '\n';
            }
        }
        first = false;
    }
    if (owned) {
        throw std::runtime_error("Boot hook markers were modified; leaving it untouched.");
    }
    return hook;
}

void writeBootHook(const std::string& path, const std::string& text, const BootHook& hook) {
    writeText(path, text, hook.metadata.st_mode & kPermissionBits);
    if (chown(path.c_str(), hook.metadata.st_uid, hook.metadata.st_gid) != 0 ||
        chmod(path.c_str(), hook.metadata.st_mode & kPermissionBits) != 0) {
        throw std::runtime_error("Cannot preserve boot hook ownership: " + path);
    }
}

std::string hookHeader(const BootHook& hook) {
    return hook.shebang.empty() ? std::string() : hook.shebang + '\n';
}

} // namespace

std::string selectBootHook() {
    std::string selected;
    for (const char* path : {"/etc/upstart/kmc.conf", "/etc/upstart/bridge.conf"}) {
        if (readText(path, true).find("/mnt/us/emergency.sh") != std::string::npos) {
            selected = "/mnt/us/emergency.sh";
            break;
        }
    }
    if (selected.empty()) {
        for (const char* path : {"/etc/init.d/userstore", "/etc/upstart/userstore"}) {
            if (readText(path, true).find(". ${MNTUS_PARAMS}") != std::string::npos) {
                // Firmware places this file in different persistent directories.
                // Read its own trusted configuration instead of guessing a model path.
                selected = trim(
                    checkedCommand({"/bin/sh",
                                    "-c",
                                    ". /etc/sysconfig/paths; printf \"%s\\n\" \"$MNTUS_PARAMS\""}));
                const std::string persistentRoot = "/var/local/";
                if (selected.compare(0, persistentRoot.size(), persistentRoot) != 0 ||
                    isSymlink(selected) || !isFile(selected) ||
                    access(selected.c_str(), R_OK) != 0) {
                    throw std::runtime_error(
                        "Stock MNTUS_PARAMS must name a readable regular file under /var/local.");
                }
                selected = canonicalPath(selected);
                if (selected.compare(0, persistentRoot.size(), persistentRoot) != 0) {
                    throw std::runtime_error("Stock MNTUS_PARAMS resolves outside /var/local.");
                }
                break;
            }
        }
    }
    if (selected.empty()) {
        throw std::runtime_error("No supported writable boot hook. Install the current jailbreak "
                                 "boot bridge before enabling this runtime.");
    }
    if (isSymlink(selected)) {
        throw std::runtime_error("Boot hook is a symlink; refusing to replace it.");
    }
    if (isFile(selected)) {
        readBootHook(selected);
    }
    return selected;
}

void enableBoot(const std::string& hook) {
    const std::string state(kStateDirectory);
    if (isSymlink(hook)) {
        throw std::runtime_error("Boot hook became a symlink; leaving it untouched.");
    }
    writeText(state + "/boot-file", hook + '\n');
    if (!isFile(hook)) {
        writeText(hook, "#!/bin/sh\n", kExecutableMode);
        writeText(state + "/boot-created", "");
    }
    const BootHook original = readBootHook(hook);
    // Userstore may not be mounted when the firmware sources its boot hook.
    // Keep the native readiness scheduler on the persistent root filesystem.
    writeText(state + "/epubctl", readText(processExecutable(getpid())), kExecutableMode);
    const std::string block =
        std::string(kBeginMarker) + "\n" + "if [ -x /var/local/kindle-epub/epubctl ]; then\n" +
        "    /var/local/kindle-epub/epubctl _boot </dev/null >/dev/null 2>&1 &\n" + "fi\n" +
        kEndMarker + "\n";
    writeBootHook(hook, hookHeader(original) + block + original.body, original);
    ensureDirectory(kBootGate);
    writeText(state + "/enabled", "");
}

void disableBoot() {
    const std::string state(kStateDirectory);
    removeFile(state + "/enabled");
    const std::string path = trim(readText(state + "/boot-file", true));
    if (!path.empty() && isFile(path)) {
        const BootHook hook = readBootHook(path);
        writeBootHook(path, hookHeader(hook) + hook.body, hook);
        if (isFile(state + "/boot-created") && hook.shebang == "#!/bin/sh" &&
            hook.body.find_first_not_of('\n') == std::string::npos) {
            removeFile(path);
        }
    }
    removeFile(state + "/boot-file");
    removeFile(state + "/boot-created");
}

int runBoot() {
    const std::string state(kStateDirectory);
    if (!isFile(state + "/enabled") || mkdir(kBootGate, kExecutableMode) != 0) {
        return 0;
    }
    const int log = open((state + "/boot.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, kFileMode);
    if (log < 0) {
        throw std::runtime_error("Cannot open boot activation log");
    }
    const bool redirected = dup2(log, STDOUT_FILENO) >= 0 && dup2(log, STDERR_FILENO) >= 0;
    close(log);
    if (!redirected) {
        throw std::runtime_error("Cannot redirect boot activation output");
    }
    std::cout << "Boot activation: " << checkedCommand({"date"}) << std::flush;
    for (unsigned attempt = 0; attempt < kBootAttempts; ++attempt) {
        if (!isFile(state + "/enabled")) {
            return 0;
        }
        const std::string payload = trim(readText(state + "/current", true));
        const std::string controller = payload + "/epubctl";
        const std::vector<std::string> processes =
            isFile("/etc/JAVA_FREE") ? std::vector<std::string>{"KPPMainAppV2", "KPPMainApp"}
                                     : std::vector<std::string>{"cvm", "java"};
        if (executable(controller) && mounted("/mnt/us") && !processPids(processes).empty()) {
            execl(controller.c_str(), controller.c_str(), "start", static_cast<char*>(nullptr));
            throw std::runtime_error("Cannot start EPUB runtime: " +
                                     std::string(std::strerror(errno)));
        }
        sleepSeconds(kBootIntervalSeconds);
    }
    std::cerr << "Boot activation timed out waiting for stock services.\n";
    return 1;
}

} // namespace epubctl
