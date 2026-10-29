// Prints what C and C++ runtime versions Linux programs ask of the system, and can insist on a
// ceiling:   relay_elf_requirements [--max-glibc 2.31] file...
// Exits 1 when a file needs a newer glibc than --max-glibc. A portable build uses it to keep a
// player from quietly depending on the machine that built it.
#include "relay/core/elf_requirements.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string maximum;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-glibc") == 0 && i + 1 < argc) maximum = argv[++i];
        else files.emplace_back(argv[i]);
    }
    if (files.empty()) {
        std::cerr << "usage: relay_elf_requirements [--max-glibc VERSION] FILE...\n";
        return 2;
    }
    int status = 0;
    for (const auto& file : files) {
        relay::ElfRequirements requirements;
        std::string error;
        if (!relay::read_elf_requirements(file, requirements, error)) {
            std::cerr << error << '\n';
            status = 2;
            continue;
        }
        std::cout << file << ": glibc " << (requirements.glibc.empty() ? "-" : requirements.glibc) << ", libstdc++ "
                  << (requirements.libstdcxx.empty() ? "-" : requirements.libstdcxx) << ", cxxabi "
                  << (requirements.cxxabi.empty() ? "-" : requirements.cxxabi) << '\n';
        if (!maximum.empty() && relay::compare_dotted_versions(requirements.glibc, maximum) > 0) {
            std::cerr << file << " needs glibc " << requirements.glibc << ", newer than the " << maximum
                      << " this build allows; build in an environment with an older glibc\n";
            status = 1;
        }
    }
    return status;
}
