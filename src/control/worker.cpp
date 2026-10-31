#include "activation.hpp"
#include "system.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace epubctl {
namespace {

constexpr unsigned kWorkerAttempts = 5;
constexpr unsigned kWorkerRetryDelay = 2;
constexpr unsigned kWorkerReadyTimeout = 30;
constexpr unsigned kWorkerStopTimeout = 10;
constexpr int kChildFailure = 127;
volatile std::sig_atomic_t workerStopping = 0;
volatile std::sig_atomic_t workerChild = 0;

void stopChild(int /* signal */) {
    const int savedError = errno;
    workerStopping = 1;
    if (workerChild > 0) {
        kill(static_cast<pid_t>(workerChild), SIGTERM);
    }
    errno = savedError;
}

bool workerReady() {
    const auto result = runCommand({"lipc-get-prop", "org.kindle.epub.scanner", "ready"});
    return result.status == 0 && trim(result.output) == "1";
}

void executeSupervisor(
    const std::string& controller, const std::string& payload, int log, int input, int gate) {
    char ready = 0;
    if (read(gate, &ready, 1) != 1) {
        _exit(kChildFailure);
    }
    close(gate);
    if (setsid() < 0 || dup2(input, STDIN_FILENO) < 0 || dup2(log, STDOUT_FILENO) < 0 ||
        dup2(log, STDERR_FILENO) < 0) {
        _exit(kChildFailure);
    }
    close(input);
    close(log);
    execl(controller.c_str(), controller.c_str(), "_worker", payload.c_str(), nullptr);
    _exit(kChildFailure);
}

bool isWorkerSupervisor(pid_t process) {
    if (!processAlive(process)) {
        return false;
    }
    const auto arguments = readText("/proc/" + std::to_string(process) + "/cmdline", true);
    const auto executableArgument = arguments.substr(0, arguments.find('\0'));
    const std::string controllerName = "/epubctl";
    const std::string workerArgument("\0_worker\0", sizeof("\0_worker\0") - 1);
    if (executableArgument.size() < controllerName.size() ||
        executableArgument.compare(executableArgument.size() - controllerName.size(),
                                   controllerName.size(),
                                   controllerName) != 0 ||
        arguments.find(workerArgument) == std::string::npos) {
        return false;
    }
    try {
        const auto executablePath = processExecutable(process);
        return executablePath == executableArgument ||
               executablePath == executableArgument + " (deleted)";
    } catch (const std::runtime_error&) {
        return false;
    }
}

void waitForChild(pid_t child) {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("Cannot wait for catalogue worker: " +
                                     std::string(std::strerror(errno)));
        }
    }
}

void launchSupervisor(const std::string& payload) {
    const auto controller = processExecutable(getpid());
    const auto logPath = std::string(kRunDirectory) + "/worker.log";
    const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, kFileMode);
    const int input = open("/dev/null", O_RDONLY);
    std::array<int, 2> gate{{-1, -1}};
    if (log < 0 || input < 0 || pipe(gate.data()) != 0) {
        const auto error = std::string(std::strerror(errno));
        if (log >= 0) {
            close(log);
        }
        if (input >= 0) {
            close(input);
        }
        throw std::runtime_error("Cannot prepare catalogue supervisor: " + error);
    }
    const pid_t supervisor = fork();
    const int forkError = errno;
    if (supervisor == 0) {
        close(gate[1]);
        executeSupervisor(controller, payload, log, input, gate[0]);
    }
    close(log);
    close(input);
    close(gate[0]);
    if (supervisor < 0) {
        close(gate[1]);
        throw std::runtime_error("Cannot start catalogue supervisor: " +
                                 std::string(std::strerror(forkError)));
    }
    try {
        // Detach only after recovery knows the PID. Cancellation before this point kills the
        // activation group.
        writeText(std::string(kRunDirectory) + "/worker-supervisor.pid",
                  std::to_string(supervisor) + '\n');
        const char ready = 1;
        if (write(gate[1], &ready, 1) != 1) {
            throw std::runtime_error("Cannot release catalogue supervisor.");
        }
        close(gate[1]);
    } catch (...) {
        close(gate[1]);
        kill(supervisor, SIGTERM);
        waitForChild(supervisor);
        throw;
    }
}

pid_t spawnWorker(const std::string& executablePath) {
    sigset_t blocked;
    sigset_t previous;
    sigemptyset(&blocked);
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        sigaddset(&blocked, signal);
    }
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) != 0) {
        throw std::runtime_error("Cannot protect catalogue worker startup from interruption.");
    }
    const pid_t child = fork();
    const int forkError = errno;
    if (child == 0) {
        struct sigaction action {};
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
            if (sigaction(signal, &action, nullptr) != 0) {
                _exit(kChildFailure);
            }
        }
        if (sigprocmask(SIG_SETMASK, &previous, nullptr) != 0 || setenv("LD_PRELOAD", "", 1) != 0) {
            _exit(kChildFailure);
        }
        execl(executablePath.c_str(), executablePath.c_str(), "--serve", nullptr);
        _exit(kChildFailure);
    }
    if (child > 0) {
        workerChild = static_cast<std::sig_atomic_t>(child);
    }
    if (sigprocmask(SIG_SETMASK, &previous, nullptr) != 0) {
        if (child > 0) {
            kill(child, SIGTERM);
            waitForChild(child);
        }
        throw std::runtime_error("Cannot restore catalogue supervisor signal mask.");
    }
    if (child < 0) {
        throw std::runtime_error("Cannot start catalogue worker: " +
                                 std::string(std::strerror(forkError)));
    }
    return child;
}

} // namespace

void startWorker(const RuntimeSelection& selection) {
    if (selection.backend != CatalogueBackend::Worker) {
        return;
    }
    if (workerReady()) {
        throw std::runtime_error("Another EPUB catalogue worker already owns the service.");
    }
    launchSupervisor(selection.payload);
    for (unsigned elapsed = 0; elapsed <= kWorkerReadyTimeout; ++elapsed) {
        if (workerReady()) {
            return;
        }
        if (elapsed != kWorkerReadyTimeout) {
            sleepSeconds(1);
        }
    }
    throw std::runtime_error("Catalogue worker did not become ready.");
}

void stopWorker() {
    const auto marker = std::string(kRunDirectory) + "/worker-supervisor.pid";
    const auto saved = readText(marker, true);
    if (saved.empty()) {
        return;
    }
    const pid_t supervisor = parsePid(saved);
    if (isWorkerSupervisor(supervisor)) {
        kill(supervisor, SIGTERM);
        for (unsigned elapsed = 0; elapsed < kWorkerStopTimeout && processAlive(supervisor);
             ++elapsed) {
            sleepSeconds(1);
        }
        if (processAlive(supervisor)) {
            throw std::runtime_error(
                "Catalogue supervisor did not stop; activation state retained.");
        }
    }
    removeFile(marker);
}

int superviseWorker(const std::string& payload) {
    struct sigaction action {};
    action.sa_handler = stopChild;
    sigemptyset(&action.sa_mask);
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        if (sigaction(signal, &action, nullptr) < 0) {
            throw std::runtime_error("Cannot install catalogue supervisor signal handler.");
        }
    }
    const auto executablePath = payload + "/kindle-epub-import";
    for (unsigned attempt = 0; attempt < kWorkerAttempts && workerStopping == 0; ++attempt) {
        const pid_t child = spawnWorker(executablePath);
        if (workerStopping != 0) {
            kill(child, SIGTERM);
        }
        waitForChild(child);
        workerChild = 0;
        if (workerStopping == 0) {
            sleepSeconds(kWorkerRetryDelay);
        }
    }
    if (workerStopping == 0) {
        std::cerr << "Catalogue worker exceeded restart limit.\n";
        return 1;
    }
    return 0;
}

} // namespace epubctl
