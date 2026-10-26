#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// A game's files in one archive: the project's files and what the build cooked from them. The
// file is mapped into memory, so a read is a view into it and big files (music) stream from it
// without being copied.
//
//     "RELAYPAK" | version u32 | flags u32 | entry count u64 | table offset u64 | table size u64
//     file data, each file starting on a 16 byte boundary
//     the table: per entry name length u16 | offset u64 | size u64 | crc32 u32 | name
//
// All numbers are little endian.
inline constexpr std::uint32_t pak_version = 1;

struct PakEntry {
    std::string name; // Relative to the archive's root, with forward slashes.
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint32_t crc32{};
};

// A file to put in an archive: from disk, or `text` when `source` is empty.
struct PakInput {
    std::string name;
    std::filesystem::path source;
    std::string text;
};

[[nodiscard]] bool write_pak(const std::filesystem::path& output, const std::vector<PakInput>& inputs,
                             std::string& error);

class PakArchive {
public:
    [[nodiscard]] static std::shared_ptr<const PakArchive> open(const std::filesystem::path& path,
                                                                std::string& error);
    ~PakArchive();
    PakArchive(const PakArchive&) = delete;
    PakArchive& operator=(const PakArchive&) = delete;

    [[nodiscard]] std::optional<std::span<const std::uint8_t>> find(std::string_view name) const;
    [[nodiscard]] const std::vector<PakEntry>& entries() const { return entries_; }
    // Checks every file against its checksum.
    [[nodiscard]] bool verify(std::string& error) const;

private:
    PakArchive() = default;
    std::vector<PakEntry> entries_;
    const std::uint8_t* data_{};
    std::size_t size_{};
    std::vector<std::uint8_t> owned_; // Where the file is read into when it cannot be mapped.
    void* mapping_{};
    std::size_t mapped_size_{};
};

// The archive the process reads project files from, and the folder they appear to be in. A path
// that falls inside `root` and names an entry reads from the archive; everything else is the file
// system as usual. The editor mounts nothing.
namespace vfs {
void mount(std::shared_ptr<const PakArchive> archive, const std::filesystem::path& root);
void unmount();
[[nodiscard]] bool mounted();
// The file's bytes when `path` is an entry of the mounted archive.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> packed(const std::filesystem::path& path);
// The mounted archive's entry named `name` (relative to its root), for cooked files that stand
// in for nothing on disk.
[[nodiscard]] std::optional<std::span<const std::uint8_t>> packed_name(std::string_view name);
// What a file looks like to code that caches by size and time: an archive entry has its size and no
// modification time; anything else is the file system's. `found` is false when there is no such
// regular file.
struct Stamp {
    bool found{};
    std::uintmax_t size{};
    std::filesystem::file_time_type time{};
};
[[nodiscard]] Stamp stamp(const std::filesystem::path& path);
// The whole file as text, from the archive or the disk; false when missing or over `maximum`.
[[nodiscard]] bool read_all(const std::filesystem::path& path, std::uintmax_t maximum, std::string& text,
                            std::string& error);
} // namespace vfs

} // namespace relay
