#include "control/system.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <mntent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace epubctl {
namespace {

constexpr std::size_t kMaximumTextSize = std::size_t{16} * 1024U * 1024U;
constexpr std::size_t kReadBufferSize = 8192;
constexpr int kExecFailure = 127;
constexpr int kSignalExitBase = 128;

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) {
            close(value_);
        }
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const {
        return value_;
    }

private:
    int value_;
};

std::runtime_error systemError(const std::string& action) {
    return std::runtime_error(action + ": " + std::strerror(errno));
}

void writeAll(int descriptor, const std::string& text) {
    std::size_t position = 0;
    while (position < text.size()) {
        const ssize_t count = write(descriptor, text.data() + position, text.size() - position);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw systemError("Cannot write file");
        }
        position += static_cast<std::size_t>(count);
    }
}

std::string readAll(int descriptor) {
    std::string result;
    char buffer[kReadBufferSize];
    for (;;) {
        const ssize_t count = read(descriptor, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw systemError("Cannot read file");
        }
        if (count == 0) {
            return result;
        }
        if (static_cast<std::size_t>(count) > kMaximumTextSize - result.size()) {
            throw std::runtime_error("File or command output exceeds the text limit");
        }
        result.append(buffer, static_cast<std::size_t>(count));
    }
}

int waitFor(pid_t process) {
    int status = 0;
    while (waitpid(process, &status, 0) < 0) {
        if (errno != EINTR) {
            throw systemError("Cannot wait for command");
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : kSignalExitBase + WTERMSIG(status);
}

int temporaryFile() {
    char path[] = "/tmp/epubctl-io-XXXXXX";
    int descriptor = mkstemp(path);
    if (descriptor < 0) {
        throw systemError("Cannot prepare command stream");
    }
    unlink(path);
    if (descriptor <= STDERR_FILENO) {
        const int duplicate = fcntl(descriptor, F_DUPFD, STDERR_FILENO + 1);
        close(descriptor);
        if (duplicate < 0) {
            throw systemError("Cannot reserve command stream descriptor");
        }
        descriptor = duplicate;
    }
    return descriptor;
}

using Directory = std::unique_ptr<DIR, int (*)(DIR*)>;

std::string parentDirectory(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    if (separator == std::string::npos) {
        return ".";
    }
    return separator == 0 ? "/" : path.substr(0, separator);
}

} // namespace

CommandResult runCommand(const std::vector<std::string>& arguments,
                         const std::string& input,
                         const std::map<std::string, std::string>& environment) {
    if (arguments.empty() || arguments.front().empty()) {
        throw std::invalid_argument("Command has no executable");
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments) {
        if (argument.find('\0') != std::string::npos) {
            throw std::invalid_argument("Command argument contains a NUL");
        }
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    for (const auto& variable : environment) {
        if (variable.first.empty() || variable.first.find('=') != std::string::npos ||
            variable.first.find('\0') != std::string::npos ||
            variable.second.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid command environment");
        }
    }

    // Regular temporary files avoid pipe deadlocks and do not require daemon
    // descendants to close stdout before the direct command can finish.
    Descriptor inputFile(temporaryFile());
    Descriptor outputFile(temporaryFile());
    writeAll(inputFile.get(), input);
    if (lseek(inputFile.get(), 0, SEEK_SET) < 0) {
        throw systemError("Cannot rewind command input");
    }
    const pid_t child = fork();
    if (child < 0) {
        throw systemError("Cannot start command");
    }
    if (child == 0) {
        if (dup2(inputFile.get(), STDIN_FILENO) < 0 || dup2(outputFile.get(), STDOUT_FILENO) < 0) {
            _exit(kExecFailure);
        }
        close(outputFile.get());
        close(inputFile.get());
        for (const auto& variable : environment) {
            if (setenv(variable.first.c_str(), variable.second.c_str(), 1) != 0) {
                _exit(kExecFailure);
            }
        }
        execvp(argv.front(), argv.data());
        _exit(kExecFailure);
    }
    int status = 0;
    try {
        status = waitFor(child);
    } catch (...) {
        kill(child, SIGKILL);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        throw;
    }
    if (lseek(outputFile.get(), 0, SEEK_SET) < 0) {
        throw systemError("Cannot rewind command output");
    }
    return {status, readAll(outputFile.get())};
}

std::string checkedCommand(const std::vector<std::string>& arguments,
                           const std::string& input,
                           const std::map<std::string, std::string>& environment) {
    const CommandResult result = runCommand(arguments, input, environment);
    if (result.status != 0) {
        throw std::runtime_error(arguments.front() + " failed (" + std::to_string(result.status) +
                                 "): " + result.output);
    }
    return result.output;
}

std::string readText(const std::string& path, bool optional) {
    Descriptor file(open(path.c_str(), O_RDONLY));
    if (file.get() < 0) {
        if (optional && (errno == ENOENT || errno == ENOTDIR)) {
            return {};
        }
        throw systemError("Cannot read " + path);
    }
    return readAll(file.get());
}

std::string trim(const std::string& text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

pid_t parsePid(const std::string& text) {
    const std::string value = trim(text);
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char digit) {
            return digit >= '0' && digit <= '9';
        })) {
        return 0;
    }
    errno = 0;
    char* end = nullptr;
    const long number = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || number <= 0 || number > std::numeric_limits<pid_t>::max()) {
        return 0;
    }
    return static_cast<pid_t>(number);
}

// Filesystem paths and file contents are both strings; their named arguments
// retain the standard path/contents ordering used by every call site.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void writeText(const std::string& path, const std::string& text, mode_t mode) {
    std::vector<char> temporary(path.begin(), path.end());
    const char suffix[] = ".XXXXXX";
    temporary.insert(temporary.end(), suffix, suffix + sizeof(suffix));
    Descriptor file(mkstemp(temporary.data()));
    if (file.get() < 0) {
        throw systemError("Cannot create " + path);
    }
    try {
        if (fchmod(file.get(), mode) != 0) {
            throw systemError("Cannot set permissions for " + path);
        }
        writeAll(file.get(), text);
        if (fsync(file.get()) != 0 || rename(temporary.data(), path.c_str()) != 0) {
            throw systemError("Cannot replace " + path);
        }
    } catch (...) {
        unlink(temporary.data());
        throw;
    }
}

void ensureDirectory(const std::string& path, mode_t mode) {
    if (isDirectory(path)) {
        return;
    }
    const std::string parent = parentDirectory(path);
    if (parent != path) {
        ensureDirectory(parent, mode);
    }
    if (mkdir(path.c_str(), mode) != 0 && (errno != EEXIST || !isDirectory(path))) {
        throw systemError("Cannot create directory " + path);
    }
}

void removeFile(const std::string& path) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw systemError("Cannot remove " + path);
    }
}

void removeTree(const std::string& path) {
    if (path.empty() || path == "/" || path == "." || path == "..") {
        throw std::invalid_argument("Invalid removal directory");
    }
    struct stat status {};
    if (lstat(path.c_str(), &status) != 0) {
        if (errno == ENOENT) {
            return;
        }
        throw systemError("Cannot inspect " + path);
    }
    if (!S_ISDIR(status.st_mode)) {
        removeFile(path);
        return;
    }
    Directory directory(opendir(path.c_str()), closedir);
    if (!directory) {
        throw systemError("Cannot list " + path);
    }
    while (const dirent* entry = readdir(directory.get())) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") {
            std::string child = path + "/";
            child += name;
            removeTree(child);
        }
    }
    if (rmdir(path.c_str()) != 0) {
        throw systemError("Cannot remove directory " + path);
    }
}

bool exists(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0;
}

bool isFile(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode);
}

bool isSymlink(const std::string& path) {
    struct stat status {};
    return lstat(path.c_str(), &status) == 0 && S_ISLNK(status.st_mode);
}

bool executable(const std::string& path) {
    // Kindle's FSP can reject access(X_OK) even when root can execute the file.
    // Match the stock root shell's mode check; exec reports actual launch errors.
    struct stat status {};
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
           (status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

std::string canonicalPath(const std::string& path) {
    std::unique_ptr<char, decltype(&std::free)> resolved(realpath(path.c_str(), nullptr),
                                                         std::free);
    if (!resolved) {
        throw systemError("Cannot resolve " + path);
    }
    return resolved.get();
}

std::vector<pid_t> processPids(const std::vector<std::string>& names) {
    Directory processes(opendir("/proc"), closedir);
    if (!processes) {
        throw systemError("Cannot list processes");
    }
    std::vector<pid_t> result;
    while (const dirent* entry = readdir(processes.get())) {
        const pid_t process = parsePid(entry->d_name);
        if (process == 0) {
            continue;
        }
        const std::string status = readText("/proc/" + std::to_string(process) + "/stat", true);
        const std::size_t start = status.find('(');
        const std::size_t end = status.rfind(')');
        if (start == std::string::npos || end == std::string::npos || end <= start) {
            continue;
        }
        const std::string name = status.substr(start + 1, end - start - 1);
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            result.push_back(process);
        }
    }
    std::sort(result.begin(), result.end(), std::greater<pid_t>());
    return result;
}

bool processAlive(pid_t process) {
    return process > 0 && (kill(process, 0) == 0 || errno == EPERM);
}

std::string processExecutable(pid_t process) {
    const std::string path = "/proc/" + std::to_string(process) + "/exe";
    char buffer[PATH_MAX];
    const ssize_t length = readlink(path.c_str(), buffer, sizeof(buffer));
    if (length < 0 || static_cast<std::size_t>(length) == sizeof(buffer)) {
        throw systemError("Cannot resolve " + path);
    }
    return std::string(buffer, static_cast<std::size_t>(length));
}

std::string processEnvironment(pid_t process, const std::string& name) {
    const std::string content = readText("/proc/" + std::to_string(process) + "/environ", true);
    const std::string prefix = name + "=";
    std::size_t start = 0;
    while (start < content.size()) {
        const std::size_t end = content.find('\0', start);
        const std::string entry = content.substr(start, end - start);
        if (entry.compare(0, prefix.size(), prefix) == 0) {
            return entry.substr(prefix.size());
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

bool processMaps(pid_t process, const std::string& path) {
    return readText("/proc/" + std::to_string(process) + "/maps", true).find(path) !=
           std::string::npos;
}

bool mapped(const std::vector<std::string>& names, const std::string& library) {
    const std::vector<pid_t> processes = processPids(names);
    return std::any_of(processes.begin(), processes.end(), [&](pid_t process) {
        return processMaps(process, library);
    });
}

bool mounted(const std::string& path) {
    std::unique_ptr<FILE, int (*)(FILE*)> mounts(setmntent("/proc/mounts", "r"), endmntent);
    if (!mounts) {
        throw systemError("Cannot read mount table");
    }
    while (const mntent* mount = getmntent(mounts.get())) {
        if (path == mount->mnt_dir) {
            return true;
        }
    }
    return false;
}

void sleepSeconds(unsigned seconds) {
    while (seconds != 0) {
        seconds = sleep(seconds);
    }
}

} // namespace epubctl
