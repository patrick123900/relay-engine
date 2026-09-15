#include "relay/render/assets.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <sstream>

namespace relay {
namespace {

const std::array initial_vertices{
    MeshVertex{0.0F, -0.62F, 0.0F, 0.5F, 1.0F},
    MeshVertex{0.58F, 0.42F, 0.0F, 1.0F, 0.0F},
    MeshVertex{-0.58F, 0.42F, 0.0F, 0.0F, 0.0F},
    MeshVertex{-0.55F, -0.55F, 0.0F, 0.0F, 1.0F},
    MeshVertex{0.55F, -0.55F, 0.0F, 1.0F, 1.0F},
    MeshVertex{0.55F, 0.55F, 0.0F, 1.0F, 0.0F},
    MeshVertex{-0.55F, 0.55F, 0.0F, 0.0F, 0.0F},
};

constexpr std::array<std::uint32_t, 9> indices{0U, 1U, 2U, 0U, 1U, 2U, 2U, 3U, 0U};

const std::array initial_meshes{
    MeshAsset{"builtin.triangle", 0U, 3U, 0},
    MeshAsset{"builtin.quad", 3U, 6U, 3},
};

const std::array initial_materials{
    MaterialAsset{"builtin.orange", {0.98F, 0.45F, 0.16F, 1.0F}, "builtin.checker"},
    MaterialAsset{"builtin.azure", {0.18F, 0.76F, 0.96F, 1.0F}, "builtin.gradient"},
    MaterialAsset{"builtin.violet", {0.62F, 0.32F, 0.98F, 1.0F}, "builtin.checker"},
};

std::vector<TextureAsset> make_textures() {
    constexpr std::uint32_t size = 64U;
    std::vector<TextureAsset> output;
    TextureAsset checker{"builtin.checker", size, size,
                         std::vector<std::uint8_t>(size * size * 4U)};
    TextureAsset gradient{"builtin.gradient", size, size,
                          std::vector<std::uint8_t>(size * size * 4U)};
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * size + x) * 4U;
            const bool bright = ((x / 8U) + (y / 8U)) % 2U == 0U;
            const auto checker_value = static_cast<std::uint8_t>(bright ? 255U : 105U);
            checker.rgba[offset] = checker_value;
            checker.rgba[offset + 1U] = checker_value;
            checker.rgba[offset + 2U] = checker_value;
            checker.rgba[offset + 3U] = 255U;
            gradient.rgba[offset] = static_cast<std::uint8_t>(80U + x * 175U / (size - 1U));
            gradient.rgba[offset + 1U] = static_cast<std::uint8_t>(100U + y * 155U / (size - 1U));
            gradient.rgba[offset + 2U] = 255U;
            gradient.rgba[offset + 3U] = 255U;
        }
    }
    output.push_back(std::move(checker));
    output.push_back(std::move(gradient));
    return output;
}

template <typename Assets>
const typename Assets::value_type* find_named(const Assets& assets, const std::string_view name) {
    const auto found = std::find_if(assets.begin(), assets.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == assets.end() ? nullptr : &*found;
}

} // namespace

AssetRegistry::AssetRegistry()
    : vertices_{initial_vertices.begin(), initial_vertices.end()},
      indices_{relay::indices.begin(), relay::indices.end()},
      meshes_{initial_meshes.begin(), initial_meshes.end()},
      materials_{initial_materials.begin(), initial_materials.end()},
      textures_{make_textures()} {}

std::span<const MeshVertex> AssetRegistry::mesh_vertices() const { return vertices_; }
std::span<const std::uint32_t> AssetRegistry::mesh_indices() const { return indices_; }
std::span<const MeshAsset> AssetRegistry::meshes() const { return meshes_; }
std::span<const MaterialAsset> AssetRegistry::materials() const { return materials_; }
std::span<const TextureAsset> AssetRegistry::textures() const { return textures_; }

const MeshAsset* AssetRegistry::find_mesh(const std::string_view name) const {
    return find_named(meshes_, name);
}

const MaterialAsset* AssetRegistry::find_material(const std::string_view name) const {
    return find_named(materials_, name);
}

const TextureAsset* AssetRegistry::find_texture(const std::string_view name) const {
    return find_named(textures_, name);
}

std::uint32_t AssetRegistry::texture_index(const std::string_view name) const {
    const auto found = std::find_if(textures_.begin(), textures_.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == textures_.end() ? 0U : static_cast<std::uint32_t>(found - textures_.begin());
}

std::string AssetRegistry::to_json() const {
    std::ostringstream output;
    output << "{\"texture_table_capacity\":" << bindless_texture_capacity
           << ",\"meshes\":[";
    for (std::size_t index = 0; index < meshes_.size(); ++index) {
        if (index != 0U) output << ',';
        const auto next_vertex = index + 1U < meshes_.size()
                                     ? static_cast<std::uint32_t>(meshes_[index + 1U].vertex_offset)
                                     : static_cast<std::uint32_t>(vertices_.size());
        output << "{\"name\":\"" << meshes_[index].name << "\",\"vertices\":"
               << next_vertex - static_cast<std::uint32_t>(meshes_[index].vertex_offset)
               << ",\"indices\":" << meshes_[index].index_count << '}';
    }
    output << "],\"materials\":[";
    for (std::size_t index = 0; index < materials_.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& material = materials_[index];
        output << "{\"name\":\"" << material.name << "\",\"color\":["
               << material.color[0] << ',' << material.color[1] << ',' << material.color[2]
               << ',' << material.color[3] << "],\"texture\":\"" << material.texture << "\"}";
    }
    output << "],\"textures\":[";
    for (std::size_t index = 0; index < textures_.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"slot\":" << index << ",\"name\":\"" << textures_[index].name
               << "\",\"width\":" << textures_[index].width << ",\"height\":"
               << textures_[index].height << ",\"mip_levels\":"
               << std::bit_width(std::max(textures_[index].width, textures_[index].height)) << "}";
    }
    output << "]}";
    return output.str();
}

std::uint64_t AssetRegistry::revision() const { return revision_; }

bool AssetRegistry::register_imported(std::vector<MeshVertex> vertices,
                                      std::vector<std::uint32_t> indices_to_add,
                                      std::vector<MeshAsset> meshes,
                                      std::vector<MaterialAsset> materials) {
    if (vertices.empty() || indices_to_add.empty() || meshes.empty()) return false;
    if (std::all_of(meshes.begin(), meshes.end(), [this](const MeshAsset& mesh) {
            return find_mesh(mesh.name) != nullptr;
        })) return true;
    const auto vertex_base = static_cast<std::int32_t>(vertices_.size());
    const auto index_base = static_cast<std::uint32_t>(indices_.size());
    for (auto& mesh : meshes) {
        // Importer offsets are relative to the batch being registered.
        mesh.vertex_offset += vertex_base;
        mesh.first_index += index_base;
        meshes_.push_back(std::move(mesh));
    }
    for (auto& material : materials) {
        if (find_material(material.name) == nullptr) {
            materials_.push_back(std::move(material));
        }
    }
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
    indices_.insert(indices_.end(), indices_to_add.begin(), indices_to_add.end());
    ++revision_;
    return true;
}

} // namespace relay
