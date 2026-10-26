#include "relay/core/pak.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace relay {
namespace {

constexpr char magic[8] = {'R', 'E', 'L', 'A', 'Y', 'P', 'A', 'K'};
constexpr std::size_t header_size = 40;
constexpr std::size_t alignment = 16;

std::uint32_t crc32_of(const std::uint8_t* data, const std::size_t size, std::uint32_t crc = 0xFFFFFFFFU) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value & 1U) ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
            values[i] = value;
        }
        return values;
    }();
    for (std::size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    return crc;
}

template <typename T>
void put(std::string& out, const T value, const std::size_t bytes = sizeof(T)) {
    for (std::size_t i = 0; i < bytes; ++i) out += static_cast<char>((static_cast<std::uint64_t>(value) >> (8U * i)) & 0xFFU);
}

std::uint64_t get(const std::uint8_t* at, const std::size_t bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) value |= static_cast<std::uint64_t>(at[i]) << (8U * i);
    return value;
}

} // namespace

bool write_pak(const std::filesystem::path& output, const std::vector<PakInput>& inputs, std::string& error) {
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot write " + output.string();
        return false;
    }
    std::string header(header_size, '\0');
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::uint64_t position = header_size;
    std::vector<PakEntry> entries;
    entries.reserve(inputs.size());
    const std::array<char, alignment> zeros{};
    std::array<char, 65536> buffer{};
    for (const auto& input : inputs) {
        if (input.name.empty() || input.name.size() > 0xFFFFU) {
            error = "a packed file has an unusable name";
            return false;
        }
        if (const auto pad = (alignment - position % alignment) % alignment) {
            stream.write(zeros.data(), static_cast<std::streamsize>(pad));
            position += pad;
        }
        PakEntry entry;
        entry.name = input.name;
        entry.offset = position;
        std::uint32_t crc = 0xFFFFFFFFU;
        if (input.source.empty()) {
            stream.write(input.text.data(), static_cast<std::streamsize>(input.text.size()));
            crc = crc32_of(reinterpret_cast<const std::uint8_t*>(input.text.data()), input.text.size(), crc);
            entry.size = input.text.size();
        } else {
            std::ifstream file(input.source, std::ios::binary);
            if (!file) {
                error = "cannot read " + input.source.string();
                return false;
            }
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const auto count = static_cast<std::size_t>(file.gcount());
                if (count == 0U) break;
                stream.write(buffer.data(), static_cast<std::streamsize>(count));
                crc = crc32_of(reinterpret_cast<const std::uint8_t*>(buffer.data()), count, crc);
                entry.size += count;
            }
        }
        entry.crc32 = ~crc;
        position += entry.size;
        entries.push_back(std::move(entry));
    }
    std::string table;
    for (const auto& entry : entries) {
        put<std::uint16_t>(table, static_cast<std::uint16_t>(entry.name.size()));
        put<std::uint64_t>(table, entry.offset);
        put<std::uint64_t>(table, entry.size);
        put<std::uint32_t>(table, entry.crc32);
        table += entry.name;
    }
    const auto table_offset = position;
    stream.write(table.data(), static_cast<std::streamsize>(table.size()));
    header.clear();
    header.append(magic, sizeof magic);
    put<std::uint32_t>(header, pak_version);
    put<std::uint32_t>(header, 0U);
    put<std::uint64_t>(header, entries.size());
    put<std::uint64_t>(header, table_offset);
    put<std::uint64_t>(header, table.size());
    stream.seekp(0);
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    stream.flush();
    if (!stream) {
        error = "cannot finish " + output.string();
        return false;
    }
    error.clear();
    return true;
}

std::shared_ptr<const PakArchive> PakArchive::open(const std::filesystem::path& path, std::string& error) {
    std::shared_ptr<PakArchive> archive(new PakArchive());
#ifndef _WIN32
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || status.st_size < static_cast<off_t>(header_size)) {
        ::close(descriptor);
        error = path.string() + " is not a game archive";
        return nullptr;
    }
    void* mapped = ::mmap(nullptr, static_cast<std::size_t>(status.st_size), PROT_READ, MAP_PRIVATE, descriptor, 0);
    ::close(descriptor);
    if (mapped == MAP_FAILED) {
        error = "cannot map " + path.string();
        return nullptr;
    }
    archive->mapping_ = mapped;
    archive->mapped_size_ = static_cast<std::size_t>(status.st_size);
    archive->data_ = static_cast<const std::uint8_t*>(mapped);
    archive->size_ = archive->mapped_size_;
#else
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open " + path.string();
        return nullptr;
    }
    archive->owned_.resize(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(archive->owned_.data()), static_cast<std::streamsize>(archive->owned_.size()));
    archive->data_ = archive->owned_.data();
    archive->size_ = archive->owned_.size();
#endif
    if (archive->size_ < header_size || std::memcmp(archive->data_, magic, sizeof magic) != 0 ||
        get(archive->data_ + 8, 4) != pak_version) {
        error = path.string() + " is not a game archive this Relay can read";
        return nullptr;
    }
    const auto count = get(archive->data_ + 16, 8);
    const auto table_offset = get(archive->data_ + 24, 8);
    const auto table_size = get(archive->data_ + 32, 8);
    if (table_offset > archive->size_ || table_size > archive->size_ - table_offset) {
        error = path.string() + " is damaged: its file table is out of range";
        return nullptr;
    }
    const auto* at = archive->data_ + table_offset;
    const auto* end = at + table_size;
    archive->entries_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 1U << 20U)));
    for (std::uint64_t index = 0; index < count; ++index) {
        if (static_cast<std::size_t>(end - at) < 22U) {
            error = path.string() + " is damaged: its file table is cut short";
            return nullptr;
        }
        PakEntry entry;
        const auto name_size = static_cast<std::size_t>(get(at, 2));
        entry.offset = get(at + 2, 8);
        entry.size = get(at + 10, 8);
        entry.crc32 = static_cast<std::uint32_t>(get(at + 18, 4));
        at += 22;
        if (static_cast<std::size_t>(end - at) < name_size || entry.offset > table_offset ||
            entry.size > table_offset - entry.offset) {
            error = path.string() + " is damaged: a file lies outside it";
            return nullptr;
        }
        entry.name.assign(reinterpret_cast<const char*>(at), name_size);
        at += name_size;
        archive->entries_.push_back(std::move(entry));
    }
    std::sort(archive->entries_.begin(), archive->entries_.end(),
              [](const PakEntry& a, const PakEntry& b) { return a.name < b.name; });
    error.clear();
    return archive;
}

PakArchive::~PakArchive() {
#ifndef _WIN32
    if (mapping_ != nullptr) ::munmap(mapping_, mapped_size_);
#endif
}

std::optional<std::span<const std::uint8_t>> PakArchive::find(const std::string_view name) const {
    const auto found = std::lower_bound(entries_.begin(), entries_.end(), name,
                                        [](const PakEntry& entry, const std::string_view value) { return entry.name < value; });
    if (found == entries_.end() || found->name != name) return std::nullopt;
    return std::span<const std::uint8_t>{data_ + found->offset, static_cast<std::size_t>(found->size)};
}

bool PakArchive::verify(std::string& error) const {
    for (const auto& entry : entries_)
        if (~crc32_of(data_ + entry.offset, static_cast<std::size_t>(entry.size)) != entry.crc32) {
            error = entry.name + " does not match its checksum";
            return false;
        }
    error.clear();
    return true;
}

namespace vfs {
namespace {
struct Mount {
    std::shared_ptr<const PakArchive> archive;
    std::filesystem::path root;
};
std::shared_ptr<const Mount> current;
std::mutex guard;

std::shared_ptr<const Mount> snapshot() {
    std::lock_guard lock(guard);
    return current;
}
} // namespace

void mount(std::shared_ptr<const PakArchive> archive, const std::filesystem::path& root) {
    auto next = std::make_shared<Mount>();
    next->archive = std::move(archive);
    next->root = std::filesystem::absolute(root).lexically_normal();
    std::lock_guard lock(guard);
    current = std::move(next);
}

void unmount() {
    std::lock_guard lock(guard);
    current.reset();
}

bool mounted() { return snapshot() != nullptr; }

std::optional<std::span<const std::uint8_t>> packed(const std::filesystem::path& path) {
    const auto mount = snapshot();
    if (!mount) return std::nullopt;
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec) return std::nullopt;
    const auto relative = absolute.lexically_relative(mount->root);
    if (relative.empty() || *relative.begin() == ".." || relative == ".") return std::nullopt;
    return mount->archive->find(relative.generic_string());
}

std::optional<std::span<const std::uint8_t>> packed_name(const std::string_view name) {
    const auto mount = snapshot();
    return mount ? mount->archive->find(name) : std::nullopt;
}

Stamp stamp(const std::filesystem::path& path) {
    Stamp result;
    if (const auto bytes = packed(path)) {
        result.found = true;
        result.size = bytes->size();
        return result;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return result;
    result.time = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    result.size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    result.found = true;
    return result;
}

bool read_all(const std::filesystem::path& path, const std::uintmax_t maximum, std::string& text,
              std::string& error) {
    if (const auto bytes = packed(path)) {
        if (bytes->size() > maximum) {
            error = "file is too large";
            return false;
        }
        text.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return true;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "file not found";
        return false;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > maximum) {
        error = ec ? "could not read the file size" : "file is too large";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    text.assign(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(size)) && size != 0U) {
        error = "could not read the file";
        return false;
    }
    return true;
}
} // namespace vfs

} // namespace relay
