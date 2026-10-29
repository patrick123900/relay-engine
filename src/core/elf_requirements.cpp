#include "relay/core/elf_requirements.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace relay {
namespace {

constexpr std::uint32_t sht_gnu_verneed = 0x6ffffffe;

template <typename T>
bool read_at(const std::vector<std::uint8_t>& bytes, const std::uint64_t offset, T& out) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T)) return false;
    std::memcpy(&out, bytes.data() + offset, sizeof(T));
    return true;
}

struct SectionHeader {
    std::uint32_t name, type;
    std::uint64_t flags, address, offset, size;
    std::uint32_t link, info;
    std::uint64_t alignment, entry_size;
};

std::vector<std::uint64_t> version_parts(const std::string& version) {
    std::vector<std::uint64_t> parts;
    std::uint64_t current = 0;
    bool any = false;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            current = current * 10U + static_cast<std::uint64_t>(c - '0');
            any = true;
        } else if (c == '.') {
            parts.push_back(current);
            current = 0;
            any = false;
        }
    }
    if (any) parts.push_back(current);
    return parts;
}

} // namespace

int compare_dotted_versions(const std::string& a, const std::string& b) {
    const auto left = version_parts(a), right = version_parts(b);
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        const auto x = i < left.size() ? left[i] : 0U, y = i < right.size() ? right[i] : 0U;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

void merge_requirements(ElfRequirements& into, const ElfRequirements& other) {
    if (!other.glibc.empty() && compare_dotted_versions(other.glibc, into.glibc) > 0) into.glibc = other.glibc;
    if (!other.libstdcxx.empty() && compare_dotted_versions(other.libstdcxx, into.libstdcxx) > 0) into.libstdcxx = other.libstdcxx;
    if (!other.cxxabi.empty() && compare_dotted_versions(other.cxxabi, into.cxxabi) > 0) into.cxxabi = other.cxxabi;
}

bool read_elf_requirements(const std::filesystem::path& file, ElfRequirements& requirements, std::string& error) {
    requirements = {};
    std::ifstream stream(file, std::ios::binary);
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (!stream || ec || size < 64U || size > 2ULL * 1024ULL * 1024ULL * 1024ULL) {
        error = "cannot read " + file.string();
        return false;
    }
    // Anything that is not an ELF file is refused before it is read in.
    std::uint8_t head[6]{};
    stream.read(reinterpret_cast<char*>(head), sizeof head);
    if (!stream || std::memcmp(head, "\x7f" "ELF", 4) != 0 || head[4] != 2 || head[5] != 1) {
        error = file.filename().string() + " is not a 64-bit little-endian ELF file";
        return false;
    }
    stream.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0 || bytes[4] != 2 || bytes[5] != 1) {
        error = file.filename().string() + " is not a 64-bit little-endian ELF file";
        return false;
    }
    std::uint64_t section_offset = 0;
    std::uint16_t entry_size = 0, count = 0;
    if (!read_at(bytes, 0x28, section_offset) || !read_at(bytes, 0x3A, entry_size) || !read_at(bytes, 0x3C, count) ||
        entry_size < sizeof(SectionHeader) - 0U || count == 0U) {
        // No section table (stripped of it): nothing to read, which asks nothing of the system as far as we can tell.
        return true;
    }
    const auto section = [&](const std::uint32_t index, SectionHeader& out) {
        const auto at = section_offset + static_cast<std::uint64_t>(index) * entry_size;
        std::uint32_t name{}, type{}, link{}, info{};
        std::uint64_t flags{}, address{}, offset{}, section_size{}, alignment{}, entry{};
        const bool ok = read_at(bytes, at, name) && read_at(bytes, at + 4, type) && read_at(bytes, at + 8, flags) &&
                        read_at(bytes, at + 16, address) && read_at(bytes, at + 24, offset) &&
                        read_at(bytes, at + 32, section_size) && read_at(bytes, at + 40, link) &&
                        read_at(bytes, at + 44, info) && read_at(bytes, at + 48, alignment) && read_at(bytes, at + 56, entry);
        out = {name, type, flags, address, offset, section_size, link, info, alignment, entry};
        return ok;
    };
    const auto string_at = [&](const SectionHeader& strings, const std::uint64_t offset) {
        std::string text;
        for (auto at = strings.offset + offset; at < bytes.size() && at < strings.offset + strings.size && bytes[at] != 0U; ++at)
            text += static_cast<char>(bytes[at]);
        return text;
    };
    for (std::uint32_t index = 0; index < count; ++index) {
        SectionHeader header{};
        if (!section(index, header) || header.type != sht_gnu_verneed) continue;
        SectionHeader strings{};
        if (!section(header.link, strings)) continue;
        // Verneed entries: version, count, file name, offset of the first Vernaux, offset of the next Verneed.
        std::uint64_t need = header.offset;
        for (std::uint32_t n = 0; n < header.info && need + 16U <= bytes.size(); ++n) {
            std::uint16_t aux_count{};
            std::uint32_t file_name{}, aux_offset{}, next{};
            if (!read_at(bytes, need + 2, aux_count) || !read_at(bytes, need + 4, file_name) ||
                !read_at(bytes, need + 8, aux_offset) || !read_at(bytes, need + 12, next))
                break;
            std::uint64_t aux = need + aux_offset;
            for (std::uint16_t a = 0; a < aux_count && aux + 16U <= bytes.size(); ++a) {
                std::uint32_t name{}, aux_next{};
                if (!read_at(bytes, aux + 8, name) || !read_at(bytes, aux + 12, aux_next)) break;
                const auto version = string_at(strings, name);
                const auto take = [&](const char* prefix, std::string& slot) {
                    const std::string head = prefix;
                    if (version.rfind(head, 0) == 0 && compare_dotted_versions(version.substr(head.size()), slot) > 0)
                        slot = version.substr(head.size());
                };
                take("GLIBC_", requirements.glibc);
                take("GLIBCXX_", requirements.libstdcxx);
                take("CXXABI_", requirements.cxxabi);
                if (aux_next == 0U) break;
                aux += aux_next;
            }
            if (next == 0U) break;
            need += next;
        }
    }
    error.clear();
    return true;
}

} // namespace relay
