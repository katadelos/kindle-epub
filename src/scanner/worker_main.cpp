#include "worker_request.hpp"
#include "worker_runtime.hpp"

#include <cstdio>
#include <cstring>
#include <exception>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--check") == 0) {
            epub_catalog::checkRuntime();
            std::puts("Native catalogue APIs available");
            return 0;
        }
        if (argc == 2 &&
            (std::strcmp(argv[1], "--serve") == 0 || std::strcmp(argv[1], "--daemon") == 0)) {
            return epub_catalog::serve(std::strcmp(argv[1], "--daemon") == 0);
        }

        const char* filename = nullptr;
        const char* existingUuid = nullptr;
        if (argc == 2) {
            filename = argv[1];
        } else if (argc == 4 && std::strcmp(argv[1], "--update") == 0) {
            existingUuid = argv[2];
            filename = argv[3];
        } else {
            std::fprintf(
                stderr,
                "Usage: %s [--update CATALOG_UUID] FILE.epub | --serve | --daemon | --check\n",
                argv[0]);
            return 2;
        }

        const epub_catalog::ImportRequest request(
            filename, existingUuid, epub_catalog::PathScope::Any);
        const std::string uuid = epub_catalog::importFile(request);
        std::printf("%s\n", uuid.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kindle-epub-import: %s\n", error.what());
        return 1;
    }
}
