#pragma once

#include <filesystem>
#include <string>

namespace relay {

// The newest versions of the C and C++ runtimes a Linux program or library asks of the system, read
// from its ELF symbol version needs (what `objdump -T` shows as GLIBC_2.38). They decide which
// distributions can run it: a program that needs GLIBC_2.43 will not start on one with glibc 2.39.
struct ElfRequirements {
    std::string glibc;     // "2.38", or empty when it asks nothing of glibc.
    std::string libstdcxx; // "3.4.32" from GLIBCXX_3.4.32.
    std::string cxxabi;    // "1.3.15" from CXXABI_1.3.15.
};

// Reads a 64-bit little-endian ELF file. False, with `error`, for anything else.
[[nodiscard]] bool read_elf_requirements(const std::filesystem::path& file, ElfRequirements& requirements,
                                         std::string& error);

// Dotted version numbers compared piece by piece: -1, 0 or 1. Empty is older than anything.
[[nodiscard]] int compare_dotted_versions(const std::string& a, const std::string& b);

// Raises `into` to whatever `other` needs.
void merge_requirements(ElfRequirements& into, const ElfRequirements& other);

} // namespace relay
