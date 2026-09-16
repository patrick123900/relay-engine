#pragma once

#include "relay/render/blender_adapter.hpp"
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
    float nx{0.0F};
    float ny{0.0F};
    float nz{1.0F};
    float tx{1.0F};
    float ty{0.0F};
    float tz{0.0F};
    float tw{1.0F};
};

struct MeshAsset {
    std::string name;
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::int32_t vertex_offset{};
    // Local-space axis-aligned bounds over the vertices this mesh actually indexes. Computed by the
    // registry when the mesh is added; used for frustum culling.
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
};

struct MaterialAsset {
    std::string name;
    // glTF metallic-roughness material inputs. `color` and `texture` retain their original names
    // for scene/protocol compatibility and represent baseColorFactor/baseColorTexture.
    std::array<float, 4> color{};
    std::string texture;
    float metallic_factor{0.0F};
    float roughness_factor{1.0F};
    std::string metallic_roughness_texture;
    std::string normal_texture;
    float normal_scale{1.0F};
    std::string occlusion_texture;
    float occlusion_strength{1.0F};
    std::array<float, 3> emissive_factor{};
    std::string emissive_texture;
    enum class AlphaMode : std::uint8_t { opaque, mask, blend } alpha_mode{AlphaMode::opaque};
    float alpha_cutoff{0.5F};
    bool double_sided{false};
};

enum class TextureColorSpace : std::uint8_t { linear, srgb };
enum class TextureFilter : std::uint8_t { nearest, linear };
enum class TextureWrap : std::uint8_t { repeat, clamp_to_edge, mirrored_repeat };

struct TextureAsset {
    std::string name;
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
    TextureColorSpace color_space{TextureColorSpace::srgb};
    TextureFilter mag_filter{TextureFilter::linear};
    TextureFilter min_filter{TextureFilter::linear};
    TextureFilter mip_filter{TextureFilter::linear};
    TextureWrap wrap_u{TextureWrap::repeat};
    TextureWrap wrap_v{TextureWrap::repeat};
};

// Owns every mesh, material and texture a renderer may draw. An instance starts populated with
// Relay's built-in assets; imported content is appended. Registries are independent, so tests and
// future multi-project hosts can hold several without sharing mutable state.
class AssetRegistry {
public:
    AssetRegistry();

    [[nodiscard]] std::span<const MeshVertex> mesh_vertices() const;
    [[nodiscard]] std::span<const std::uint32_t> mesh_indices() const;
    [[nodiscard]] std::span<const MeshAsset> meshes() const;
    [[nodiscard]] std::span<const MaterialAsset> materials() const;
    [[nodiscard]] std::span<const TextureAsset> textures() const;

    [[nodiscard]] const MeshAsset* find_mesh(std::string_view name) const;
    [[nodiscard]] const MaterialAsset* find_material(std::string_view name) const;
    [[nodiscard]] const TextureAsset* find_texture(std::string_view name) const;
    [[nodiscard]] std::uint32_t texture_index(std::string_view name) const;
    [[nodiscard]] std::uint32_t material_index(std::string_view name) const;

    [[nodiscard]] std::string to_json() const;

    // Increments whenever imported content is appended. Render backends compare this against the
    // revision they last uploaded to decide when to rebuild device-local buffers.
    [[nodiscard]] std::uint64_t revision() const;

    bool register_imported(std::vector<MeshVertex> vertices, std::vector<std::uint32_t> indices,
                           std::vector<MeshAsset> meshes, std::vector<MaterialAsset> materials,
                           std::vector<TextureAsset> textures = {});

private:
    void recompute_bounds(std::size_t first_mesh);

    std::vector<MeshVertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<MeshAsset> meshes_;
    std::vector<MaterialAsset> materials_;
    std::vector<TextureAsset> textures_;
    std::uint64_t revision_{1U};
};

struct ModelImportResult {
    bool imported{};
    std::string content_id;
    std::string source_format;
    std::vector<std::string> meshes;
    std::vector<std::string> materials;
    std::vector<std::string> textures;
    std::vector<std::string> warnings;
    std::vector<Entity> roots;
    std::string source_adapter{"direct"};
    std::string preset{"scene"};
    bool conversion_cache_hit{};
    bool conversion_sandboxed{};
    std::string conversion_diagnostics;
    // Every file read during the import, relative to the assets root, in the order first opened.
    std::vector<std::string> dependencies;

    [[nodiscard]] std::string json() const;
};

struct ModelImportSettings {
    BlenderConversionSettings blender;
    // `scene` retains conversion-time animation/camera/light data for later import stages;
    // `static_mesh` strips those channels from Blender conversion output.
    std::string preset{"scene"};
};

[[nodiscard]] std::string model_import_capabilities_json();

// Imports `filename` from `assets_root`. Every dependency the model references is resolved inside
// that root; nothing outside it is readable. Pass a null scene to register assets without
// instantiating a node hierarchy.
[[nodiscard]] ModelImportResult import_model_asset(const std::filesystem::path& assets_root,
                                                   std::string_view filename,
                                                   AssetRegistry& registry, Scene* scene,
                                                   std::string& error,
                                                   const ModelImportSettings& settings = {});

} // namespace relay
