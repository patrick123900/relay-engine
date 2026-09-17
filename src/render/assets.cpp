#include "relay/render/assets.hpp"
#include "relay/core/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <iomanip>
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
    MaterialAsset{"builtin.orange", {0.98F, 0.45F, 0.16F, 1.0F}, "builtin.checker",
                  0.0F, 1.0F, {}, {}, 1.0F, {}, 1.0F, {}, {},
                  MaterialAsset::AlphaMode::opaque, 0.5F, false},
    MaterialAsset{"builtin.azure", {0.18F, 0.76F, 0.96F, 1.0F}, "builtin.gradient",
                  0.0F, 1.0F, {}, {}, 1.0F, {}, 1.0F, {}, {},
                  MaterialAsset::AlphaMode::opaque, 0.5F, false},
    MaterialAsset{"builtin.violet", {0.62F, 0.32F, 0.98F, 1.0F}, "builtin.checker",
                  0.0F, 1.0F, {}, {}, 1.0F, {}, 1.0F, {}, {},
                  MaterialAsset::AlphaMode::opaque, 0.5F, false},
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
      textures_{make_textures()} {
    recompute_bounds(0U);
}

void AssetRegistry::recompute_bounds(const std::size_t first_mesh) {
    for (std::size_t mesh_index = first_mesh; mesh_index < meshes_.size(); ++mesh_index) {
        auto& mesh = meshes_[mesh_index];
        std::array<float, 3> low{};
        std::array<float, 3> high{};
        bool seen = false;
        for (std::uint32_t offset = 0; offset < mesh.index_count; ++offset) {
            const std::size_t slot = mesh.first_index + offset;
            if (slot >= indices_.size()) break;
            const auto vertex_slot = static_cast<std::size_t>(mesh.vertex_offset) +
                                     static_cast<std::size_t>(indices_[slot]);
            if (vertex_slot >= vertices_.size()) continue;
            const auto& vertex = vertices_[vertex_slot];
            const std::array<float, 3> position{vertex.x, vertex.y, vertex.z};
            if (!seen) {
                low = position;
                high = position;
                seen = true;
                continue;
            }
            for (std::size_t axis = 0; axis < 3U; ++axis) {
                low[axis] = std::min(low[axis], position[axis]);
                high[axis] = std::max(high[axis], position[axis]);
            }
        }
        mesh.bounds_min = low;
        mesh.bounds_max = high;
    }
}

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

std::uint32_t AssetRegistry::material_index(const std::string_view name) const {
    const auto found = std::find_if(materials_.begin(), materials_.end(), [name](const auto& asset) {
        return asset.name == name;
    });
    return found == materials_.end() ? 0U : static_cast<std::uint32_t>(found - materials_.begin());
}

std::string AssetRegistry::to_json() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "{\"texture_table_capacity\":" << bindless_texture_capacity
           << ",\"meshes\":[";
    for (std::size_t index = 0; index < meshes_.size(); ++index) {
        if (index != 0U) output << ',';
        const auto next_vertex = index + 1U < meshes_.size()
                                     ? static_cast<std::uint32_t>(meshes_[index + 1U].vertex_offset)
                                     : static_cast<std::uint32_t>(vertices_.size());
        output << "{\"name\":\"" << meshes_[index].name << "\",\"vertices\":"
               << next_vertex - static_cast<std::uint32_t>(meshes_[index].vertex_offset)
               << ",\"indices\":" << meshes_[index].index_count
               << ",\"joints\":" << meshes_[index].joints.size()
               << ",\"morph_targets\":" << meshes_[index].morph_targets.size()
               << ",\"morph_default_weights\":[";
        for (std::size_t target = 0; target < meshes_[index].morph_targets.size(); ++target) {
            if (target) output << ',';
            output << meshes_[index].morph_targets[target].weight;
        }
        output << "]}";
    }
    output << "],\"materials\":[";
    for (std::size_t index = 0; index < materials_.size(); ++index) {
        if (index != 0U) output << ',';
        const auto& material = materials_[index];
        output << "{\"name\":\"" << material.name << "\",\"color\":["
               << material.color[0] << ',' << material.color[1] << ',' << material.color[2]
               << ',' << material.color[3] << "],\"texture\":\"" << material.texture
               << "\",\"metallic_factor\":" << material.metallic_factor
               << ",\"roughness_factor\":" << material.roughness_factor
               << ",\"normal_texture\":\"" << material.normal_texture
               << "\",\"occlusion_texture\":\"" << material.occlusion_texture
               << "\",\"emissive_texture\":\"" << material.emissive_texture << "\"}";
    }
    output << "],\"textures\":[";
    for (std::size_t index = 0; index < textures_.size(); ++index) {
        if (index != 0U) output << ',';
        output << "{\"slot\":" << index << ",\"name\":\"" << textures_[index].name
               << "\",\"width\":" << textures_[index].width << ",\"height\":"
               << textures_[index].height << ",\"mip_levels\":"
               << std::bit_width(std::max(textures_[index].width, textures_[index].height))
               << ",\"color_space\":\""
               << (textures_[index].color_space == TextureColorSpace::srgb ? "srgb" : "linear")
               << "\",\"mag_filter\":\""
               << (textures_[index].mag_filter == TextureFilter::linear ? "linear" : "nearest")
               << "\",\"min_filter\":\""
               << (textures_[index].min_filter == TextureFilter::linear ? "linear" : "nearest")
               << "\",\"mip_filter\":\""
               << (textures_[index].mip_filter == TextureFilter::linear ? "linear" : "nearest")
               << "\",\"wrap_u\":\""
               << (textures_[index].wrap_u == TextureWrap::repeat ? "repeat" :
                   textures_[index].wrap_u == TextureWrap::mirrored_repeat ? "mirrored_repeat" :
                                                                            "clamp_to_edge")
               << "\",\"wrap_v\":\""
               << (textures_[index].wrap_v == TextureWrap::repeat ? "repeat" :
                   textures_[index].wrap_v == TextureWrap::mirrored_repeat ? "mirrored_repeat" :
                                                                            "clamp_to_edge")
               << "\"}";
    }
    output << "],\"models\":[";
    for (std::size_t i = 0; i < models_.size(); ++i) {
        if (i)
            output << ',';
        output << "{\"name\":\"" << json_escape(models_[i].name)
               << "\",\"nodes\":" << models_[i].nodes.size() << ",\"clips\":[";
        for (std::size_t c = 0; c < models_[i].clips.size(); ++c) {
            if (c)
                output << ',';
            output << "{\"index\":" << c << ",\"name\":\"" << json_escape(models_[i].clips[c].name)
                   << "\",\"duration_seconds\":" << models_[i].clips[c].duration_seconds << '}';
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

std::uint64_t AssetRegistry::revision() const { return revision_; }

const ModelAsset *AssetRegistry::find_model(const std::string_view name) const {
    const auto found = std::find_if(models_.begin(), models_.end(),
                                    [&](const auto &model) { return model.name == name; });
    return found == models_.end() ? nullptr : &*found;
}
std::span<const ModelAsset> AssetRegistry::models() const { return models_; }
bool AssetRegistry::register_model(ModelAsset model) {
    if (model.name.empty() || model.nodes.empty())
        return false;
    if (!find_model(model.name))
        models_.push_back(std::move(model));
    return true;
}

bool AssetRegistry::register_imported(std::vector<MeshVertex> vertices,
                                      std::vector<std::uint32_t> indices_to_add,
                                      std::vector<MeshAsset> meshes,
                                      std::vector<MaterialAsset> materials,
                                      std::vector<TextureAsset> textures) {
    if (vertices.empty() || indices_to_add.empty() || meshes.empty()) return false;
    if (std::all_of(meshes.begin(), meshes.end(), [this](const MeshAsset& mesh) {
            return find_mesh(mesh.name) != nullptr;
        })) return true;
    if (vertices_.size() + vertices.size() > 16'000'000U ||
        indices_.size() + indices_to_add.size() > 32'000'000U)
        return false;
    for (auto &mesh : meshes) {
        if (mesh.vertex_offset < 0 || mesh.first_index > indices_to_add.size() ||
            mesh.index_count > indices_to_add.size() - mesh.first_index ||
            mesh.joints.size() > 256U || mesh.morph_targets.size() > 64U)
            return false;
        if (!mesh.vertex_count) {
            for (std::size_t i = mesh.first_index; i < mesh.first_index + mesh.index_count; ++i)
                mesh.vertex_count = std::max(mesh.vertex_count, indices_to_add[i] + 1U);
        }
        if (static_cast<std::size_t>(mesh.vertex_offset) + mesh.vertex_count > vertices.size())
            return false;
        for (std::size_t i = mesh.first_index; i < mesh.first_index + mesh.index_count; ++i)
            if (indices_to_add[i] >= mesh.vertex_count)
                return false;
        if (!mesh.skin.empty() && mesh.skin.size() != mesh.vertex_count)
            return false;
        for (const auto &influences : mesh.skin) {
            if (influences.size() > 8U)
                return false;
            for (const auto &w : influences)
                if (w.joint >= mesh.joints.size() || !std::isfinite(w.weight) || w.weight < 0.0F)
                    return false;
        }
        for (const auto &target : mesh.morph_targets)
            if (target.deltas.size() != mesh.vertex_count)
                return false;
    }
    const auto first_new_mesh = meshes_.size();
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
    for (auto& texture : textures) {
        if (find_texture(texture.name) == nullptr) textures_.push_back(std::move(texture));
    }
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
    indices_.insert(indices_.end(), indices_to_add.begin(), indices_to_add.end());
    recompute_bounds(first_new_mesh);
    ++revision_;
    return true;
}

} // namespace relay
