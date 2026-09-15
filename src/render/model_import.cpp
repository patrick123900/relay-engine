#include "relay/render/assets.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#ifdef RELAY_HAS_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace relay {
namespace {

std::string escape_json(const std::string_view value) {
    std::string output;
    for (const char c : value) {
        if (c == '"' || c == '\\') output += '\\';
        if (c == '\n') output += "\\n";
        else output += c;
    }
    return output;
}

std::string content_hash(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open model asset: " + path.string();
        return {};
    }
    // FNV-1a is deliberately named in the public identity. The scheme can be upgraded without
    // invalidating old scenes because the algorithm is part of every generated asset name.
    std::uint64_t hash = 14695981039346656037ULL;
    std::array<char, 64U * 1024U> block{};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        for (std::streamsize i = 0; i < input.gcount(); ++i) {
            hash ^= static_cast<unsigned char>(block[static_cast<std::size_t>(i)]);
            hash *= 1099511628211ULL;
        }
    }
    std::ostringstream output;
    output << "fnv1a64-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

#ifdef RELAY_HAS_ASSIMP
void hash_bytes(std::uint64_t& hash, const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
}

void hash_node(std::uint64_t& hash, const aiNode& node) {
    hash_bytes(hash, node.mName.C_Str(), node.mName.length);
    hash_bytes(hash, &node.mTransformation, sizeof(node.mTransformation));
    hash_bytes(hash, node.mMeshes, sizeof(unsigned) * node.mNumMeshes);
    for (unsigned index = 0; index < node.mNumChildren; ++index) {
        hash_node(hash, *node.mChildren[index]);
    }
}

std::string imported_content_hash(const aiScene& scene) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned mesh_index = 0; mesh_index < scene.mNumMeshes; ++mesh_index) {
        const auto& mesh = *scene.mMeshes[mesh_index];
        hash_bytes(hash, mesh.mVertices, sizeof(aiVector3D) * mesh.mNumVertices);
        if (mesh.HasTextureCoords(0)) {
            hash_bytes(hash, mesh.mTextureCoords[0], sizeof(aiVector3D) * mesh.mNumVertices);
        }
        for (unsigned face = 0; face < mesh.mNumFaces; ++face) {
            hash_bytes(hash, mesh.mFaces[face].mIndices,
                       sizeof(unsigned) * mesh.mFaces[face].mNumIndices);
        }
        hash_bytes(hash, &mesh.mMaterialIndex, sizeof(mesh.mMaterialIndex));
    }
    for (unsigned material_index = 0; material_index < scene.mNumMaterials; ++material_index) {
        aiColor4D color{1.0F, 1.0F, 1.0F, 1.0F};
        const auto* material = scene.mMaterials[material_index];
        if (material->Get(AI_MATKEY_BASE_COLOR, color) != aiReturn_SUCCESS) {
            (void)material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        hash_bytes(hash, &color, sizeof(color));
    }
    hash_node(hash, *scene.mRootNode);
    std::ostringstream output;
    output << "fnv1a64-" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return output.str();
}

Transform imported_transform(const aiMatrix4x4& matrix) {
    aiVector3D scale;
    aiVector3D position;
    aiQuaternion rotation;
    matrix.Decompose(scale, rotation, position);
    const double sinr = 2.0 * (rotation.w * rotation.x + rotation.y * rotation.z);
    const double cosr = 1.0 - 2.0 * (rotation.x * rotation.x + rotation.y * rotation.y);
    const double sinp = 2.0 * (rotation.w * rotation.y - rotation.z * rotation.x);
    const double siny = 2.0 * (rotation.w * rotation.z + rotation.x * rotation.y);
    const double cosy = 1.0 - 2.0 * (rotation.y * rotation.y + rotation.z * rotation.z);
    constexpr double radians_to_degrees = 57.295779513082320876;
    const double pitch = std::abs(sinp) >= 1.0 ? std::copysign(1.5707963267948966, sinp)
                                                : std::asin(sinp);
    return {{position.x, position.y, position.z},
            {std::atan2(sinr, cosr) * radians_to_degrees, pitch * radians_to_degrees,
             std::atan2(siny, cosy) * radians_to_degrees},
            {scale.x, scale.y, scale.z}};
}

Entity instantiate_node(const aiNode* node, const Entity parent, Scene& scene,
                        const std::vector<std::string>& mesh_names,
                        const std::vector<std::string>& material_names,
                        const aiScene& imported, std::vector<Entity>& roots) {
    auto name = std::string{node->mName.C_Str()};
    if (name.empty()) name = "Imported Node";
    const auto entity = scene.create(name, parent);
    (void)scene.set_transform(entity, imported_transform(node->mTransformation));
    if (!parent.valid()) roots.push_back(entity);
    for (unsigned index = 0; index < node->mNumMeshes; ++index) {
        const auto mesh_index = node->mMeshes[index];
        Entity render_entity = entity;
        if (node->mNumMeshes > 1U || index > 0U) {
            render_entity = scene.create(name + " Mesh " + std::to_string(index + 1U), entity);
        }
        const auto material_index = imported.mMeshes[mesh_index]->mMaterialIndex;
        const auto material = material_index < material_names.size()
                                  ? material_names[material_index] : "builtin.orange";
        (void)scene.set_mesh_renderer(render_entity,
                                      MeshRenderer{mesh_names[mesh_index], material});
    }
    for (unsigned index = 0; index < node->mNumChildren; ++index) {
        (void)instantiate_node(node->mChildren[index], entity, scene, mesh_names, material_names,
                               imported, roots);
    }
    return entity;
}
#endif

} // namespace

std::string ModelImportResult::json() const {
    std::ostringstream output;
    output << "{\"imported\":" << (imported ? "true" : "false")
           << ",\"content_id\":\"" << escape_json(content_id) << "\",\"format\":\""
           << escape_json(source_format) << "\",\"meshes\":[";
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(meshes[i]) << '"';
    }
    output << "],\"materials\":[";
    for (std::size_t i = 0; i < materials.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(materials[i]) << '"';
    }
    output << "],\"roots\":[";
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (i) output << ',';
        output << '"' << roots[i].to_string() << '"';
    }
    output << "],\"warnings\":[";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(warnings[i]) << '"';
    }
    output << "]}";
    return output.str();
}

std::string model_import_capabilities_json() {
#ifdef RELAY_HAS_ASSIMP
    return R"({"available":true,"native_recommended":[".gltf",".glb"],"formats":[".gltf",".glb",".fbx",".obj",".dae",".blend"],"backend":"Assimp","notes":{".blend":"Godot-style Blender-to-glTF conversion is planned; direct Assimp loading is currently used","materials":"base color is imported; image textures and PBR channels are deferred","animation":"detected and reported but not yet instantiated"}})";
#else
    return R"({"available":false,"native_recommended":[".gltf",".glb"],"formats":[],"backend":"none","reason":"Relay was built without Assimp"})";
#endif
}

ModelImportResult import_model_asset(const std::filesystem::path& path, Scene* scene,
                                     std::string& error) {
    ModelImportResult result;
    result.source_format = path.extension().string();
    result.content_id = content_hash(path, error);
    if (result.content_id.empty()) return result;
#ifndef RELAY_HAS_ASSIMP
    (void)scene;
    error = "this Relay build does not include the Assimp model importer";
    return result;
#else
    Assimp::Importer importer;
    const auto* imported = importer.ReadFile(path.string(), aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType | aiProcess_ValidateDataStructure);
    if (imported == nullptr || imported->mRootNode == nullptr) {
        error = "model import failed: " + std::string{importer.GetErrorString()};
        return result;
    }
    // Hash normalized imported content, not just the container file. This catches changes in the
    // external .bin payload referenced by a .gltf descriptor.
    result.content_id = imported_content_hash(*imported);
    std::vector<MaterialAsset> new_materials;
    new_materials.reserve(imported->mNumMaterials);
    bool has_image_textures = imported->HasTextures();
    for (unsigned index = 0; index < imported->mNumMaterials; ++index) {
        aiColor4D color{1.0F, 1.0F, 1.0F, 1.0F};
        const auto* source = imported->mMaterials[index];
        has_image_textures = has_image_textures || source->GetTextureCount(aiTextureType_BASE_COLOR) > 0U ||
                             source->GetTextureCount(aiTextureType_DIFFUSE) > 0U ||
                             source->GetTextureCount(aiTextureType_NORMALS) > 0U ||
                             source->GetTextureCount(aiTextureType_METALNESS) > 0U ||
                             source->GetTextureCount(aiTextureType_DIFFUSE_ROUGHNESS) > 0U;
        if (source->Get(AI_MATKEY_BASE_COLOR, color) != aiReturn_SUCCESS) {
            (void)source->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        const auto name = "asset." + result.content_id + ".material." + std::to_string(index);
        new_materials.push_back({name, {color.r, color.g, color.b, color.a}, "builtin.checker"});
        result.materials.push_back(name);
    }
    if (new_materials.empty()) {
        const auto name = "asset." + result.content_id + ".material.0";
        new_materials.push_back({name, {1.0F, 1.0F, 1.0F, 1.0F}, "builtin.checker"});
        result.materials.push_back(name);
    }
    std::vector<MeshVertex> new_vertices;
    std::vector<std::uint32_t> new_indices;
    std::vector<MeshAsset> new_meshes;
    new_meshes.reserve(imported->mNumMeshes);
    bool has_skeleton = false;
    for (unsigned mesh_index = 0; mesh_index < imported->mNumMeshes; ++mesh_index) {
        const auto* source = imported->mMeshes[mesh_index];
        has_skeleton = has_skeleton || source->HasBones();
        const auto vertex_offset = static_cast<std::int32_t>(new_vertices.size());
        const auto first_index = static_cast<std::uint32_t>(new_indices.size());
        for (unsigned vertex = 0; vertex < source->mNumVertices; ++vertex) {
            const auto& p = source->mVertices[vertex];
            const auto uv = source->HasTextureCoords(0) ? source->mTextureCoords[0][vertex]
                                                        : aiVector3D{};
            new_vertices.push_back({p.x, p.y, p.z, uv.x, uv.y});
        }
        for (unsigned face = 0; face < source->mNumFaces; ++face) {
            for (unsigned index = 0; index < source->mFaces[face].mNumIndices; ++index) {
                new_indices.push_back(source->mFaces[face].mIndices[index]);
            }
        }
        const auto name = "asset." + result.content_id + ".mesh." + std::to_string(mesh_index);
        new_meshes.push_back({name, first_index,
                              static_cast<std::uint32_t>(new_indices.size()) - first_index,
                              vertex_offset});
        result.meshes.push_back(name);
    }
    if (new_meshes.empty()) {
        error = "model contains no triangle meshes";
        return result;
    }
    extern bool register_imported_render_assets(std::vector<MeshVertex>, std::vector<std::uint32_t>,
                                                std::vector<MeshAsset>, std::vector<MaterialAsset>);
    if (!register_imported_render_assets(std::move(new_vertices), std::move(new_indices),
                                         std::move(new_meshes), std::move(new_materials))) {
        error = "model assets could not be registered";
        return result;
    }
    if (imported->HasAnimations()) result.warnings.push_back("animations are not imported yet");
    if (has_skeleton) result.warnings.push_back("skeletons and skin weights are not imported yet");
    if (has_image_textures) result.warnings.push_back("image textures and PBR channels are not imported yet");
    if (scene != nullptr) {
        instantiate_node(imported->mRootNode, {}, *scene, result.meshes, result.materials,
                         *imported, result.roots);
    }
    result.imported = true;
    return result;
#endif
}

} // namespace relay
