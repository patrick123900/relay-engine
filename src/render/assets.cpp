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

struct AssetStorage {
    std::vector<MeshVertex> vertices{initial_vertices.begin(), initial_vertices.end()};
    std::vector<std::uint32_t> indices{relay::indices.begin(), relay::indices.end()};
    std::vector<MeshAsset> meshes{initial_meshes.begin(), initial_meshes.end()};
    std::vector<MaterialAsset> materials{initial_materials.begin(), initial_materials.end()};
    std::vector<TextureAsset> textures{make_textures()};
    std::uint64_t revision{1U};
};

AssetStorage& storage() {
    static AssetStorage value;
    return value;
}

} // namespace

std::span<const MeshVertex> built_in_mesh_vertices() { return storage().vertices; }
std::span<const std::uint32_t> built_in_mesh_indices() { return storage().indices; }
std::span<const MeshAsset> built_in_meshes() { return storage().meshes; }
std::span<const MaterialAsset> built_in_materials() { return storage().materials; }
std::span<const TextureAsset> built_in_textures() { return storage().textures; }

const MeshAsset* find_mesh_asset(const std::string_view name) {
    const auto& meshes = storage().meshes;
    const auto found = std::find_if(meshes.begin(), meshes.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == meshes.end() ? nullptr : &*found;
}

const MaterialAsset* find_material_asset(const std::string_view name) {
    const auto& materials = storage().materials;
    const auto found = std::find_if(materials.begin(), materials.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == materials.end() ? nullptr : &*found;
}

const TextureAsset* find_texture_asset(const std::string_view name) {
    const auto& textures = storage().textures;
    const auto found = std::find_if(textures.begin(), textures.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == textures.end() ? nullptr : &*found;
}

std::uint32_t texture_asset_index(const std::string_view name) {
    const auto& textures = storage().textures;
    const auto found = std::find_if(textures.begin(), textures.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == textures.end() ? 0U : static_cast<std::uint32_t>(found - textures.begin());
}

std::string render_assets_json() {
    const auto& store = storage();
    const auto& meshes = store.meshes;
    const auto& materials = store.materials;
    const auto& textures = store.textures;
    std::ostringstream output;
    output << "{\"texture_table_capacity\":" << bindless_texture_capacity
           << ",\"meshes\":[";
    for (std::size_t index = 0; index < meshes.size(); ++index) {
        if (index != 0U) output << ',';
        const auto next_vertex = index + 1U < meshes.size()
                                     ? static_cast<std::uint32_t>(meshes[index + 1U].vertex_offset)
                                     : static_cast<std::uint32_t>(store.vertices.size());
        output << "{\"name\":\"" << meshes[index].name << "\",\"vertices\":"
               << next_vertex - static_cast<std::uint32_t>(meshes[index].vertex_offset)
               << ",\"indices\":" << meshes[index].index_count << '}';
    }
    output << "],\"materials\":[";
    for (std::size_t index = 0; index < materials.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& material = materials[index];
        output << "{\"name\":\"" << material.name << "\",\"color\":["
               << material.color[0] << ',' << material.color[1] << ',' << material.color[2]
               << ',' << material.color[3] << "],\"texture\":\"" << material.texture << "\"}";
    }
    output << "],\"textures\":[";
    for (std::size_t index = 0; index < textures.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"slot\":" << index << ",\"name\":\"" << textures[index].name
               << "\",\"width\":" << textures[index].width << ",\"height\":"
               << textures[index].height << ",\"mip_levels\":"
               << std::bit_width(std::max(textures[index].width, textures[index].height)) << "}";
    }
    output << "]}";
    return output.str();
}

std::uint64_t render_asset_revision() { return storage().revision; }

bool register_imported_render_assets(std::vector<MeshVertex> vertices,
                                     std::vector<std::uint32_t> indices_to_add,
                                     std::vector<MeshAsset> meshes,
                                     std::vector<MaterialAsset> materials) {
    auto& store = storage();
    if (vertices.empty() || indices_to_add.empty() || meshes.empty()) return false;
    if (std::all_of(meshes.begin(), meshes.end(), [](const MeshAsset& mesh) {
            return find_mesh_asset(mesh.name) != nullptr;
        })) return true;
    const auto vertex_base = static_cast<std::int32_t>(store.vertices.size());
    const auto index_base = static_cast<std::uint32_t>(store.indices.size());
    for (auto& mesh : meshes) {
        // Importer offsets are relative to the batch being registered.
        mesh.vertex_offset += vertex_base;
        mesh.first_index += index_base;
        store.meshes.push_back(std::move(mesh));
    }
    for (auto& material : materials) {
        if (find_material_asset(material.name) == nullptr) {
            store.materials.push_back(std::move(material));
        }
    }
    store.vertices.insert(store.vertices.end(), vertices.begin(), vertices.end());
    store.indices.insert(store.indices.end(), indices_to_add.begin(), indices_to_add.end());
    ++store.revision;
    return true;
}

} // namespace relay
