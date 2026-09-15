#pragma once

#include "relay/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace relay {

inline constexpr std::uint32_t bindless_texture_capacity = 16U;

struct MeshVertex {
    float x{};
    float y{};
    float z{};
    float u{};
    float v{};
};

struct MeshAsset {
    std::string name;
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::int32_t vertex_offset{};
};

struct MaterialAsset {
    std::string name;
    std::array<float, 4> color{};
    std::string texture;
};

struct TextureAsset {
    std::string name;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] std::span<const MeshVertex> built_in_mesh_vertices();
[[nodiscard]] std::span<const std::uint32_t> built_in_mesh_indices();
[[nodiscard]] std::span<const MeshAsset> built_in_meshes();
[[nodiscard]] std::span<const MaterialAsset> built_in_materials();
[[nodiscard]] std::span<const TextureAsset> built_in_textures();
[[nodiscard]] const MeshAsset* find_mesh_asset(std::string_view name);
[[nodiscard]] const MaterialAsset* find_material_asset(std::string_view name);
[[nodiscard]] const TextureAsset* find_texture_asset(std::string_view name);
[[nodiscard]] std::uint32_t texture_asset_index(std::string_view name);
[[nodiscard]] std::string render_assets_json();
[[nodiscard]] std::uint64_t render_asset_revision();

struct ModelImportResult {
    bool imported{};
    std::string content_id;
    std::string source_format;
    std::vector<std::string> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> warnings;
    std::vector<Entity> roots;

    [[nodiscard]] std::string json() const;
};

[[nodiscard]] std::string model_import_capabilities_json();
[[nodiscard]] ModelImportResult import_model_asset(const std::filesystem::path& path,
                                                    Scene* scene, std::string& error);

} // namespace relay
