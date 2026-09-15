#include "relay/core/hash.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_map>

#ifdef RELAY_HAS_ASSIMP
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/GltfMaterial.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cstring>
#endif

namespace relay {
namespace {

// Identity scheme version. It is part of every generated asset name, so the digest can be upgraded
// later without silently changing the meaning of names already written into saved scenes.
constexpr std::string_view asset_identity_prefix = "sha256-v1-";

// A model may pull in external buffers and images. These bounds keep a malicious or malformed file
// from exhausting memory or file handles through its dependency graph.
constexpr std::size_t max_dependency_files = 64U;
constexpr std::uintmax_t max_dependency_bytes = 64U * 1024U * 1024U;

std::string escape_json(const std::string_view value) {
    std::string output;
    for (const char c : value) {
        if (c == '"' || c == '\\') output += '\\';
        if (c == '\n') output += "\\n";
        else output += c;
    }
    return output;
}

// Canonicalizes `requested` and confirms it stays inside `root`. Returns nullopt for anything that
// escapes, including via symlink, `..` segments or an absolute path.
std::optional<std::filesystem::path> resolve_inside(const std::filesystem::path& root,
                                                    const std::filesystem::path& requested) {
    std::error_code code;
    auto candidate = requested.is_absolute() ? requested : root / requested;
    const auto canonical = std::filesystem::weakly_canonical(candidate, code);
    if (code) return std::nullopt;
    const auto relative = canonical.lexically_relative(root);
    if (relative.empty() || relative.native().starts_with("..")) return std::nullopt;
    return canonical;
}

std::string hash_file_identity(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "could not open model asset: " + path.filename().string();
        return {};
    }
    Sha256 digest;
    std::array<char, 64U * 1024U> block{};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        if (input.gcount() > 0) {
            digest.update(block.data(), static_cast<std::size_t>(input.gcount()));
        }
    }
    return std::string{asset_identity_prefix} + digest.hex();
}

#ifdef RELAY_HAS_ASSIMP

// Backs Assimp with an in-memory copy of an already size-checked file, so the importer never holds
// a live handle to anything on disk.
class SandboxedStream final : public Assimp::IOStream {
public:
    explicit SandboxedStream(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::size_t Read(void* buffer, const std::size_t size, const std::size_t count) override {
        if (size == 0U) return 0U;
        const auto available = (bytes_.size() - cursor_) / size;
        const auto taken = std::min(count, available);
        std::memcpy(buffer, bytes_.data() + cursor_, taken * size);
        cursor_ += taken * size;
        return taken;
    }

    std::size_t Write(const void*, std::size_t, std::size_t) override { return 0U; }

    aiReturn Seek(const std::size_t offset, const aiOrigin origin) override {
        std::size_t target = offset;
        if (origin == aiOrigin_CUR) target = cursor_ + offset;
        if (origin == aiOrigin_END) target = bytes_.size() - offset;
        if (target > bytes_.size()) return aiReturn_FAILURE;
        cursor_ = target;
        return aiReturn_SUCCESS;
    }

    std::size_t Tell() const override { return cursor_; }
    std::size_t FileSize() const override { return bytes_.size(); }
    void Flush() override {}

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t cursor_{};
};

// Restricts every read Assimp performs to the project asset root and records what was touched, so
// the import manifest can later detect a changed or missing dependency.
class SandboxedIOSystem final : public Assimp::IOSystem {
public:
    explicit SandboxedIOSystem(std::filesystem::path root) : root_(std::move(root)) {}

    bool Exists(const char* file) const override {
        const auto resolved = resolve_inside(root_, std::filesystem::path{file});
        return resolved && std::filesystem::exists(*resolved);
    }

    char getOsSeparator() const override {
        return static_cast<char>(std::filesystem::path::preferred_separator);
    }

    Assimp::IOStream* Open(const char* file, const char* mode) override {
        if (mode != nullptr && std::string_view{mode}.find_first_of("wa+") != std::string_view::npos) {
            reject(std::string{"the importer tried to write through '"} + file + "'");
            return nullptr;
        }
        const auto resolved = resolve_inside(root_, std::filesystem::path{file});
        if (!resolved) {
            reject(std::string{"model dependency '"} + file +
                   "' resolves outside the project assets directory");
            return nullptr;
        }
        std::error_code code;
        const auto size = std::filesystem::file_size(*resolved, code);
        if (code) {
            reject("model dependency '" + resolved->filename().string() + "' could not be read");
            return nullptr;
        }
        if (size > max_dependency_bytes) {
            reject("model dependency '" + resolved->filename().string() + "' exceeds the " +
                   std::to_string(max_dependency_bytes / (1024U * 1024U)) + " MiB import limit");
            return nullptr;
        }
        if (dependencies_.size() >= max_dependency_files &&
            std::find(dependencies_.begin(), dependencies_.end(), *resolved) == dependencies_.end()) {
            reject("model references more than " + std::to_string(max_dependency_files) +
                   " dependency files");
            return nullptr;
        }
        std::ifstream input(*resolved, std::ios::binary);
        if (!input) {
            reject("model dependency '" + resolved->filename().string() + "' could not be opened");
            return nullptr;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size > 0U) {
            input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
            if (static_cast<std::uintmax_t>(input.gcount()) != size) {
                reject("model dependency '" + resolved->filename().string() + "' ended early");
                return nullptr;
            }
        }
        if (std::find(dependencies_.begin(), dependencies_.end(), *resolved) == dependencies_.end()) {
            dependencies_.push_back(*resolved);
        }
        return new SandboxedStream(std::move(bytes));
    }

    void Close(Assimp::IOStream* stream) override { delete stream; }

    bool read_bytes(const char* file, std::vector<std::uint8_t>& bytes) {
        auto* stream = Open(file, "rb");
        if (stream == nullptr) return false;
        bytes.resize(stream->FileSize());
        const auto read = bytes.empty() ? 0U : stream->Read(bytes.data(), 1U, bytes.size());
        Close(stream);
        if (read != bytes.size()) {
            reject(std::string{"model dependency '"} + file + "' ended early");
            bytes.clear();
            return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& rejection() const { return rejection_; }
    [[nodiscard]] const std::vector<std::filesystem::path>& dependencies() const {
        return dependencies_;
    }

private:
    void reject(std::string message) {
        if (rejection_.empty()) rejection_ = std::move(message);
    }

    std::filesystem::path root_;
    std::vector<std::filesystem::path> dependencies_;
    std::string rejection_;
};

void hash_bytes(Sha256& digest, const void* data, const std::size_t size) {
    digest.update(data, size);
}

void hash_node(Sha256& digest, const aiNode& node) {
    hash_bytes(digest, node.mName.C_Str(), node.mName.length);
    hash_bytes(digest, &node.mTransformation, sizeof(node.mTransformation));
    hash_bytes(digest, node.mMeshes, sizeof(unsigned) * node.mNumMeshes);
    for (unsigned index = 0; index < node.mNumChildren; ++index) {
        hash_node(digest, *node.mChildren[index]);
    }
}

std::string imported_content_hash(const aiScene& scene,
                                  const std::vector<MaterialAsset>& materials,
                                  const std::vector<TextureAsset>& textures) {
    Sha256 digest;
    for (unsigned mesh_index = 0; mesh_index < scene.mNumMeshes; ++mesh_index) {
        const auto& mesh = *scene.mMeshes[mesh_index];
        hash_bytes(digest, mesh.mVertices, sizeof(aiVector3D) * mesh.mNumVertices);
        if (mesh.HasTextureCoords(0)) {
            hash_bytes(digest, mesh.mTextureCoords[0], sizeof(aiVector3D) * mesh.mNumVertices);
        }
        if (mesh.HasNormals()) {
            hash_bytes(digest, mesh.mNormals, sizeof(aiVector3D) * mesh.mNumVertices);
        }
        if (mesh.HasTangentsAndBitangents()) {
            hash_bytes(digest, mesh.mTangents, sizeof(aiVector3D) * mesh.mNumVertices);
            hash_bytes(digest, mesh.mBitangents, sizeof(aiVector3D) * mesh.mNumVertices);
        }
        for (unsigned face = 0; face < mesh.mNumFaces; ++face) {
            hash_bytes(digest, mesh.mFaces[face].mIndices,
                       sizeof(unsigned) * mesh.mFaces[face].mNumIndices);
        }
        hash_bytes(digest, &mesh.mMaterialIndex, sizeof(mesh.mMaterialIndex));
    }
    for (unsigned material_index = 0; material_index < scene.mNumMaterials; ++material_index) {
        aiColor4D color{1.0F, 1.0F, 1.0F, 1.0F};
        const auto* material = scene.mMaterials[material_index];
        if (material->Get(AI_MATKEY_BASE_COLOR, color) != aiReturn_SUCCESS) {
            (void)material->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        hash_bytes(digest, &color, sizeof(color));
    }
    for (const auto& material : materials) {
        hash_bytes(digest, material.color.data(), sizeof(float) * material.color.size());
        hash_bytes(digest, &material.metallic_factor, sizeof(material.metallic_factor));
        hash_bytes(digest, &material.roughness_factor, sizeof(material.roughness_factor));
        hash_bytes(digest, material.texture.data(), material.texture.size());
        hash_bytes(digest, material.metallic_roughness_texture.data(),
                   material.metallic_roughness_texture.size());
        hash_bytes(digest, material.normal_texture.data(), material.normal_texture.size());
        hash_bytes(digest, &material.normal_scale, sizeof(material.normal_scale));
        hash_bytes(digest, material.occlusion_texture.data(), material.occlusion_texture.size());
        hash_bytes(digest, &material.occlusion_strength, sizeof(material.occlusion_strength));
        hash_bytes(digest, material.emissive_factor.data(),
                   sizeof(float) * material.emissive_factor.size());
        hash_bytes(digest, material.emissive_texture.data(), material.emissive_texture.size());
        hash_bytes(digest, &material.alpha_mode, sizeof(material.alpha_mode));
        hash_bytes(digest, &material.alpha_cutoff, sizeof(material.alpha_cutoff));
        hash_bytes(digest, &material.double_sided, sizeof(material.double_sided));
    }
    for (const auto& texture : textures) {
        hash_bytes(digest, &texture.width, sizeof(texture.width));
        hash_bytes(digest, &texture.height, sizeof(texture.height));
        hash_bytes(digest, &texture.color_space, sizeof(texture.color_space));
        hash_bytes(digest, &texture.mag_filter, sizeof(texture.mag_filter));
        hash_bytes(digest, &texture.min_filter, sizeof(texture.min_filter));
        hash_bytes(digest, &texture.mip_filter, sizeof(texture.mip_filter));
        hash_bytes(digest, &texture.wrap_u, sizeof(texture.wrap_u));
        hash_bytes(digest, &texture.wrap_v, sizeof(texture.wrap_v));
        hash_bytes(digest, texture.rgba.data(), texture.rgba.size());
    }
    hash_node(digest, *scene.mRootNode);
    return std::string{asset_identity_prefix} + digest.hex();
}

bool decode_texture_reference(const aiScene& scene, SandboxedIOSystem& sandbox,
                              const aiString& reference, const TextureColorSpace color_space,
                              TextureAsset& output, std::string& error) {
    const auto* embedded = scene.GetEmbeddedTexture(reference.C_Str());
    if (embedded != nullptr) {
        if (embedded->mHeight != 0U) {
            if (embedded->mWidth == 0U || embedded->mWidth > 8192U || embedded->mHeight > 8192U ||
                static_cast<std::uint64_t>(embedded->mWidth) * embedded->mHeight * 4U >
                    256U * 1024U * 1024U) {
                error = "embedded texture dimensions exceed the image limit";
                return false;
            }
            output.width = embedded->mWidth;
            output.height = embedded->mHeight;
            output.rgba.resize(static_cast<std::size_t>(output.width) * output.height * 4U);
            for (std::size_t index = 0; index <
                 static_cast<std::size_t>(output.width) * output.height; ++index) {
                output.rgba[index * 4U] = embedded->pcData[index].r;
                output.rgba[index * 4U + 1U] = embedded->pcData[index].g;
                output.rgba[index * 4U + 2U] = embedded->pcData[index].b;
                output.rgba[index * 4U + 3U] = embedded->pcData[index].a;
            }
        } else {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(embedded->pcData);
            const std::span<const std::uint8_t> bytes{begin, embedded->mWidth};
            if (!decode_image_rgba(bytes, embedded->achFormatHint, output, error)) return false;
        }
    } else {
        std::vector<std::uint8_t> bytes;
        if (!sandbox.read_bytes(reference.C_Str(), bytes)) {
            error = sandbox.rejection();
            return false;
        }
        if (!decode_image_rgba(bytes, std::filesystem::path{reference.C_Str()}.extension().string(),
                               output, error)) {
            return false;
        }
    }
    output.color_space = color_space;
    return true;
}

std::string texture_token(const std::size_t index) {
    return "@relay-texture-" + std::to_string(index);
}

void resolve_texture_token(std::string& value, const std::vector<TextureAsset>& textures) {
    constexpr std::string_view prefix = "@relay-texture-";
    if (!value.starts_with(prefix)) return;
    const auto index = static_cast<std::size_t>(std::stoull(value.substr(prefix.size())));
    value = index < textures.size() ? textures[index].name : std::string{};
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
    output << "],\"textures\":[";
    for (std::size_t i = 0; i < textures.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(textures[i]) << '"';
    }
    output << "],\"roots\":[";
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (i) output << ',';
        output << '"' << roots[i].to_string() << '"';
    }
    output << "],\"dependencies\":[";
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(dependencies[i]) << '"';
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
    return R"({"available":true,"native_recommended":[".gltf",".glb"],"formats":[".gltf",".glb",".fbx",".obj",".dae",".blend"],"backend":"Assimp","identity":"sha256-v1","image_decoders":["png","jpeg"],"sandbox":{"root":"assets","max_dependency_files":64,"max_dependency_bytes":67108864},"notes":{".blend":"Godot-style Blender-to-glTF conversion is planned; direct Assimp loading is currently used","materials":"glTF metallic-roughness factors and base-color, metallic-roughness, normal, occlusion and emissive textures are imported","animation":"detected and reported but not yet instantiated"}})";
#else
    return R"({"available":false,"native_recommended":[".gltf",".glb"],"formats":[],"backend":"none","identity":"sha256-v1","reason":"Relay was built without Assimp"})";
#endif
}

ModelImportResult import_model_asset(const std::filesystem::path& assets_root,
                                     const std::string_view filename, AssetRegistry& registry,
                                     Scene* scene, std::string& error) {
    ModelImportResult result;
    result.source_format = std::filesystem::path{filename}.extension().string();

    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(assets_root, code);
    if (code) {
        error = "project assets directory is not accessible";
        return result;
    }
    const auto model_path = resolve_inside(root, std::filesystem::path{filename});
    if (!model_path || !std::filesystem::is_regular_file(*model_path)) {
        error = "model file is not a readable file inside the project assets directory";
        return result;
    }
    result.content_id = hash_file_identity(*model_path, error);
    if (result.content_id.empty()) return result;
#ifndef RELAY_HAS_ASSIMP
    (void)scene;
    (void)registry;
    error = "this Relay build does not include the Assimp model importer";
    return result;
#else
    Assimp::Importer importer;
    // The importer takes ownership of the handler; the observer stays valid while it is alive.
    auto* sandbox = new SandboxedIOSystem(root);
    importer.SetIOHandler(sandbox);
    const auto* imported = importer.ReadFile(std::string{filename}, aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
        aiProcess_SortByPType | aiProcess_ValidateDataStructure | aiProcess_GenSmoothNormals |
        aiProcess_CalcTangentSpace);
    if (imported == nullptr || imported->mRootNode == nullptr) {
        error = sandbox->rejection().empty()
                    ? "model import failed: " + std::string{importer.GetErrorString()}
                    : "model import blocked: " + sandbox->rejection();
        return result;
    }
    std::vector<MaterialAsset> new_materials;
    new_materials.reserve(imported->mNumMaterials);
    std::vector<TextureAsset> new_textures;
    std::unordered_map<std::string, std::size_t> texture_lookup;
    const auto assign_texture = [&](const aiMaterial& material,
                                    const std::initializer_list<aiTextureType> types,
                                    const std::string_view semantic,
                                    const TextureColorSpace color_space,
                                    std::string& destination) {
        aiString reference;
        bool found = false;
        aiTextureType selected_type = aiTextureType_NONE;
        for (const auto type : types) {
            if (material.GetTexture(type, 0U, &reference) == aiReturn_SUCCESS) {
                found = true;
                selected_type = type;
                break;
            }
        }
        if (!found) return;
        const auto key = std::string{reference.C_Str()} +
                         (color_space == TextureColorSpace::srgb ? "#srgb" : "#linear");
        if (const auto existing = texture_lookup.find(key); existing != texture_lookup.end()) {
            destination = texture_token(existing->second);
            return;
        }
        TextureAsset texture;
        aiTextureMapMode mapping[3]{aiTextureMapMode_Wrap, aiTextureMapMode_Wrap,
                                    aiTextureMapMode_Wrap};
        (void)material.GetTexture(selected_type, 0U, &reference, nullptr, nullptr, nullptr, nullptr,
                                  mapping);
        const auto convert_wrap = [](const aiTextureMapMode mode) {
            if (mode == aiTextureMapMode_Clamp || mode == aiTextureMapMode_Decal) {
                return TextureWrap::clamp_to_edge;
            }
            if (mode == aiTextureMapMode_Mirror) return TextureWrap::mirrored_repeat;
            return TextureWrap::repeat;
        };
        texture.wrap_u = convert_wrap(mapping[0]);
        texture.wrap_v = convert_wrap(mapping[1]);
        int mag_filter = 9729;
        int min_filter = 9987;
        (void)material.Get(AI_MATKEY_GLTF_MAPPINGFILTER_MAG(selected_type, 0), mag_filter);
        (void)material.Get(AI_MATKEY_GLTF_MAPPINGFILTER_MIN(selected_type, 0), min_filter);
        texture.mag_filter = mag_filter == 9728 ? TextureFilter::nearest : TextureFilter::linear;
        texture.min_filter = (min_filter == 9728 || min_filter == 9984 || min_filter == 9986)
                                 ? TextureFilter::nearest : TextureFilter::linear;
        texture.mip_filter = (min_filter == 9984 || min_filter == 9985)
                                 ? TextureFilter::nearest : TextureFilter::linear;
        std::string decode_error;
        if (!decode_texture_reference(*imported, *sandbox, reference, color_space, texture,
                                      decode_error)) {
            result.warnings.push_back(std::string{semantic} + " texture '" + reference.C_Str() +
                                      "' was not decoded: " + decode_error);
            return;
        }
        const auto texture_index = new_textures.size();
        texture_lookup.emplace(key, texture_index);
        new_textures.push_back(std::move(texture));
        destination = texture_token(texture_index);
    };
    for (unsigned index = 0; index < imported->mNumMaterials; ++index) {
        aiColor4D color{1.0F, 1.0F, 1.0F, 1.0F};
        const auto* source = imported->mMaterials[index];
        if (source->Get(AI_MATKEY_BASE_COLOR, color) != aiReturn_SUCCESS) {
            (void)source->Get(AI_MATKEY_COLOR_DIFFUSE, color);
        }
        MaterialAsset material;
        material.color = {color.r, color.g, color.b, color.a};
        (void)source->Get(AI_MATKEY_METALLIC_FACTOR, material.metallic_factor);
        (void)source->Get(AI_MATKEY_ROUGHNESS_FACTOR, material.roughness_factor);
        aiColor3D emissive{};
        if (source->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == aiReturn_SUCCESS) {
            material.emissive_factor = {emissive.r, emissive.g, emissive.b};
        }
        int double_sided = 0;
        if (source->Get(AI_MATKEY_TWOSIDED, double_sided) == aiReturn_SUCCESS) {
            material.double_sided = double_sided != 0;
        }
        aiString alpha_mode;
        if (source->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode) == aiReturn_SUCCESS) {
            const std::string_view mode{alpha_mode.C_Str()};
            if (mode == "MASK") material.alpha_mode = MaterialAsset::AlphaMode::mask;
            else if (mode == "BLEND") material.alpha_mode = MaterialAsset::AlphaMode::blend;
        }
        (void)source->Get(AI_MATKEY_GLTF_ALPHACUTOFF, material.alpha_cutoff);
        (void)source->Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0),
                          material.normal_scale);
        (void)source->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_AMBIENT_OCCLUSION, 0),
                          material.occlusion_strength);
        assign_texture(*source, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, "base color",
                       TextureColorSpace::srgb, material.texture);
        assign_texture(*source, {aiTextureType_GLTF_METALLIC_ROUGHNESS, aiTextureType_METALNESS,
                                 aiTextureType_DIFFUSE_ROUGHNESS},
                       "metallic-roughness", TextureColorSpace::linear,
                       material.metallic_roughness_texture);
        assign_texture(*source, {aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA}, "normal",
                       TextureColorSpace::linear, material.normal_texture);
        assign_texture(*source, {aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP},
                       "occlusion", TextureColorSpace::linear, material.occlusion_texture);
        assign_texture(*source, {aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE}, "emissive",
                       TextureColorSpace::srgb, material.emissive_texture);
        new_materials.push_back(std::move(material));
    }
    if (new_materials.empty()) {
        MaterialAsset material;
        material.color = {1.0F, 1.0F, 1.0F, 1.0F};
        new_materials.push_back(std::move(material));
    }
    if (!sandbox->rejection().empty()) {
        result.warnings.push_back("blocked dependency: " + sandbox->rejection());
    }
    for (const auto& dependency : sandbox->dependencies()) {
        result.dependencies.push_back(dependency.lexically_relative(root).generic_string());
    }
    // Identity covers normalized geometry, generated normals/tangents, material factors and decoded
    // image pixels, so changes in external image dependencies produce new durable asset IDs.
    result.content_id = imported_content_hash(*imported, new_materials, new_textures);
    for (std::size_t index = 0; index < new_textures.size(); ++index) {
        new_textures[index].name = "asset." + result.content_id + ".texture." +
                                   std::to_string(index);
        result.textures.push_back(new_textures[index].name);
    }
    for (std::size_t index = 0; index < new_materials.size(); ++index) {
        auto& material = new_materials[index];
        material.name = "asset." + result.content_id + ".material." + std::to_string(index);
        resolve_texture_token(material.texture, new_textures);
        resolve_texture_token(material.metallic_roughness_texture, new_textures);
        resolve_texture_token(material.normal_texture, new_textures);
        resolve_texture_token(material.occlusion_texture, new_textures);
        resolve_texture_token(material.emissive_texture, new_textures);
        result.materials.push_back(material.name);
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
            const auto normal = source->HasNormals() ? source->mNormals[vertex]
                                                     : aiVector3D{0.0F, 0.0F, 1.0F};
            const auto tangent = source->HasTangentsAndBitangents()
                                     ? source->mTangents[vertex] : aiVector3D{1.0F, 0.0F, 0.0F};
            float handedness = 1.0F;
            if (source->HasTangentsAndBitangents()) {
                const auto cross = normal ^ tangent;
                handedness = (cross * source->mBitangents[vertex]) < 0.0F ? -1.0F : 1.0F;
            }
            new_vertices.push_back({p.x, p.y, p.z, uv.x, uv.y,
                                    normal.x, normal.y, normal.z,
                                    tangent.x, tangent.y, tangent.z, handedness});
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
    if (registry.textures().size() + new_textures.size() > bindless_texture_capacity) {
        error = "model textures exceed the 16-slot texture table capacity";
        return result;
    }
    if (!registry.register_imported(std::move(new_vertices), std::move(new_indices),
                                    std::move(new_meshes), std::move(new_materials),
                                    std::move(new_textures))) {
        error = "model assets could not be registered";
        return result;
    }
    if (imported->HasAnimations()) result.warnings.push_back("animations are not imported yet");
    if (has_skeleton) result.warnings.push_back("skeletons and skin weights are not imported yet");
    if (scene != nullptr) {
        instantiate_node(imported->mRootNode, {}, *scene, result.meshes, result.materials,
                         *imported, result.roots);
    }
    result.imported = true;
    return result;
#endif
}

} // namespace relay
