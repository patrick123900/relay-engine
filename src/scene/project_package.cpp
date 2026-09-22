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

struct Entry {
    std::filesystem::path source;
    std::string name;
    std::uint64_t size{};
};

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
    return !ec && size <= 512ULL * 1024ULL * 1024ULL;
}

void octal(char* target, const std::size_t width, std::uint64_t value) {
    std::memset(target, '0', width);
    target[width - 1] = '\0';
    for (std::size_t i = width - 1; i > 0 && value; --i) {
        target[i - 1] = static_cast<char>('0' + value % 8U);
        value /= 8U;
    }
}

bool write_entry(std::ofstream& output, const Entry& entry,
                 const std::string* generated = nullptr) {
    std::array<char, 512> header{};
    if (entry.name.size() > 100) return false;
    std::memcpy(header.data(), entry.name.data(), entry.name.size());
    octal(header.data() + 100, 8, 0644);
    octal(header.data() + 108, 8, 0);
    octal(header.data() + 116, 8, 0);
    octal(header.data() + 124, 12, entry.size);
    octal(header.data() + 136, 12, 0);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = '0';
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    unsigned checksum = 0;
    for (const auto value : header) checksum += static_cast<unsigned char>(value);
    octal(header.data() + 148, 8, checksum);
    header[155] = ' ';
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (generated) {
        output.write(generated->data(), static_cast<std::streamsize>(generated->size()));
    } else {
        std::ifstream input(entry.source, std::ios::binary);
        if (!input) return false;
        std::array<char, 65536> buffer{};
        auto remaining = entry.size;
        while (remaining) {
            const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, buffer.size()));
            input.read(buffer.data(), count);
            if (input.gcount() != count) return false;
            output.write(buffer.data(), count);
            remaining -= static_cast<std::uint64_t>(count);
        }
    }
    const std::array<char, 512> padding{};
    const auto extra = static_cast<std::size_t>((512U - entry.size % 512U) % 512U);
    output.write(padding.data(), static_cast<std::streamsize>(extra));
    return static_cast<bool>(output);
}

} // namespace

bool package_project(const Project& project, const std::string_view filename,
                     std::string& error, std::uint64_t& bytes) {
    bytes = 0;
    const auto output_path = workspace_file((project.root() / "exports").generic_string(), filename, ".tar");
    if (!output_path || std::filesystem::path(filename).has_parent_path()) {
        error = "unsafe package filename";
        return false;
    }
    const auto root = project.root();
    std::vector<Entry> entries;
    for (const auto& scene : project.scenes) {
        const auto relative = std::filesystem::path("scenes") / scene;
        std::uint64_t size{};
        if (!workspace_file((root / "scenes").generic_string(), scene, ".relay.json") ||
            !safe_source(root, relative, size)) {
            error = "project scene is missing, unsafe or too large: " + scene;
            return false;
        }
        entries.push_back({root / relative, relative.generic_string(), size});
    }
    std::error_code ec;
    if (std::filesystem::exists(root, ec)) {
        if (std::filesystem::is_symlink(root, ec) || !std::filesystem::is_directory(root, ec) || ec) {
            error = "unsafe project directory";
            return false;
        }
        for (std::filesystem::recursive_directory_iterator it(root, ec), end;
             !ec && it != end; it.increment(ec)) {
            const auto relative = it->path().lexically_relative(root);
            const auto first = (*relative.begin()).string();
            if (first == "scenes" || first == "exports" || first == "captures" ||
                first == "traces" || first == ".relay") {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            const auto name = it->path().filename().string();
            if (it->is_symlink(ec) || ec) {
                error = "project assets contain a symlink";
                return false;
            }
            if ((name.starts_with('.') && name != ".relay-imports.json") ||
                (it.depth() == 0 && name.ends_with(".relayproject"))) {
                if (it->is_directory(ec)) it.disable_recursion_pending();
                continue;
            }
            if (it->is_directory(ec)) continue;
            if (!it->is_regular_file(ec) || ec) { error = "unsupported asset entry"; return false; }
            std::uint64_t size{};
            if (!safe_source(root, relative, size)) { error = "unsafe or oversized asset"; return false; }
            entries.push_back({it->path(), relative.generic_string(), size});
            if (entries.size() > 4096) { error = "too many package files"; return false; }
        }
        if (ec) { error = "cannot inspect project assets"; return false; }
    } else { error = "project directory is missing"; return false; }
    Project portable = project;
    portable.filename = "project.relayproject";
    const auto metadata = portable.json() + '\n';
    entries.push_back({{}, "project.relayproject", metadata.size()});
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.name < b.name;
    });
    std::uint64_t total = 1024;
    for (const auto& entry : entries) {
        if (entry.name.size() > 100 || entry.name.empty() ||
            total > 2ULL * 1024ULL * 1024ULL * 1024ULL - 512 - ((entry.size + 511) / 512) * 512) {
            error = "package path or total size exceeds limit";
            return false;
        }
        total += 512 + ((entry.size + 511) / 512) * 512;
    }
    if (std::filesystem::exists(*output_path, ec) || ec) {
        error = "package already exists";
        return false;
    }
    std::filesystem::create_directories(output_path->parent_path(), ec);
    if (ec) { error = "cannot create exports directory"; return false; }
    static std::atomic<std::uint64_t> serial{};
    const auto temporary = std::filesystem::path(output_path->string() + "." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(++serial) + ".tmp");
    bool success = false;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        success = static_cast<bool>(output);
        for (const auto& entry : entries) {
            if (!success) break;
            success = write_entry(output, entry, entry.name == "project.relayproject" ? &metadata : nullptr);
        }
        const std::array<char, 1024> trailer{};
        if (success) output.write(trailer.data(), static_cast<std::streamsize>(trailer.size()));
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
    bytes = total;
    error.clear();
    return true;
}

} // namespace relay
