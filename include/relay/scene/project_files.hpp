#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// File operations for the editor's asset browser. Every path is relative to the asset root (the
// open project folder) and must pass workspace_file(): no traversal, hidden components or symlinks.
// An empty directory names the root itself.

inline constexpr std::size_t maximum_asset_listing = 4096U;
// Deleted entries move here, so a deletion can be recovered by hand. Hidden folders are excluded
// from listings and project packages.
inline constexpr std::string_view asset_trash_directory = ".relay-trash";

inline constexpr std::size_t maximum_asset_search_results = 512U;

// Coarse file categories for browsing and filtering, decided by name alone.
enum class AssetKind : std::uint8_t {
    folder, model, scene, node_template, image, shader, script, text, media, project, other
};
[[nodiscard]] std::string_view asset_kind_name(AssetKind kind);
[[nodiscard]] std::optional<AssetKind> asset_kind_from_name(std::string_view name);
[[nodiscard]] AssetKind asset_kind_of(std::string_view filename, bool directory);

struct AssetFileEntry {
    std::string name;
    std::string path; // Root-relative, generic separators.
    bool directory{};
    std::uintmax_t size{};
    AssetKind kind{AssetKind::other};
};

struct AssetDirectoryListing {
    std::vector<AssetFileEntry> entries; // Folders first, then files, each sorted by name.
    bool truncated{};
};

[[nodiscard]] std::optional<AssetDirectoryListing> list_asset_directory(
    const std::filesystem::path& root, std::string_view directory, std::string& error);
// Searches the whole tree below the root for names containing `query` (case-insensitive) whose kind
// is in `kinds` (any kind when empty). Hidden entries and symlinks are skipped. Results are sorted by
// path and bounded; truncated reports a partial result.
[[nodiscard]] AssetDirectoryListing search_assets(const std::filesystem::path& root,
                                                  std::string_view query,
                                                  const std::vector<AssetKind>& kinds);
[[nodiscard]] bool create_asset_folder(const std::filesystem::path& root, std::string_view path,
                                       std::string& error);
// Renames or moves a file or folder. The destination must not exist; folders cannot move into
// themselves.
[[nodiscard]] bool move_asset(const std::filesystem::path& root, std::string_view from,
                              std::string_view to, std::string& error);
// Moves a file or folder into the project trash and returns its trash-relative location.
[[nodiscard]] std::optional<std::string> delete_asset(const std::filesystem::path& root,
                                                      std::string_view path, std::string& error);

} // namespace relay
