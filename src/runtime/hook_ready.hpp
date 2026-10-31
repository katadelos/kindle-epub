#ifndef KINDLE_EPUB_HOOK_READY_HPP
#define KINDLE_EPUB_HOOK_READY_HPP

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kindle_epub {
// The lifecycle creates this file for the service uid before launch. Retaining
// its descriptor avoids replacing ownership or requiring directory write access.
class HookReadyMarker {
public:
    HookReadyMarker() noexcept {
        const char* path = std::getenv("KINDLE_EPUB_HOOK_READY");
        if (path == nullptr || *path == '\0') {
            return;
        }
        requested_ = true;
        fd_ = open(path, O_WRONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd_ < 0) {
            return;
        }
        struct stat status;
        if (fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode) || ftruncate(fd_, 0) != 0) {
            close(fd_);
            fd_ = -1;
            return;
        }
        fcntl(fd_, F_SETFD, FD_CLOEXEC);
    }
    ~HookReadyMarker() noexcept {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    HookReadyMarker(const HookReadyMarker&) = delete;
    HookReadyMarker& operator=(const HookReadyMarker&) = delete;

    bool valid() const noexcept {
        return !requested_ || fd_ >= 0;
    }
    bool publish() const noexcept {
        if (!requested_) {
            return true;
        }
        if (fd_ < 0) {
            return false;
        }
        constexpr std::size_t pidBufferSize = 32;
        char text[pidBufferSize];
        const int length = std::snprintf(text, sizeof(text), "%ld\n", static_cast<long>(getpid()));
        if (length <= 0 || static_cast<unsigned>(length) >= sizeof(text)) {
            return false;
        }
        int written = 0;
        while (written < length) {
            const ssize_t count = write(fd_, text + written, length - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                ftruncate(fd_, 0);
                return false;
            }
            written += static_cast<int>(count);
        }
        return true;
    }

private:
    int fd_{-1};
    bool requested_{false};
};
} // namespace kindle_epub
#endif
