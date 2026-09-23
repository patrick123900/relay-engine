#include "relay/scene/project_files.hpp"
#include "relay/scene/project.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <cctype>
#include <chrono>

namespace relay {
namespace {

std::optional<std::filesystem::path> resolve(const std::filesystem::path& root,
                                             const std::string_view path, std::string& error) {
    if (path.empty()) return root;
    auto resolved = workspace_file(root.generic_string(), path, "");
    if (!resolved) error = "path must be a safe project-relative name without hidden parts";
    return resolved;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

constexpr std::array<std::string_view, 11> kind_names{
    "folder", "model", "scene", "template", "image", "shader", "script", "text", "media", "project",
    "other"};

} // namespace

std::string_view asset_kind_name(const AssetKind kind) {
    return kind_names[static_cast<std::size_t>(kind)];
}

std::optional<AssetKind> asset_kind_from_name(const std::string_view name) {
    for (std::size_t index = 0; index < kind_names.size(); ++index)
        if (kind_names[index] == name) return static_cast<AssetKind>(index);
    return std::nullopt;
}

AssetKind asset_kind_of(const std::string_view filename, const bool directory) {
    if (directory) return AssetKind::folder;
    const auto name = lowercase(std::string(filename));
    if (name.ends_with(".relay.json")) return AssetKind::scene;
    if (name.ends_with(".relay-template.json")) return AssetKind::node_template;
    const auto dot = name.rfind('.');
    const auto extension = dot == std::string::npos ? std::string{} : name.substr(dot + 1U);
    const auto any = [&](std::initializer_list<std::string_view> list) {
        return std::find(list.begin(), list.end(), extension) != list.end();
    };
    if (any({"gltf", "glb", "fbx", "obj", "dae", "blend"})) return AssetKind::model;
    if (any({"png", "jpg", "jpeg", "bmp", "tga", "hdr", "exr", "ktx", "ktx2", "webp"}))
        return AssetKind::image;
    if (any({"glsl", "vert", "frag", "comp", "geom", "tesc", "tese", "spv", "hlsl", "wgsl"}))
        return AssetKind::shader;
    if (any({"cpp", "cc", "cxx", "hpp", "h", "hh", "lua", "js", "ts", "py", "wasm"}))
        return AssetKind::script;
    if (any({"txt", "md", "json", "yaml", "yml", "toml", "csv", "ini", "log"})) return AssetKind::text;
    if (any({"webm", "mp4", "mkv", "wav", "ogg", "mp3", "flac"})) return AssetKind::media;
    if (extension == "relayproject") return AssetKind::project;
    return AssetKind::other;
}

AssetDirectoryListing search_assets(const std::filesystem::path& root, const std::string_view query,
                                    const std::vector<AssetKind>& kinds) {
    AssetDirectoryListing result;
    const auto needle = lowercase(std::string(query));
    std::error_code failure;
    if (!std::filesystem::is_directory(root, failure)) return result;
    // Visiting is bounded too, so a huge folder cannot stall the editor.
    constexpr std::size_t maximum_visited = 65536U;
    std::size_t visited = 0;
    std::filesystem::recursive_directory_iterator item{root, failure}, end;
    for (; !failure && item != end; item.increment(failure)) {
        if (++visited > maximum_visited) {
            result.truncated = true;
            break;
        }
        const auto name = item->path().filename().string();
        std::error_code status;
        if (name.starts_with('.') || item->is_symlink(status)) {
            if (item->is_directory(status)) item.disable_recursion_pending();
            continue;
        }
        const bool is_directory = item->is_directory(status);
        if (!is_directory && !item->is_regular_file(status)) continue;
        const auto relative = item->path().lexically_relative(root).generic_string();
        if (!workspace_file(root.generic_string(), relative, "")) {
            if (is_directory) item.disable_recursion_pending();
            continue;
        }
        const auto kind = asset_kind_of(name, is_directory);
        if (!kinds.empty() && std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) continue;
        if (!needle.empty() && lowercase(name).find(needle) == std::string::npos) continue;
        if (result.entries.size() == maximum_asset_search_results) {
            result.truncated = true;
            break;
        }
        result.entries.push_back({name, relative, is_directory,
                                  is_directory ? 0U : item->file_size(status), kind});
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const AssetFileEntry& a, const AssetFileEntry& b) {
        const auto left = lowercase(a.path), right = lowercase(b.path);
        return left != right ? left < right : a.path < b.path;
    });
    return result;
}

std::optional<AssetDirectoryListing> list_asset_directory(const std::filesystem::path& root,
                                                          const std::string_view directory,
                                                          std::string& error) {
    const auto folder = resolve(root, directory, error);
    if (!folder) return std::nullopt;
    AssetDirectoryListing listing;
    std::error_code failure;
    if (directory.empty() && !std::filesystem::exists(*folder, failure)) return listing;
    if (!std::filesystem::is_directory(*folder, failure)) {
        error = "folder does not exist";
        return std::nullopt;
    }
    for (std::filesystem::directory_iterator item{*folder, failure}, end;
         !failure && item != end; item.increment(failure)) {
        const auto name = item->path().filename().string();
        std::error_code status;
        if (name.starts_with('.') || item->is_symlink(status)) continue;
        const bool is_directory = item->is_directory(status);
        if (!is_directory && !item->is_regular_file(status)) continue;
        const auto relative = directory.empty() ? name : std::string(directory) + '/' + name;
        if (!workspace_file(root.generic_string(), relative, "")) continue;
        if (listing.entries.size() == maximum_asset_listing) {
            listing.truncated = true;
            break;
        }
        listing.entries.push_back({name, relative, is_directory,
                                   is_directory ? 0U : item->file_size(status),
                                   asset_kind_of(name, is_directory)});
    }
    if (failure) {
        error = "folder could not be read";
        return std::nullopt;
    }
    std::sort(listing.entries.begin(), listing.entries.end(),
              [](const AssetFileEntry& a, const AssetFileEntry& b) {
        if (a.directory != b.directory) return a.directory;
        const auto left = lowercase(a.name), right = lowercase(b.name);
        return left != right ? left < right : a.name < b.name;
    });
    return listing;
}

bool create_asset_folder(const std::filesystem::path& root, const std::string_view path,
                         std::string& error) {
    const auto target = resolve(root, path, error);
    if (!target || path.empty()) {
        if (error.empty()) error = "folder name is required";
        return false;
    }
    std::error_code failure;
    // A project-less workspace may not have created its asset folder yet.
    std::filesystem::create_directories(root, failure);
    if (!std::filesystem::is_directory(target->parent_path(), failure)) {
        error = "parent folder does not exist";
        return false;
    }
    if (std::filesystem::exists(*target, failure)) {
        error = "a file or folder with that name already exists";
        return false;
    }
    if (!std::filesystem::create_directory(*target, failure) || failure) {
        error = "folder could not be created";
        return false;
    }
    return true;
}

bool move_asset(const std::filesystem::path& root, const std::string_view from,
                const std::string_view to, std::string& error) {
    if (from.empty() || to.empty()) {
        error = "source and destination are required";
        return false;
    }
    const auto source = resolve(root, from, error);
    const auto destination = source ? resolve(root, to, error) : std::nullopt;
    if (!source || !destination) return false;
    std::error_code failure;
    if (!std::filesystem::exists(*source, failure)) {
        error = "source does not exist";
        return false;
    }
    if (std::filesystem::exists(*destination, failure)) {
        error = "a file or folder with that name already exists";
        return false;
    }
    if (std::string(to).starts_with(std::string(from) + '/')) {
        error = "a folder cannot move into itself";
        return false;
    }
    if (!std::filesystem::is_directory(destination->parent_path(), failure)) {
        error = "destination folder does not exist";
        return false;
    }
    std::filesystem::rename(*source, *destination, failure);
    if (failure) {
        error = "move failed: " + failure.message();
        return false;
    }
    return true;
}

std::optional<std::string> delete_asset(const std::filesystem::path& root,
                                        const std::string_view path, std::string& error) {
    const auto source = path.empty() ? std::nullopt : resolve(root, path, error);
    if (!source) {
        if (error.empty()) error = "path is required";
        return std::nullopt;
    }
    std::error_code failure;
    if (!std::filesystem::exists(*source, failure)) {
        error = "file or folder does not exist";
        return std::nullopt;
    }
    const auto trash = root / asset_trash_directory;
    std::filesystem::create_directories(trash, failure);
    if (failure || std::filesystem::is_symlink(trash, failure)) {
        error = "project trash is unavailable";
        return std::nullopt;
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto name = std::to_string(stamp) + '-' + source->filename().string();
    std::filesystem::rename(*source, trash / name, failure);
    if (failure) {
        error = "delete failed: " + failure.message();
        return std::nullopt;
    }
    return std::string(asset_trash_directory) + '/' + name;
}

} // namespace relay
