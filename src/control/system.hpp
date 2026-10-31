#pragma once

#include <sys/types.h>

#include <map>
#include <string>
#include <vector>

namespace epubctl {

constexpr mode_t kFileMode = 0644;
constexpr mode_t kDirectoryMode = 0755;

struct CommandResult {
    int status;
    std::string output;
};

CommandResult runCommand(const std::vector<std::string>& arguments,
                         const std::string& input = {},
                         const std::map<std::string, std::string>& environment = {});
std::string checkedCommand(const std::vector<std::string>& arguments,
                           const std::string& input = {},
                           const std::map<std::string, std::string>& environment = {});

std::string readText(const std::string& path, bool optional = false);
std::string trim(const std::string& text);
pid_t parsePid(const std::string& text);
void writeText(const std::string& path, const std::string& text, mode_t mode = kFileMode);
void ensureDirectory(const std::string& path, mode_t mode = kDirectoryMode);
void removeTree(const std::string& path);
void removeFile(const std::string& path);
bool exists(const std::string& path);
bool isFile(const std::string& path);
bool isDirectory(const std::string& path);
bool isSymlink(const std::string& path);
bool executable(const std::string& path);
std::string canonicalPath(const std::string& path);
std::vector<pid_t> processPids(const std::vector<std::string>& names);
bool processAlive(pid_t process);
std::string processExecutable(pid_t process);
std::string processEnvironment(pid_t process, const std::string& name);
bool processMaps(pid_t process, const std::string& path);
bool mapped(const std::vector<std::string>& names, const std::string& library);
bool mounted(const std::string& path);
void sleepSeconds(unsigned seconds);

} // namespace epubctl
