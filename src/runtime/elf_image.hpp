#ifndef KINDLE_EPUB_ELF_IMAGE_HPP
#define KINDLE_EPUB_ELF_IMAGE_HPP

#include <cstdint>
#include <cstring>
#include <elf.h>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace kindle_epub {
// Read-only ARM32 ELF inspection. All offsets and strings are bounded by the
// mapped file; neither stock code nor its constructors are executed by probes.
class ElfImage {
public:
    explicit ElfImage(const std::string& path) {
        const int descriptor = open(path.c_str(), O_RDONLY);
        if (descriptor < 0) {
            throw std::runtime_error("Cannot open " + path);
        }
        constexpr off_t maxFileSize = off_t(512) * 1024 * 1024;
        struct stat status;
        if (fstat(descriptor, &status) != 0 ||
            status.st_size < static_cast<off_t>(sizeof(Elf32_Ehdr)) ||
            status.st_size > maxFileSize) {
            close(descriptor);
            throw std::runtime_error("Invalid ELF file size: " + path);
        }
        size_ = static_cast<std::size_t>(status.st_size);
        void* data = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor, 0);
        close(descriptor);
        if (data == MAP_FAILED) {
            throw std::runtime_error("Cannot map " + path);
        }
        bytes_ = static_cast<const unsigned char*>(data);
        try {
            header_ = read<Elf32_Ehdr>(0);
            if (std::memcmp(header_.e_ident, ELFMAG, SELFMAG) != 0 ||
                header_.e_ident[EI_CLASS] != ELFCLASS32 ||
                header_.e_ident[EI_DATA] != ELFDATA2LSB || header_.e_machine != EM_ARM ||
                header_.e_phentsize != sizeof(Elf32_Phdr) ||
                header_.e_shentsize != sizeof(Elf32_Shdr) || header_.e_shnum == 0) {
                throw std::runtime_error("Expected little-endian ARM32 ELF: " + path);
            }
            for (unsigned i = 0; i < header_.e_phnum; ++i) {
                segments_.push_back(
                    read<Elf32_Phdr>(header_.e_phoff + std::size_t(i) * sizeof(Elf32_Phdr)));
            }
            for (unsigned i = 0; i < header_.e_shnum; ++i) {
                const Elf32_Shdr section =
                    read<Elf32_Shdr>(header_.e_shoff + std::size_t(i) * sizeof(Elf32_Shdr));
                if (section.sh_type != SHT_NOBITS) {
                    span(section.sh_offset, section.sh_size);
                }
                sections_.push_back(section);
            }
        } catch (...) {
            munmap(const_cast<unsigned char*>(bytes_), size_);
            bytes_ = nullptr;
            throw;
        }
    }
    ~ElfImage() {
        if (bytes_ != nullptr) {
            munmap(const_cast<unsigned char*>(bytes_), size_);
        }
    }
    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;
    bool hardFloat() const {
        constexpr Elf32_Word hardFloatAbiFlag = 0x400;
        return (header_.e_flags & hardFloatAbiFlag) != 0;
    }
    unsigned type() const {
        return header_.e_type;
    }
    const std::vector<Elf32_Phdr>& loadSegments() const {
        return segments_;
    }
    std::string soname() const {
        for (const Elf32_Shdr& dynamic : sections_) {
            if (dynamic.sh_type != SHT_DYNAMIC) {
                continue;
            }
            if (dynamic.sh_entsize != sizeof(Elf32_Dyn) || dynamic.sh_link >= sections_.size()) {
                throw std::runtime_error(std::string("Invalid ELF dynamic table"));
            }
            for (std::size_t i = 0; i < dynamic.sh_size / sizeof(Elf32_Dyn); ++i) {
                const Elf32_Dyn entry = read<Elf32_Dyn>(dynamic.sh_offset + i * sizeof(Elf32_Dyn));
                if (entry.d_tag == DT_SONAME) {
                    Elf32_Sym fake = {};
                    fake.st_name = entry.d_un.d_val;
                    return name(dynamic, fake);
                }
            }
        }
        return std::string();
    }
    const unsigned char* span(std::size_t offset, std::size_t size) const {
        if (offset > size_ || size > size_ - offset) {
            throw std::runtime_error(std::string("ELF offset outside file"));
        }
        return bytes_ + offset;
    }
    template <class T> T read(std::size_t offset) const {
        T result;
        std::memcpy(&result, span(offset, sizeof(T)), sizeof(T));
        return result;
    }
    const unsigned char*
    at(std::uint32_t address, std::size_t size, bool executable = false) const {
        for (const auto& segment : segments_) {
            if (segment.p_type == PT_LOAD && (!executable || (segment.p_flags & PF_X) != 0) &&
                address >= segment.p_vaddr && address - segment.p_vaddr <= segment.p_filesz &&
                size <= segment.p_filesz - (address - segment.p_vaddr)) {
                return span(segment.p_offset + std::size_t(address - segment.p_vaddr), size);
            }
        }
        throw std::runtime_error(std::string("ELF virtual address outside load segment"));
    }
    std::uint32_t word(std::uint32_t address) const {
        std::uint32_t value;
        std::memcpy(&value, at(address, 4), 4);
        return value;
    }
    std::string name(const Elf32_Shdr& symbols, const Elf32_Sym& symbol) const {
        if (symbols.sh_link >= sections_.size()) {
            throw std::runtime_error(std::string("Invalid ELF string table"));
        }
        const Elf32_Shdr& strings = sections_[symbols.sh_link];
        if (symbol.st_name >= strings.sh_size) {
            throw std::runtime_error(std::string("Invalid ELF symbol name"));
        }
        const char* start = reinterpret_cast<const char*>(
            span(strings.sh_offset + symbol.st_name, strings.sh_size - symbol.st_name));
        const void* end = std::memchr(start, 0, strings.sh_size - symbol.st_name);
        if (end == nullptr) {
            throw std::runtime_error(std::string("Unterminated ELF symbol name"));
        }
        return std::string(start, static_cast<const char*>(end) - start);
    }
    bool symbol(const std::string& wanted, Elf32_Sym* output, bool defined = true) const {
        unsigned matches = 0;
        for (const auto& table : sections_) {
            if (table.sh_type != SHT_DYNSYM) {
                continue;
            }
            if (table.sh_entsize != sizeof(Elf32_Sym)) {
                throw std::runtime_error(std::string("Invalid ELF symbol table"));
            }
            for (std::size_t i = 0; i < table.sh_size / sizeof(Elf32_Sym); ++i) {
                const Elf32_Sym value = read<Elf32_Sym>(table.sh_offset + i * sizeof(Elf32_Sym));
                if ((value.st_shndx != SHN_UNDEF) == defined && name(table, value) == wanted) {
                    *output = value;
                    ++matches;
                }
            }
        }
        if (matches > 1) {
            throw std::runtime_error("Ambiguous ELF symbol: " + wanted);
        }
        return matches == 1;
    }
    std::vector<std::string> slotNames(std::uint32_t address) const {
        std::vector<std::string> result;
        for (const Elf32_Shdr& rels : sections_) {
            if (rels.sh_type != SHT_REL) {
                continue;
            }
            if (rels.sh_entsize != sizeof(Elf32_Rel) || rels.sh_link >= sections_.size()) {
                throw std::runtime_error(std::string("Invalid ELF relocation table"));
            }
            const Elf32_Shdr& table = sections_[rels.sh_link];
            for (std::size_t i = 0; i < rels.sh_size / sizeof(Elf32_Rel); ++i) {
                const Elf32_Rel rel = read<Elf32_Rel>(rels.sh_offset + i * sizeof(Elf32_Rel));
                const std::size_t index = ELF32_R_SYM(rel.r_info);
                if (rel.r_offset != address || index == 0) {
                    continue;
                }
                if (index >= table.sh_size / sizeof(Elf32_Sym)) {
                    throw std::runtime_error(std::string("Invalid relocation symbol"));
                }
                result.push_back(
                    name(table, read<Elf32_Sym>(table.sh_offset + index * sizeof(Elf32_Sym))));
                return result;
            }
        }
        const std::uint32_t target = word(address);
        for (const auto& table : sections_) {
            if (table.sh_type != SHT_DYNSYM) {
                continue;
            }
            for (std::size_t i = 0; i < table.sh_size / sizeof(Elf32_Sym); ++i) {
                const Elf32_Sym value = read<Elf32_Sym>(table.sh_offset + i * sizeof(Elf32_Sym));
                if (value.st_shndx != SHN_UNDEF && value.st_value == target) {
                    result.push_back(name(table, value));
                }
            }
        }
        return result;
    }

private:
    const unsigned char* bytes_{nullptr};
    std::size_t size_{0};
    Elf32_Ehdr header_;
    std::vector<Elf32_Phdr> segments_;
    std::vector<Elf32_Shdr> sections_;
};
} // namespace kindle_epub
#endif
