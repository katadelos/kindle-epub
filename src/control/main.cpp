#include "activation.hpp"
#include "runtime.hpp"
#include "system.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace epubctl {
namespace {

volatile sig_atomic_t interrupted = 0;
volatile sig_atomic_t activationChild = 0;

void interrupt(int signal) {
    const int savedError = errno;
    interrupted = signal;
    if (activationChild > 0) {
        kill(-static_cast<pid_t>(activationChild), SIGTERM);
    }
    errno = savedError;
}

void signalHandlers(void (*handler)(int)) {
    struct sigaction action = {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGHUP, SIGINT, SIGTERM}) {
        if (sigaction(signal, &action, nullptr) != 0) {
            throw std::runtime_error("Cannot configure lifecycle signal handling");
        }
    }
}

void checkInterrupted() {
    if (interrupted != 0) {
        throw std::runtime_error("Lifecycle command interrupted");
    }
}

class LifecycleLock {
public:
    LifecycleLock() : descriptor_(open(kLockPath, O_RDWR | O_CREAT | O_CLOEXEC, kFileMode)) {
        if (descriptor_ < 0) {
            throw std::runtime_error("Cannot open lifecycle lock: " +
                                     std::string(std::strerror(errno)));
        }
        if (flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
            const int error = errno;
            close(descriptor_);
            if (error == EWOULDBLOCK) {
                throw std::runtime_error("Another lifecycle command is running.");
            }
            throw std::runtime_error("Cannot acquire lifecycle lock: " +
                                     std::string(std::strerror(error)));
        }
    }

    ~LifecycleLock() {
        close(descriptor_);
    }

    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;

private:
    int descriptor_;
};

class BlockedLifecycleSignals {
public:
    BlockedLifecycleSignals() {
        sigset_t signals;
        sigemptyset(&signals);
        for (int signal : {SIGHUP, SIGINT, SIGTERM}) {
            sigaddset(&signals, signal);
        }
        if (sigprocmask(SIG_BLOCK, &signals, &previous_) != 0) {
            throw std::runtime_error("Cannot block lifecycle signals");
        }
    }

    ~BlockedLifecycleSignals() {
        restore();
    }

    void restore() const {
        sigprocmask(SIG_SETMASK, &previous_, nullptr);
    }

    BlockedLifecycleSignals(const BlockedLifecycleSignals&) = delete;
    BlockedLifecycleSignals& operator=(const BlockedLifecycleSignals&) = delete;

private:
    sigset_t previous_;
};

bool runActivation(const std::string& payload) {
    checkInterrupted();
    std::fflush(nullptr);
    pid_t child;
    {
        // The child must replace inherited handlers before cancellation can arrive.
        const BlockedLifecycleSignals blockedSignals;
        child = fork();
        if (child < 0) {
            throw std::runtime_error("Cannot start activation process");
        }
        if (child == 0) {
            try {
                if (setpgid(0, 0) != 0) {
                    throw std::runtime_error("Cannot isolate activation process group");
                }
                signalHandlers(SIG_DFL);
                blockedSignals.restore();
                activate(payload);
                std::cout.flush();
                _exit(0);
            } catch (const std::exception& error) {
                std::cerr << "kindle-epub: " << error.what() << '\n' << std::flush;
                _exit(1);
            }
        }
        // Both sides set the group so cancellation cannot race child startup.
        if (setpgid(child, child) != 0 && errno != ESRCH && errno != EACCES) {
            kill(child, SIGTERM);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
            }
            throw std::runtime_error("Cannot isolate activation process group");
        }
        activationChild = child;
    }
    if (interrupted != 0) {
        kill(-child, SIGTERM);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    activationChild = 0;
    if (waited < 0) {
        throw std::runtime_error("Cannot wait for activation process");
    }
    checkInterrupted();
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void runLifecycle(const std::string& command, bool& mutating) {
    if (command == "start") {
        // Validate the installed package before disturbing a running reader.
        const std::string payload = validatePayload(kLibraryDirectory).payload;
        if (isFile(std::string(kRunDirectory) + "/active") && printStatus() == 0) {
            return;
        }
        checkInterrupted();
        mutating = true;
        deactivate();
        if (!runActivation(payload)) {
            deactivate();
            throw std::runtime_error("Activation failed; changes rolled back.");
        }
        return;
    }
    mutating = true;
    deactivate();
    checkInterrupted();
}

void printUsage(std::ostream& output) {
    output << "Usage: epubctl start | status | stop\n";
}

bool validArguments(const std::string& command, int argc) {
    if (command == "start") {
        return argc == 2;
    }
    if (command == "status") {
        return argc == 1 || argc == 2;
    }
    return argc == 2 && (command == "stop" || command == "_worker");
}

int control(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "status";
    if (command == "--help" && argc == 2) {
        printUsage(std::cout);
        return 0;
    }
    if (!validArguments(command, argc)) {
        printUsage(std::cerr);
        return 2;
    }
    if (geteuid() != 0) {
        throw std::runtime_error("Run as root.");
    }
    if (setenv("PATH", "/usr/sbin:/sbin:/usr/bin:/bin", 1) != 0) {
        throw std::runtime_error("Cannot configure command search path");
    }
    if (command == "status") {
        return printStatus();
    }
    if (command == "_worker") {
        return superviseWorker();
    }
    ensureDirectory(kStateDirectory);
    ensureDirectory("/var/run");
    const LifecycleLock lock;
    signalHandlers(interrupt);
    bool mutating = false;
    try {
        runLifecycle(command, mutating);
        checkInterrupted();
    } catch (...) {
        if (interrupted == 0) {
            throw;
        }
        signalHandlers(SIG_IGN);
        if (mutating) {
            try {
                deactivate();
            } catch (const std::exception& error) {
                std::cerr << "kindle-epub: " << error.what() << '\n';
            }
        }
        std::cerr << "Lifecycle command interrupted; any incomplete recovery state is retained.\n";
        return 1;
    }
    return 0;
}

} // namespace
} // namespace epubctl

int main(int argc, char** argv) {
    try {
        return epubctl::control(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "kindle-epub: " << error.what() << '\n';
        return 1;
    }
}
