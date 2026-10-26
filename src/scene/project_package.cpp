#include "relay/scene/project.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace relay {
namespace {

constexpr std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL;

bool safe_source(const std::filesystem::path& root, const std::filesystem::path& relative,
                 std::uint64_t& size) {
    std::error_code ec;
    auto current = root;
    if (std::filesystem::is_symlink(current, ec) || ec) return false;
    for (const auto& part : relative) {
        if (part == "." || part == ".." || part.empty()) return false;
        current /= part;
        if (std::filesystem::is_symlink(current, ec) || ec) return false;
    }
    if (!std::filesystem::is_regular_file(current, ec) || ec) return false;
    size = std::filesystem::file_size(current, ec);
    return !ec && size <= maximum_file_bytes;
}

// True when `relative` is `folder` or lies below it, compared by whole path components.
bool inside_folder(const std::filesystem::path& relative, const std::filesystem::path& folder) {
    auto path = relative.begin();
    for (auto part = folder.begin(); part != folder.end(); ++part, ++path)
        if (path == relative.end() || *path != *part) return false;
    return true;
}

void octal(char* target, const std::size_t width, std::uint64_t value) {
    std::memset(target, '0', width);
    target[width - 1] = '\0';
    for (std::size_t i = width - 1; i > 0 && value; --i) {
        target[i - 1] = static_cast<char>('0' + value % 8U);
        value /= 8U;
    }
}

// ustar keeps 100 characters of name plus a 155 character prefix, split at a slash.
bool split_tar_name(const std::string& name, std::string& prefix, std::string& base) {
    if (name.empty() || name.size() > 255U) return false;
    if (name.size() <= 100U) {
        prefix.clear();
        base = name;
        return true;
    }
    for (auto slash = name.rfind('/'); slash != std::string::npos && slash > 0;
         slash = name.rfind('/', slash - 1)) {
        if (slash <= 155U && name.size() - slash - 1U <= 100U) {
            prefix = name.substr(0, slash);
            base = name.substr(slash + 1U);
            return true;
        }
        if (slash == 0) break;
    }
    return false;
}

// Copies `size` bytes of the file to `output`, failing if the file changed length meanwhile.
bool copy_file_bytes(std::ofstream& output, const std::filesystem::path& source,
                     const std::uint64_t size) {
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::array<char, 65536> buffer{};
    auto remaining = size;
    while (remaining) {
        const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count) return false;
        output.write(buffer.data(), count);
        remaining -= static_cast<std::uint64_t>(count);
    }
    return static_cast<bool>(output);
}

bool write_tar_entry(std::ofstream& output, const ProjectFile& entry) {
    const std::string* generated = entry.source.empty() ? &entry.text : nullptr;
    std::array<char, 512> header{};
    std::string prefix;
    std::string base;
    if (!split_tar_name(entry.name, prefix, base)) return false;
    std::memcpy(header.data(), base.data(), base.size());
    octal(header.data() + 100, 8, 0644);
    octal(header.data() + 108, 8, 0);
    octal(header.data() + 116, 8, 0);
    octal(header.data() + 124, 12, entry.size);
    octal(header.data() + 136, 12, 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = '0';
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    std::memcpy(header.data() + 345, prefix.data(), prefix.size());
    unsigned checksum = 0;
    for (const auto value : header) checksum += static_cast<unsigned char>(value);
    octal(header.data() + 148, 8, checksum);
    header[155] = ' ';
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (generated) output.write(generated->data(), static_cast<std::streamsize>(generated->size()));
    else if (!copy_file_bytes(output, entry.source, entry.size)) return false;
    const std::array<char, 512> padding{};
    const auto extra = static_cast<std::size_t>((512U - entry.size % 512U) % 512U);
    output.write(padding.data(), static_cast<std::streamsize>(extra));
    return static_cast<bool>(output);
}

std::uint32_t crc32_update(std::uint32_t crc, const char* data, const std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto value = i;
            for (int bit = 0; bit < 8; ++bit) value = (value & 1U) ? 0xEDB88320U ^ (value >> 1U) : value >> 1U;
            values[i] = value;
        }
        return values;
    }();
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xFFU] ^ (crc >> 8U);
    return crc;
}

bool file_crc32(const std::filesystem::path& source, const std::uint64_t size, std::uint32_t& crc) {
    std::ifstream input(source, std::ios::binary);
    if (!input) return false;
    std::array<char, 65536> buffer{};
    auto remaining = size;
    std::uint32_t value = 0xFFFFFFFFU;
    while (remaining) {
        const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), count);
        if (input.gcount() != count) return false;
        value = crc32_update(value, buffer.data(), static_cast<std::size_t>(count));
        remaining -= static_cast<std::uint64_t>(count);
    }
    crc = ~value;
    return true;
}

void put16(std::string& out, const std::uint16_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>(value >> 8U);
}

void put32(std::string& out, const std::uint32_t value) {
    put16(out, static_cast<std::uint16_t>(value & 0xFFFFU));
    put16(out, static_cast<std::uint16_t>(value >> 16U));
}

// A stored (uncompressed) zip: local headers with their data, then the central directory. Names
// are UTF-8 and the stamp is fixed at 1980-01-01 so packaging the same project gives the same bytes.
bool write_zip(std::ofstream& output, const std::vector<ProjectFile>& entries) {
    constexpr std::uint16_t dos_date = (0U << 9U) | (1U << 5U) | 1U;
    std::string directory;
    std::uint64_t offset = 0;
    for (const auto& entry : entries) {
        const bool generated = entry.source.empty();
        std::uint32_t crc{};
        if (generated) {
            crc = ~crc32_update(0xFFFFFFFFU, entry.text.data(), entry.text.size());
        } else if (!file_crc32(entry.source, entry.size, crc)) {
            return false;
        }
        std::string header;
        put32(header, 0x04034b50U);
        put16(header, 20);
        put16(header, 0x0800U);
        put16(header, 0);
        put16(header, 0);
        put16(header, dos_date);
        put32(header, crc);
        put32(header, static_cast<std::uint32_t>(entry.size));
        put32(header, static_cast<std::uint32_t>(entry.size));
        put16(header, static_cast<std::uint16_t>(entry.name.size()));
        put16(header, 0);
        header += entry.name;
        output.write(header.data(), static_cast<std::streamsize>(header.size()));
        if (generated) output.write(entry.text.data(), static_cast<std::streamsize>(entry.text.size()));
        else if (!copy_file_bytes(output, entry.source, entry.size)) return false;
        put32(directory, 0x02014b50U);
        put16(directory, 0x031EU); // Made on Unix by a 3.0 writer, so the mode below is honoured.
        put16(directory, 20);
        put16(directory, 0x0800U);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, dos_date);
        put32(directory, crc);
        put32(directory, static_cast<std::uint32_t>(entry.size));
        put32(directory, static_cast<std::uint32_t>(entry.size));
        put16(directory, static_cast<std::uint16_t>(entry.name.size()));
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0100644U << 16U);
        put32(directory, static_cast<std::uint32_t>(offset));
        directory += entry.name;
        offset += header.size() + entry.size;
    }
    std::string end;
    put32(end, 0x06054b50U);
    put16(end, 0);
    put16(end, 0);
    put16(end, static_cast<std::uint16_t>(entries.size()));
    put16(end, static_cast<std::uint16_t>(entries.size()));
    put32(end, static_cast<std::uint32_t>(directory.size()));
    put32(end, static_cast<std::uint32_t>(offset));
    put16(end, 0);
    output.write(directory.data(), static_cast<std::streamsize>(directory.size()));
    output.write(end.data(), static_cast<std::streamsize>(end.size()));
    return static_cast<bool>(output);
}

} // namespace

bool collect_project_files(const Project& project, const std::vector<std::string>& excluded,
                           std::vector<ProjectFile>& files, std::string& metadata,
                           std::string& error) {
    files.clear();
    const auto root = project.root();
    for (const auto& scene : project.scenes) {
        const auto relative = std::filesystem::path("scenes") / scene;
        std::uint64_t size{};
        if (!workspace_file((root / "scenes").generic_string(), scene, ".relay.json") ||
            !safe_source(root, relative, size)) {
            error = "project scene is missing, unsafe or too large: " + scene;
            return false;
        }
    }
    std::vector<std::filesystem::path> skipped{"exports", "captures", "traces", ".relay"};
    for (const auto& folder : excluded) skipped.emplace_back(folder);
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) {
        error = "project directory is missing";
        return false;
    }
    if (std::filesystem::is_symlink(root, ec) || !std::filesystem::is_directory(root, ec) || ec) {
        error = "unsafe project directory";
        return false;
    }
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end;
         it.increment(ec)) {
        const auto relative = it->path().lexically_relative(root);
        if (std::any_of(skipped.begin(), skipped.end(), [&](const auto& folder) {
                return inside_folder(relative, folder);
            })) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const auto name = it->path().filename().string();
        if (it->is_symlink(ec) || ec) {
            error = "project assets contain a symlink";
            return false;
        }
        if ((name.starts_with('.') && name != ".relay-imports.json") ||
            (it.depth() == 0 && name.ends_with(".relayproject")) || name.ends_with(".tmp")) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(ec)) continue;
        if (!it->is_regular_file(ec) || ec) {
            error = "unsupported asset entry";
            return false;
        }
        std::uint64_t size{};
        if (!safe_source(root, relative, size)) {
            error = "unsafe or oversized asset";
            return false;
        }
        files.push_back({it->path(), relative.generic_string(), size});
        if (files.size() >= maximum_package_files) {
            error = "too many package files";
            return false;
        }
    }
    if (ec) {
        error = "cannot inspect project assets";
        return false;
    }
    Project portable = project;
    portable.filename = "project.relayproject";
    metadata = portable.json() + '\n';
    files.push_back({{}, "project.relayproject", metadata.size(), metadata});
    std::sort(files.begin(), files.end(),
              [](const ProjectFile& a, const ProjectFile& b) { return a.name < b.name; });
    error.clear();
    return true;
}

bool package_project(const Project& project, const std::string_view filename,
                     const std::string_view directory, std::string& error, std::uint64_t& bytes,
                     std::filesystem::path* written) {
    bytes = 0;
    const bool zip = filename.ends_with(".zip");
    const auto checked_name = workspace_file(".", filename, zip ? ".zip" : ".tar");
    if (!checked_name || std::filesystem::path(filename).has_parent_path()) {
        error = "unsafe package filename";
        return false;
    }
    std::filesystem::path output_directory = project.root() / "exports";
    if (!directory.empty()) {
        const std::filesystem::path requested{directory};
        for (const auto& part : requested)
            if (part == "..") {
                error = "the export folder must not contain ..";
                return false;
            }
        if (directory.size() > 512U || directory.find('\0') != std::string_view::npos) {
            error = "unsafe export folder";
            return false;
        }
        output_directory = requested.is_absolute() ? requested : project.root() / requested;
    }
    const auto output_path = std::optional{output_directory / filename};
    std::vector<std::string> excluded{"builds"};
    if (project.build)
        for (const auto& profile : project.build->profiles)
            if (const auto inside = output_inside_project(project.root(), profile); !inside.empty())
                excluded.push_back(inside);
    std::vector<ProjectFile> entries;
    std::string metadata;
    if (!collect_project_files(project, excluded, entries, metadata, error)) return false;
    std::uint64_t total = zip ? 22 : 1024;
    for (const auto& entry : entries) {
        std::string prefix;
        std::string base;
        const bool name_fits = zip ? !entry.name.empty() && entry.name.size() <= 1024U
                                   : split_tar_name(entry.name, prefix, base);
        const auto cost = zip ? 76U + 2U * entry.name.size() + entry.size
                              : 512U + ((entry.size + 511U) / 512U) * 512U;
        if (!name_fits || total + cost > maximum_package_bytes) {
            error = "package path or total size exceeds limit";
            return false;
        }
        total += cost;
    }
    std::error_code ec;
    if (std::filesystem::exists(*output_path, ec) || ec) {
        error = "package already exists";
        return false;
    }
    std::filesystem::create_directories(output_path->parent_path(), ec);
    if (ec) { error = "cannot create the export folder"; return false; }
    static std::atomic<std::uint64_t> serial{};
    const auto temporary = std::filesystem::path(output_path->string() + "." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(++serial) + ".tmp");
    bool success = false;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        success = static_cast<bool>(output);
        if (success && zip) {
            success = write_zip(output, entries);
        } else {
            for (const auto& entry : entries) {
                if (!success) break;
                success = write_tar_entry(output, entry);
            }
            const std::array<char, 1024> trailer{};
            if (success) output.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
        }
        output.flush();
        success = success && static_cast<bool>(output);
    }
    if (!success) {
        std::filesystem::remove(temporary, ec);
        error = "cannot write package";
        return false;
    }
    // Linking within the same directory publishes the complete archive atomically and
    // fails if another export claimed the name meanwhile.
    std::filesystem::create_hard_link(temporary, *output_path, ec);
    if (ec) {
        const bool occupied = std::filesystem::exists(*output_path);
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        error = occupied ? "package already exists" : "cannot finalize package";
        return false;
    }
    std::filesystem::remove(temporary, ec);
    if (ec) {
        error = "cannot finalize package";
        return false;
    }
    std::error_code size_error;
    const auto written_size = std::filesystem::file_size(*output_path, size_error);
    bytes = size_error ? total : written_size;
    if (written) *written = *output_path;
    error.clear();
    return true;
}

} // namespace relay
