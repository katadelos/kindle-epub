#include "discovery.hpp"
#include "epub/filename.hpp"
#include "kpp/gate.hpp"
#include "runtime/hook_ready.hpp"

#include <limits.h>
#include <link.h>
#include <sys/mman.h>
#include <syslog.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "mobi8/abi.hpp"
#include "mobi8/model.hpp"

extern "C" {
// Assembly dispatchers tail-jump here for stock books, retaining the caller's
// original stack and LR so native C++ exceptions unwind through stock frames.
__attribute__((visibility("hidden"))) void* kindle_epub_kpp_factory_original = nullptr;
__attribute__((visibility("hidden"))) void* kindle_epub_kpp_extension_original = nullptr;
void kindle_epub_kpp_factory_entry();
void kindle_epub_kpp_extension_entry();
}

namespace {
int mainImage(struct dl_phdr_info* info, std::size_t /*size*/, void* context) {
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
        return 0;
    }
    std::vector<kindle_epub::kpp::ImageRange>& image =
        *static_cast<std::vector<kindle_epub::kpp::ImageRange>*>(context);
    for (unsigned int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& header = info->dlpi_phdr[i];
        if (header.p_type == PT_LOAD && (header.p_flags & PF_R) != 0) {
            kindle_epub::kpp::ImageRange range = {
                static_cast<std::uintptr_t>(info->dlpi_addr + header.p_vaddr),
                static_cast<std::size_t>(header.p_memsz),
                (header.p_flags & PF_X) != 0,
                nullptr};
            image.push_back(range);
        }
    }
    return 1;
}

// Hook installation converts validated process addresses and sets the Thumb bit.
// NOLINTBEGIN(performance-no-int-to-ptr)
bool writeBytes(std::uintptr_t address, const unsigned char* bytes, std::size_t length) {
    const long rawPageSize = sysconf(_SC_PAGESIZE);
    if (rawPageSize <= 0) {
        return false;
    }
    const std::uintptr_t pageSize = static_cast<std::uintptr_t>(rawPageSize);
    const std::uintptr_t firstPage = address & ~(pageSize - 1U);
    const std::uintptr_t lastByte = address + length - 1U;
    if (lastByte < address) {
        return false;
    }
    const std::size_t mappedLength =
        static_cast<std::size_t>((lastByte & ~(pageSize - 1U)) + pageSize - firstPage);
    void* const page = reinterpret_cast<void*>(firstPage);
    if (mprotect(page, mappedLength, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(address), bytes, length);
    __builtin___clear_cache(reinterpret_cast<char*>(address),
                            reinterpret_cast<char*>(address + length));
    return mprotect(page, mappedLength, PROT_READ | PROT_EXEC) == 0;
}

bool installVeneer(std::uintptr_t address, void (*hook)()) {
    const auto veneer = kindle_epub::kpp::makeThumbAbsoluteVeneer(
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hook)));
    return writeBytes(address, veneer.data(), veneer.size());
}

__attribute__((constructor(101))) void initializeKppHook() {
    char executable[PATH_MAX + 1];
    const ssize_t size = readlink("/proc/self/exe", executable, PATH_MAX);
    if (size <= 0 || size > PATH_MAX) {
        return;
    }
    executable[size] = '\0';
    const char* basename = std::strrchr(executable, '/');
    basename = basename != nullptr ? basename + 1 : executable;
    if (std::strcmp(basename, "KPPMainApp") != 0 && std::strcmp(basename, "KPPMainAppV2") != 0) {
        return;
    }
    kindle_epub::HookReadyMarker ready;
    if (!ready.valid()) {
        syslog(LOG_ERR, "kindle-epub-kpp: cannot open native readiness marker; no patches");
        return;
    }
    try {
        std::vector<kindle_epub::kpp::ImageRange> image;
        dl_iterate_phdr(mainImage, &image);
        const kindle_epub::kpp::HookSites sites = kindle_epub::kpp::discover(image);
        if (sites.extensionCount != 1 || sites.factoryCount != 1 ||
            sites.stringAbi != KINDLE_EPUB_KPP_CXX11_ABI) {
            syslog(LOG_ERR,
                   "kindle-epub-kpp: discovery refused: extension=%u factory=%u string-ABI=%u "
                   "module-ABI=%u; no patches",
                   sites.extensionCount,
                   sites.factoryCount,
                   sites.stringAbi,
                   KINDLE_EPUB_KPP_CXX11_ABI);
            return;
        }
        const std::size_t trampolineSize = kindle_epub::kpp::kTrampolineSize;
        const long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize < 2 * static_cast<long>(trampolineSize)) {
            return;
        }
        unsigned char* code = static_cast<unsigned char*>(
            mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (code == MAP_FAILED) {
            return;
        }
        if (kindle_epub::kpp::makeTrampoline(image, sites.factory, false, sites.stringAbi, code) ==
                0 ||
            kindle_epub::kpp::makeTrampoline(
                image, sites.extension, true, sites.stringAbi, code + trampolineSize) == 0) {
            munmap(code, pageSize);
            syslog(LOG_ERR, "kindle-epub-kpp: unsupported entry relocation; no patches");
            return;
        }
        __builtin___clear_cache(reinterpret_cast<char*>(code),
                                reinterpret_cast<char*>(code + 2 * trampolineSize));
        if (mprotect(code, pageSize, PROT_READ | PROT_EXEC) != 0) {
            munmap(code, pageSize);
            return;
        }
        kindle_epub_kpp_factory_original =
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(code) | 1U);
        kindle_epub_kpp_extension_original =
            reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(code + trampolineSize) | 1U);
        void* const factoryEntry = reinterpret_cast<void*>(sites.factory);
        void* const extensionEntry = reinterpret_cast<void*>(sites.extension);
        unsigned char factoryBytes[kindle_epub::kpp::kThumbAbsoluteVeneerSize];
        unsigned char extensionBytes[kindle_epub::kpp::kThumbAbsoluteVeneerSize];
        std::memcpy(factoryBytes, factoryEntry, sizeof(factoryBytes));
        std::memcpy(extensionBytes, extensionEntry, sizeof(extensionBytes));
        if (!installVeneer(sites.factory, &kindle_epub_kpp_factory_entry) ||
            !installVeneer(sites.extension, &kindle_epub_kpp_extension_entry)) {
            // writeBytes can fail after the write if restoring RX fails.
            // Restore both original entries; retain trampolines for safety.
            const bool factoryRestored =
                writeBytes(sites.factory, factoryBytes, sizeof(factoryBytes));
            const bool extensionRestored =
                writeBytes(sites.extension, extensionBytes, sizeof(extensionBytes));
            syslog(LOG_ERR,
                   "kindle-epub-kpp: patch failed; restored factory=%d extension=%d",
                   static_cast<int>(factoryRestored),
                   static_cast<int>(extensionRestored));
            return;
        }
        syslog(
            LOG_INFO,
            "kindle-epub-kpp: discovered native EPUB hooks extension=%p factory=%p string-ABI=%u",
            extensionEntry,
            factoryEntry,
            sites.stringAbi);
        if (!ready.publish()) {
            syslog(LOG_ERR, "kindle-epub-kpp: cannot publish native readiness");
        }
    } catch (...) {
        syslog(LOG_ERR, "kindle-epub-kpp: discovery failed; no hooks installed");
    }
}
// NOLINTEND(performance-no-int-to-ptr)
} // namespace

extern "C" bool kindle_epub_kpp_is_epub_path(const unsigned char* path) {
    return epub::isEpubPath(path);
}

extern "C" Mobi8SDK::Result
kindle_epub_kpp_factory_hook(Mobi8SDK::ManagedPtr<Mobi8SDK::IBookManager>& output,
                             const unsigned char* path) {
    const Mobi8SDK::Result status =
        kindle_epub::createBookManager(output, reinterpret_cast<const char*>(path));
    syslog(status == Mobi8SDK::SUCCESS ? LOG_INFO : LOG_ERR,
           "kindle-epub-kpp: EPUB factory status=%u path=%s",
           static_cast<unsigned int>(status),
           reinterpret_cast<const char*>(path));
    return status;
}

extern "C" __attribute__((visibility("default"))) const char* kindle_epub_kpp_version() {
    return "kindle-epub-kpp/0.2.0 runtime-discovery";
}
