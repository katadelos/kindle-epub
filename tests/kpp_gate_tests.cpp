
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "epub/filename.hpp"
#include "kpp/gate.hpp"

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testRouting() {
    check(epub::isEpubExtension("epub", 4), "lowercase extension rejected");
    check(epub::isEpubExtension("EPUB", 4), "uppercase extension rejected");
    check(!epub::isEpubExtension("epubx", sizeof("epubx") - 1), "long extension accepted");
    check(epub::isEpubPath(reinterpret_cast<const unsigned char*>("/mnt/us/a.EPUB")),
          "EPUB path rejected");
    check(epub::isEpubPath(reinterpret_cast<const unsigned char*>("a.epub?x=1")),
          "EPUB path with query rejected");
    check(!epub::isEpubPath(reinterpret_cast<const unsigned char*>("a.azw3")),
          "AZW3 path intercepted");
}

void testVeneer() {
    const auto veneer = kindle_epub::kpp::makeThumbAbsoluteVeneer(0x12345678U);
    const unsigned char expected[8] = {0xdf, 0xf8, 0x00, 0xf0, 0x79, 0x56, 0x34, 0x12};
    check(std::memcmp(veneer.data(), expected, sizeof(expected)) == 0,
          "Thumb veneer encoding failed");
}

} // namespace

int main() {
    try {
        testRouting();
        testVeneer();
        std::cout << "kindle_kpp_gate_tests: all checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "kindle_kpp_gate_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
