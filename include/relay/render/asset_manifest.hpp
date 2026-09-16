#pragma once

#include "relay/render/assets.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace relay {

inline constexpr std::uint32_t import_manifest_version = 3;

// Bump when a change to the importer would produce different geometry or identities from the same
// source file. Recorded per entry so a stale cache is detectable rather than silently trusted.
inline constexpr std::uint32_t model_importer_version = 5;

struct ImportManifestEntry {
    std::string source;
    std::uint32_t importer_version{model_importer_version};
    std::string content_id;
    std::vector<std::string> dependencies;
    std::vector<std::string> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> textures;
    std::string preset{"scene"};
    std::vector<std::string> nodes{};
    std::vector<std::string> clips{};
};

struct ImportReloadReport {
    std::size_t restored{};
    std::size_t changed{};
    std::size_t failed{};
    std::size_t rebound{};
    std::vector<std::string> messages;
    std::vector<std::pair<std::string, std::string>> asset_remaps;

    [[nodiscard]] std::string json() const;
    // Rebinds saved mesh-renderer ids after a source or importer version changed.
    std::size_t rebind_scene(Scene& scene);
};

// A project-local record of every model import, sufficient to rebuild the imported half of an
// asset registry in a fresh process. Stored beside the models it describes.
class ImportManifest {
public:
    [[nodiscard]] static std::filesystem::path path_for(const std::filesystem::path& assets_root);

    // A missing manifest loads as empty and succeeds; only malformed content is an error.
    [[nodiscard]] bool load(const std::filesystem::path& assets_root, std::string& error);
    [[nodiscard]] bool save(const std::filesystem::path& assets_root, std::string& error) const;

    // Replaces any existing entry with the same source filename.
    void record(ImportManifestEntry entry);

    [[nodiscard]] const std::vector<ImportManifestEntry>& entries() const;

private:
    std::vector<ImportManifestEntry> entries_;
};

// Re-imports every model the manifest records, so asset IDs saved inside a scene resolve again
// after a restart. Entries whose content changed are reported rather than silently re-pointed.
[[nodiscard]] ImportReloadReport reload_imported_assets(const std::filesystem::path& assets_root,
                                                        AssetRegistry& registry);

} // namespace relay
