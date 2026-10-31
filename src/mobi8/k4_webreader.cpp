#include "epub/filename.hpp"
#include "k4_discovery.hpp"
#include "mobi8/model.hpp"
#include "runtime/elf_image.hpp"
#include "runtime/hook_ready.hpp"

#include <cstdint>
#include <cstring>
#include <link.h>
#include <strings.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

namespace {
using Factory = Mobi8SDK::Result (*)(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>&,
                                     const unsigned char*);
Factory stockFactory;

int executableBase(struct dl_phdr_info* image, std::size_t /*size*/, void* output) {
    if (image->dlpi_name == nullptr || image->dlpi_name[0] == '\0') {
        *static_cast<std::uintptr_t*>(output) = image->dlpi_addr;
        return 1;
    }
    return 0;
}

Mobi8SDK::Result openBook(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
                          const unsigned char* path) {
    const char* name = reinterpret_cast<const char*>(path);
    if (epub::isEpubPath(path)) {
        syslog(LOG_INFO, "kindle-epub: K4 WebReader opening %s", name);
        return kindle_epub::createBookManager(output, name);
    }
    return stockFactory(output, path);
}

// K4 WebReader statically embeds Mobi8 and calls its factory with an ARM BL.
// Its first two instructions are position independent. Preserve them in a
// trampoline so every non-EPUB call follows the original implementation.
__attribute__((constructor)) void installFactory() {
    try {
        constexpr std::size_t executablePathSize = 256;
        char executable[executablePathSize];
        const ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length <= 0) {
            return;
        }
        executable[length] = 0;
        if (std::strcmp(executable, "/usr/bin/webreader") != 0) {
            return;
        }
        kindle_epub::HookReadyMarker ready;
        if (!ready.valid()) {
            syslog(LOG_ERR, "kindle-epub: cannot open native readiness marker; no patches");
            return;
        }

        const kindle_epub::ElfImage image(executable);
        std::uintptr_t base = 0;
        if (image.type() == ET_DYN) {
            if (dl_iterate_phdr(executableBase, &base) == 0) {
                return;
            }
        } else if (image.type() != ET_EXEC) {
            return;
        }
        const std::uintptr_t address = base + kindle_epub::discoverK4Factory(image);
        const std::uint32_t expected[] = {0xe92d4810, 0xe28db008};
        // Factory discovery returns the validated address in the loaded executable.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        void* entry = reinterpret_cast<void*>(address);
        if (std::memcmp(entry, expected, sizeof(expected)) != 0) {
            syslog(LOG_ERR, "kindle-epub: unrecognized K4 WebReader factory");
            return;
        }
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0) {
            return;
        }
        void* memory =
            mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) {
            return;
        }
        std::uint32_t* trampoline = static_cast<std::uint32_t*>(memory);
        trampoline[0] = expected[0];
        trampoline[1] = expected[1];
        constexpr std::uint32_t jumpIndirect = 0xe51ff004; // ldr pc, [pc, #-4]
        trampoline[2] = jumpIndirect;
        trampoline[3] = address + sizeof(expected);
        __builtin___clear_cache(static_cast<char*>(memory),
                                static_cast<char*>(memory) + 4 * sizeof(*trampoline));
        if (mprotect(memory, pageSize, PROT_READ | PROT_EXEC) != 0) {
            munmap(memory, pageSize);
            return;
        }
        const std::uintptr_t pageStart = address & ~(static_cast<std::uintptr_t>(pageSize) - 1);
        const std::size_t pageBytes =
            ((address + sizeof(expected) - 1) & ~(static_cast<std::uintptr_t>(pageSize) - 1)) -
            pageStart + pageSize;
        // mprotect requires the aligned process address containing the factory.
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        void* page = reinterpret_cast<void*>(pageStart);
        if (mprotect(page, pageBytes, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            munmap(memory, pageSize);
            return;
        }
        stockFactory = reinterpret_cast<Factory>(memory);
        const std::uint32_t detour[] = {jumpIndirect, reinterpret_cast<std::uintptr_t>(&openBook)};
        std::memcpy(entry, detour, sizeof(detour));
        __builtin___clear_cache(static_cast<char*>(entry),
                                static_cast<char*>(entry) + sizeof(detour));
        if (mprotect(page, pageBytes, PROT_READ | PROT_EXEC) != 0) {
            syslog(LOG_ERR, "kindle-epub: could not restore K4 WebReader text protection");
            return;
        }
        syslog(LOG_INFO, "kindle-epub: discovered embedded WebReader EPUB factory at %p", entry);
        if (!ready.publish()) {
            syslog(LOG_ERR, "kindle-epub: cannot publish native readiness");
        }
    } catch (const std::exception& error) {
        syslog(LOG_ERR,
               "kindle-epub: embedded WebReader discovery refused: %s; no patches",
               error.what());
    } catch (...) {
        syslog(LOG_ERR, "kindle-epub: embedded WebReader discovery failed; no patches");
    }
}
} // namespace
