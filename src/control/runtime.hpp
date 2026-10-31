#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>

namespace epubctl {

extern const char kLibraryDirectory[];
extern const char kDataDirectory[];
extern const char kLibexecDirectory[];
extern const char kStateDirectory[];
extern const char kRunDirectory[];
extern const char kLockPath[];

enum class CatalogueBackend : std::uint8_t { Java, Plugin, Worker };
enum class JavaMode : std::uint8_t { None, Classpath, Agent };
enum class ServiceManager : std::uint8_t { Upstart, Pmond };

struct RuntimeSelection {
    std::string payload;
    std::string architecture;
    std::string nativeProfile;
    unsigned readerAbi = 0;
    std::string nativeDir;
    std::string readerLibrary;
    std::string kppLibrary;
    std::string javaExecutable;
    std::string applicationHandler;
    CatalogueBackend backend = CatalogueBackend::Java;
    JavaMode javaMode = JavaMode::None;
    ServiceManager manager = ServiceManager::Upstart;
    bool hasWebreader = false;
};

RuntimeSelection validatePayload(const std::string& path);
RuntimeSelection readSelection();
void writeSelection(const RuntimeSelection& selection);
bool jobExists(const std::string& job);
pid_t jobPid(const std::string& job);
bool hookReady(const std::string& job, const std::string& library);
const char* javaModeName(JavaMode mode);
const char* catalogueName(CatalogueBackend backend);
int printStatus();

} // namespace epubctl
