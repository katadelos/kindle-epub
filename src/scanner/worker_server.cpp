#include "shared_library.hpp"
#include "worker_request.hpp"
#include "worker_runtime.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace epub_catalog {
namespace {
const char* const service = "org.kindle.epub.scanner";
constexpr int kReadyAttempts = 100;
constexpr useconds_t kReadyIntervalUs = 100000;
constexpr int kDaemonTimeoutMs = 10000;
volatile sig_atomic_t stopped = 0;
int signalPipe[2] = {-1, -1};
std::mutex callbackMutex;

void stop(int /*signal*/) {
    const int saved = errno;
    stopped = 1;
    const char byte = 0;
    if (signalPipe[1] >= 0) {
        (void)write(signalPipe[1], &byte, 1);
    }
    errno = saved;
}

int ready(void* /*handle*/, const char* /*property*/, void* value, void* /*context*/) {
    if (value == nullptr) {
        return 1;
    }
    *static_cast<int*>(value) = 1;
    return 0;
}

int change(void* /*handle*/, const char* property, void* value, void* /*context*/) {
    try {
        const std::lock_guard<std::mutex> lock(callbackMutex);
        if (property == nullptr || value == nullptr) {
            throw std::runtime_error("Scanner event is missing its operation or payload");
        }
        // Stock libEBridge sends glob:uuid:path. Split only twice: colons
        // are legal in the filename. New files have an empty UUID.
        const std::string message(static_cast<const char*>(value));
        const std::size_t first = message.find(':');
        const std::size_t second =
            first == std::string::npos ? first : message.find(':', first + 1);
        if (second == std::string::npos) {
            throw std::runtime_error("Malformed scanner event");
        }
        const std::string glob = message.substr(0, first);
        const std::string uuid = message.substr(first + 1, second - first - 1);
        const std::string path = message.substr(second + 1);
        if (glob != "*.epub" && glob != "*.EPUB") {
            throw std::runtime_error("Unexpected scanner glob");
        }
        const char* existing = uuid.empty() ? nullptr : uuid.c_str();
        if (std::strcmp(property, "delete") == 0) {
            deleteFile(DeleteRequest(path.c_str(), existing));
        } else if (std::strcmp(property, "add") == 0 || std::strcmp(property, "update") == 0) {
            importFile(ImportRequest(path.c_str(), existing, PathScope::Documents));
        } else {
            throw std::runtime_error("Unknown scanner operation");
        }
        syslog(LOG_INFO, "%s %s", property, path.c_str());
        return 0;
    } catch (const std::exception& error) {
        syslog(LOG_ERR, "%s: %s", property != nullptr ? property : "(null)", error.what());
        // Error 8 makes the bridge try delete+add; use a general failure so
        // malformed books never delete an existing entry or reading progress.
    } catch (...) {
        syslog(LOG_ERR, "Scanner callback failed unexpectedly");
    }
    return 1;
}

int run(int readyFd) {
    using Open = void* (*)(const char*, int*);
    using Close = void (*)(void*);
    using Callback = int (*)(void*, const char*, void*, void*);
    using Register = int (*)(void*, const char*, Callback, Callback, void*);
    int result = 1;
    openlog("kindle-epub-scanner", LOG_PID, LOG_USER);
    try {
        if (pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
            throw std::runtime_error("Cannot create EPUB scanner signal pipe");
        }
        stopped = 0;
        const kindle_epub::SharedLibrary library("liblipc.so.1");
        const Close closeLipc = library.resolve<Close>("LipcClose");
        int error = 0;
        std::unique_ptr<void, Close> lipc(library.resolve<Open>("LipcOpenEx")(service, &error),
                                          closeLipc);
        if (!lipc || error != 0) {
            throw std::runtime_error("Cannot claim EPUB scanner LIPC service");
        }
        const Register registerProperty = library.resolve<Register>("LipcRegisterStringProperty");
        const char* properties[] = {"add", "update", "delete"};
        for (const char* property : properties) {
            if (registerProperty(lipc.get(), property, nullptr, change, nullptr) != 0) {
                throw std::runtime_error("Cannot register EPUB scanner property");
            }
        }
        if (library.resolve<Register>("LipcRegisterIntProperty")(
                lipc.get(), "ready", ready, nullptr, nullptr) != 0) {
            throw std::runtime_error("Cannot register EPUB scanner readiness");
        }
        struct sigaction action = {};
        action.sa_handler = stop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
        if (readyFd >= 0) {
            const char ready = 0;
            if (write(readyFd, &ready, 1) != 1) {
                throw std::runtime_error("Daemon parent disconnected");
            }
            ::close(readyFd);
            readyFd = -1;
        }
        syslog(LOG_INFO, "ready: %s", service);
        struct pollfd signalWaiter = {signalPipe[0], POLLIN, 0};
        while (stopped == 0) {
            if (poll(&signalWaiter, 1, -1) < 0 && errno != EINTR) {
                throw std::runtime_error("EPUB scanner signal wait failed");
            }
        }
        result = 0;
    } catch (const std::exception& error) {
        syslog(LOG_ERR, "%s", error.what());
        std::fprintf(stderr, "kindle-epub-scanner: %s\n", error.what());
    }
    if (readyFd >= 0) {
        ::close(readyFd);
    }
    for (int& descriptor : signalPipe) {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
        descriptor = -1;
    }
    closelog();
    return result;
}
} // namespace

namespace {
bool waitUntilReady() {
    using Open = void* (*)();
    using Close = void (*)(void*);
    using Get = int (*)(void*, const char*, const char*, int*);
    try {
        const kindle_epub::SharedLibrary library("liblipc.so.1");
        const Close closeLipc = library.resolve<Close>("LipcClose");
        std::unique_ptr<void, Close> lipc(library.resolve<Open>("LipcOpenNoName")(), closeLipc);
        if (!lipc) {
            throw std::runtime_error("Cannot probe EPUB scanner readiness");
        }
        const Get get = library.resolve<Get>("LipcGetIntProperty");
        for (int attempt = 0; attempt < kReadyAttempts; ++attempt) {
            int value = 0;
            if (get(lipc.get(), service, "ready", &value) == 0 && value == 1) {
                return true;
            }
            usleep(kReadyIntervalUs);
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kindle-epub-scanner: %s\n", error.what());
    }
    return false;
}

[[noreturn]] void runDaemon(int readyFd) {
    if (setsid() < 0) {
        _exit(1);
    }
    const int null = open("/dev/null", O_RDWR);
    if (null < 0) {
        _exit(1);
    }
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        dup2(null, descriptor);
    }
    if (null > 2) {
        close(null);
    }
    _exit(run(readyFd));
}

} // namespace

int serve(bool daemonize) {
    const int lock = open("/var/local/kindleepub-scanner.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock < 0) {
        std::perror("EPUB scanner lock");
        return 1;
    }
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        close(lock);
        if (error == EWOULDBLOCK) {
            return waitUntilReady() ? 0 : 1;
        }
        std::fprintf(stderr, "EPUB scanner lock: %s\n", std::strerror(error));
        return 1;
    }
    if (!daemonize) {
        const int result = run(-1);
        close(lock);
        return result;
    }
    int ready[2];
    if (pipe(ready) != 0) {
        close(lock);
        return 1;
    }
    const pid_t child = fork();
    if (child < 0) {
        close(lock);
        close(ready[0]);
        close(ready[1]);
        return 1;
    }
    if (child == 0) {
        close(ready[0]);
        runDaemon(ready[1]);
    }
    close(ready[1]);
    close(lock);
    struct pollfd waiter = {ready[0], POLLIN, 0};
    int status;
    do {
        status = poll(&waiter, 1, kDaemonTimeoutMs);
    } while (status < 0 && errno == EINTR);
    char result = 1;
    const bool success = status > 0 && read(ready[0], &result, 1) == 1 && result == 0;
    close(ready[0]);
    if (!success) {
        kill(child, SIGTERM);
        while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        std::fprintf(stderr, "EPUB scanner failed to become ready; see syslog\n");
    }
    return success ? 0 : 1;
}
} // namespace epub_catalog
