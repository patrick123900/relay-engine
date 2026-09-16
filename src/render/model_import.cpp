#include "relay/core/hash.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/blender_adapter.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/scene/scene.hpp"
#include "gltf_animation.hpp"

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
    if (relative.empty() || relative.begin() == relative.end() ||
        *relative.begin() == std::filesystem::path{".."}) return std::nullopt;
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

bool materialize_zero_accessors(std::vector<std::uint8_t> &bytes, const std::filesystem::path &file,
                                std::string &error) {
    const auto extension = file.extension().string();
    if (extension != ".gltf" && extension != ".glb")
        return true;
    try {
        using Reader = detail::GltfAnimationReader;
        const auto u32 = [&](std::size_t offset) {
            if (offset + 4U > bytes.size())
                throw std::runtime_error("truncated GLB");
            return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8U) |
                   (std::uint32_t(bytes[offset + 2]) << 16U) |
                   (std::uint32_t(bytes[offset + 3]) << 24U);
        };
        const bool glb = extension == ".glb";
        std::string json;
        std::vector<std::uint8_t> binary;
        if (glb) {
            if (bytes.size() < 20U || u32(0) != 0x46546c67U || u32(4) != 2U ||
                u32(8) != bytes.size())
                throw std::runtime_error("invalid GLB header");
            std::size_t offset = 12U;
            while (offset + 8U <= bytes.size()) {
                const auto size = u32(offset), type = u32(offset + 4U);
                offset += 8U;
                if (size > bytes.size() - offset)
                    throw std::runtime_error("truncated GLB chunk");
                if (type == 0x4e4f534aU)
                    json.assign(reinterpret_cast<const char *>(bytes.data() + offset), size);
                else if (type == 0x004e4942U)
                    binary.assign(bytes.begin() + offset, bytes.begin() + offset + size);
                offset += size;
            }
        } else
            json.assign(bytes.begin(), bytes.end());
        JsonParser parser{json};
        auto document = parser.parse();
        if (!document || !document->object())
            throw std::runtime_error("invalid glTF JSON");
        auto &root = std::get<JsonValue::Object>(document->data);
        if (root.contains("skins") && !root.contains("accessors")) root["accessors"]=JsonValue{JsonValue::Array{}};
        auto found = root.find("accessors");
        if (found == root.end())
            return true;
        if (!found->second.array())
            throw std::runtime_error("invalid accessors array");
        bool changed = false;
        std::size_t total = bytes.size();
        if (const auto skins=root.find("skins");skins!=root.end()) {
            if (!skins->second.array()) throw std::runtime_error("invalid skins array");
            for (auto& skin : std::get<JsonValue::Array>(skins->second.data)) {
                if (Reader::optional_field(skin,"inverseBindMatrices")) continue;
                const auto count=Reader::list(Reader::get(skin,"joints")).size();
                if (!count || count>256U) throw std::runtime_error("invalid skin joint count");
                std::vector<std::uint8_t> matrices(count*64U);
                for (std::size_t j=0;j<count;++j) for (unsigned diagonal=0;diagonal<4U;++diagonal) {
                    const auto offset=j*64U+diagonal*20U;matrices[offset+2U]=0x80U;matrices[offset+3U]=0x3fU;
                }
                if (!root.contains("buffers")) root["buffers"]=JsonValue{JsonValue::Array{}};
                if (!root.contains("bufferViews")) root["bufferViews"]=JsonValue{JsonValue::Array{}};
                auto& buffers=std::get<JsonValue::Array>(root["buffers"].data);
                auto& views=std::get<JsonValue::Array>(root["bufferViews"].data);
                std::size_t buffer_index=0,offset=0;
                if (glb) {
                    if (buffers.empty()) buffers.push_back(JsonValue{JsonValue::Object{}});
                    while (binary.size()%4U) binary.push_back(0);
                    offset=binary.size();binary.insert(binary.end(),matrices.begin(),matrices.end());
                    std::get<JsonValue::Object>(buffers[0].data)["byteLength"]=JsonValue{static_cast<double>(binary.size())};
                } else {
                    constexpr std::string_view alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    std::string encoded;
                    for (std::size_t i=0;i<matrices.size();i+=3U) {
                        const auto remaining=matrices.size()-i;
                        const std::uint32_t bits=(std::uint32_t(matrices[i])<<16U)|
                            (remaining>1U?std::uint32_t(matrices[i+1U])<<8U:0U)|(remaining>2U?matrices[i+2U]:0U);
                        encoded+=alphabet[(bits>>18U)&63U];encoded+=alphabet[(bits>>12U)&63U];
                        encoded+=remaining>1U?alphabet[(bits>>6U)&63U]:'=';encoded+=remaining>2U?alphabet[bits&63U]:'=';
                    }
                    buffer_index=buffers.size();
                    buffers.push_back(JsonValue{JsonValue::Object{{"byteLength",JsonValue{static_cast<double>(matrices.size())}},
                        {"uri",JsonValue{"data:application/octet-stream;base64,"+encoded}}}});
                }
                auto& accessors=std::get<JsonValue::Array>(found->second.data);
                std::get<JsonValue::Object>(skin.data)["inverseBindMatrices"]=JsonValue{static_cast<double>(accessors.size())};
                accessors.push_back(JsonValue{JsonValue::Object{{"bufferView",JsonValue{static_cast<double>(views.size())}},
                    {"componentType",JsonValue{5126.0}},{"count",JsonValue{static_cast<double>(count)}},{"type",JsonValue{std::string{"MAT4"}}}}});
                views.push_back(JsonValue{JsonValue::Object{{"buffer",JsonValue{static_cast<double>(buffer_index)}},
                    {"byteOffset",JsonValue{static_cast<double>(offset)}},{"byteLength",JsonValue{static_cast<double>(matrices.size())}}}});
                changed=true;total+=matrices.size();
                if (total>max_dependency_bytes) throw std::runtime_error("skin normalization exceeds 64 MiB");
            }
        }
        for (auto &accessor : std::get<JsonValue::Array>(found->second.data)) {
            if (!accessor.object())
                throw std::runtime_error("invalid accessor object");
            auto &a = std::get<JsonValue::Object>(accessor.data);
            if (a.contains("bufferView"))
                continue;
            const auto count = Reader::integer(Reader::get(accessor, "count"));
            const auto kind = Reader::integer(Reader::get(accessor, "componentType"));
            const auto shape = Reader::string(Reader::get(accessor, "type"));
            const auto width = kind == 5120 || kind == 5121   ? 1U
                               : kind == 5122 || kind == 5123 ? 2U
                               : kind == 5125 || kind == 5126 ? 4U
                                                              : 0U;
            const auto components = shape == "SCALAR"                    ? 1U
                                    : shape == "VEC2"                    ? 2U
                                    : shape == "VEC3"                    ? 3U
                                    : shape == "VEC4" || shape == "MAT2" ? 4U
                                    : shape == "MAT3"                    ? 9U
                                    : shape == "MAT4"                    ? 16U
                                                                         : 0U;
            if (!width || !components || count > 1'000'000U ||
                (shape.starts_with("MAT") && width != 4U))
                throw std::runtime_error("unsupported zero accessor layout");
            const auto size = count * width * components;
            total += size;
            if (total > max_dependency_bytes)
                throw std::runtime_error("zero accessors exceed 64 MiB normalization limit");
            if (!root.contains("buffers"))
                root["buffers"] = JsonValue{JsonValue::Array{}};
            if (!root.contains("bufferViews"))
                root["bufferViews"] = JsonValue{JsonValue::Array{}};
            auto &buffers = std::get<JsonValue::Array>(root["buffers"].data);
            auto &views = std::get<JsonValue::Array>(root["bufferViews"].data);
            std::size_t buffer_index = 0, offset = 0;
            if (glb) {
                if (buffers.empty())
                    buffers.push_back(JsonValue{JsonValue::Object{}});
                while (binary.size()%4U) binary.push_back(0);
                offset = binary.size();
                binary.resize(offset + size);
                std::get<JsonValue::Object>(buffers[0].data)["byteLength"] =
                    JsonValue{static_cast<double>(binary.size())};
            } else {
                buffer_index = buffers.size();
                std::string encoded((size + 2U) / 3U * 4U, 'A');
                if (size % 3U) {
                    encoded.back() = '=';
                    if (size % 3U == 1U)
                        encoded[encoded.size() - 2U] = '=';
                }
                buffers.push_back(JsonValue{JsonValue::Object{
                    {"byteLength", JsonValue{static_cast<double>(size)}},
                    {"uri", JsonValue{"data:application/octet-stream;base64," + encoded}}}});
            }
            a["bufferView"] = JsonValue{static_cast<double>(views.size())};
            a["byteOffset"] = JsonValue{0.0};
            views.push_back(JsonValue{
                JsonValue::Object{{"buffer", JsonValue{static_cast<double>(buffer_index)}},
                                  {"byteOffset", JsonValue{static_cast<double>(offset)}},
                                  {"byteLength", JsonValue{static_cast<double>(size)}}}});
            changed = true;
        }
        if (!changed)
            return true;
        json = json_stringify(*document);
        if (!glb)
            bytes.assign(json.begin(), json.end());
        else {
            while (json.size() % 4U)
                json += ' ';
            while (binary.size() % 4U)
                binary.push_back(0);
            bytes.clear();
            const auto append = [&](std::uint32_t value) {
                for (unsigned i = 0; i < 4; ++i)
                    bytes.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
            };
            append(0x46546c67U);
            append(2U);
            append(static_cast<std::uint32_t>(28U + json.size() + binary.size()));
            append(static_cast<std::uint32_t>(json.size()));
            append(0x4e4f534aU);
            bytes.insert(bytes.end(), json.begin(), json.end());
            append(static_cast<std::uint32_t>(binary.size()));
            append(0x004e4942U);
            bytes.insert(bytes.end(), binary.begin(), binary.end());
        }
        if (bytes.size() > max_dependency_bytes)
            throw std::runtime_error("normalized glTF exceeds 64 MiB");
        return true;
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    }
}

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
        std::string normalization_error;
        if (!materialize_zero_accessors(bytes, *resolved, normalization_error)) {
            reject("glTF normalization failed: " + normalization_error);
            return nullptr;
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
    float range = 0.0F;
    if (node.mMetaData && node.mMetaData->Get("PBR_LightRange", range)) {
        constexpr std::string_view marker = "PBR_LightRange";
        hash_bytes(digest, marker.data(), marker.size());
        hash_bytes(digest, &range, sizeof(range));
    }
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
    if (scene.mNumCameras != 0U) {
        constexpr std::string_view marker = "relay-cameras-v1";
        hash_bytes(digest, marker.data(), marker.size());
        hash_bytes(digest, &scene.mNumCameras, sizeof(scene.mNumCameras));
        for (unsigned index = 0; index < scene.mNumCameras; ++index) {
            const auto& camera = *scene.mCameras[index];
            hash_bytes(digest, camera.mName.C_Str(), camera.mName.length);
            hash_bytes(digest, &camera.mPosition, sizeof(camera.mPosition));
            hash_bytes(digest, &camera.mLookAt, sizeof(camera.mLookAt));
            hash_bytes(digest, &camera.mUp, sizeof(camera.mUp));
            hash_bytes(digest, &camera.mHorizontalFOV, sizeof(camera.mHorizontalFOV));
            hash_bytes(digest, &camera.mAspect, sizeof(camera.mAspect));
            hash_bytes(digest, &camera.mClipPlaneNear, sizeof(camera.mClipPlaneNear));
            hash_bytes(digest, &camera.mClipPlaneFar, sizeof(camera.mClipPlaneFar));
            hash_bytes(digest, &camera.mOrthographicWidth, sizeof(camera.mOrthographicWidth));
        }
    }
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

struct ImportedCamera {
    Transform transform;
    Camera camera;
};

std::optional<ImportedCamera> imported_camera(const aiCamera& source) {
    auto forward = source.mLookAt;
    auto up = source.mUp;
    const auto finite_vector = [](const aiVector3D& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite_vector(forward) || !finite_vector(up) || !finite_vector(source.mPosition) ||
        forward.SquareLength() < 1e-12F || up.SquareLength() < 1e-12F) return std::nullopt;
    forward.Normalize();
    auto right = forward ^ up;
    if (right.SquareLength() < 1e-12F) return std::nullopt;
    right.Normalize();
    up = right ^ forward;
    // Relay cameras look along local -Z. Assimp's vectors are relative to the named node.
    const aiMatrix4x4 pose{right.x, up.x, -forward.x, source.mPosition.x,
                          right.y, up.y, -forward.y, source.mPosition.y,
                          right.z, up.z, -forward.z, source.mPosition.z,
                          0.0F, 0.0F, 0.0F, 1.0F};
    const double aspect = source.mAspect == 0.0F ? 1.0 : source.mAspect;
    const bool ortho = source.mOrthographicWidth != 0.0F;
    const double fov =
        ortho ? 60.0
              : 2.0 * std::atan(std::tan(source.mHorizontalFOV) / aspect) * 57.295779513082320876;
    if (!std::isfinite(aspect) || aspect <= 0.0 || !std::isfinite(source.mOrthographicWidth) ||
        source.mOrthographicWidth < 0.0F ||
        (!ortho && (!std::isfinite(source.mHorizontalFOV) || source.mHorizontalFOV <= 0.0F ||
                    source.mHorizontalFOV >= 1.5707963267948966)) ||
        !std::isfinite(fov) || fov <= 1.0 || fov >= 179.0 ||
        !std::isfinite(source.mClipPlaneNear) || !std::isfinite(source.mClipPlaneFar) ||
        source.mClipPlaneNear <= 0.0F || source.mClipPlaneFar <= source.mClipPlaneNear) {
        return std::nullopt;
    }
    return ImportedCamera{imported_transform(pose),
                          Camera{fov, source.mClipPlaneNear, source.mClipPlaneFar, false,
                                 ortho ? 2.0 * source.mOrthographicWidth / aspect : 0.0}};
}

Entity instantiate_node(const aiNode *node, const Entity parent, Scene &scene,
                        const std::vector<std::string> &mesh_names,
                        const std::vector<std::string> &material_names, const aiScene &imported,
                        std::vector<Entity> &roots, const bool include_cameras,
                        const std::string &model,
                        const std::unordered_map<const aiNode *, std::uint32_t> &node_indices,
                        Entity root_entity = {}) {
    auto name = std::string{node->mName.C_Str()};
    if (name.empty()) name = "Imported Node";
    const auto entity = scene.create(name, parent);
    (void)scene.set_transform(entity, imported_transform(node->mTransformation));
    if (!parent.valid()) roots.push_back(entity);
    if (!root_entity.valid())
        root_entity = entity;
    scene.get(entity)->model_node = ModelNode{root_entity, node_indices.at(node)};
    if (entity == root_entity)
        (void)scene.set_animator(entity, Animator{model});
    if (include_cameras) {
        for (unsigned index = 0; index < imported.mNumCameras; ++index) {
            const auto& source = *imported.mCameras[index];
            if (source.mName != node->mName) continue;
            if (const auto camera = imported_camera(source)) {
                const auto child = scene.create(name + " Camera", entity);
                (void)scene.set_transform(child, camera->transform);
                (void)scene.set_camera(child, camera->camera);
            }
        }
        for (unsigned index = 0; index < imported.mNumLights; ++index) {
            const auto &source = *imported.mLights[index];
            if (source.mName != node->mName)
                continue;
            Light light;
            if (source.mType == aiLightSource_DIRECTIONAL)
                light.type = Light::Type::directional;
            else if (source.mType == aiLightSource_POINT)
                light.type = Light::Type::point;
            else if (source.mType == aiLightSource_SPOT)
                light.type = Light::Type::spot;
            else
                continue;
            light.color = {source.mColorDiffuse.r, source.mColorDiffuse.g, source.mColorDiffuse.b};
            light.attenuation = {source.mAttenuationConstant, source.mAttenuationLinear,
                                 source.mAttenuationQuadratic};
            float range = 0.0F;
            if (node->mMetaData && node->mMetaData->Get("PBR_LightRange", range))
                light.range = range;
            if (light.type == Light::Type::spot) {
                light.inner_cone = source.mAngleInnerCone;
                light.outer_cone = source.mAngleOuterCone;
            }
            Transform pose;
            if (light.type == Light::Type::point) {
                pose.position = {source.mPosition.x, source.mPosition.y, source.mPosition.z};
            } else {
                aiCamera orientation;
                orientation.mPosition = source.mPosition;
                orientation.mLookAt = source.mDirection;
                orientation.mUp = source.mUp;
                if (orientation.mUp.SquareLength()<1e-12F) orientation.mUp=aiVector3D{0.0F,1.0F,0.0F};
                auto direction=source.mDirection;direction.Normalize();
                auto up=orientation.mUp;up.Normalize();
                if (std::abs(direction*up)>0.999F) orientation.mUp=aiVector3D{1.0F,0.0F,0.0F};
                const auto converted = imported_camera(orientation);
                if (!converted)
                    continue;
                pose = converted->transform;
            }
            const auto child = scene.create(name + " Light", entity);
            (void)scene.set_transform(child, pose);
            if (!scene.set_light(child, light))
                (void)scene.destroy(child);
        }
    }
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
                               imported, roots, include_cameras, model, node_indices, root_entity);
    }
    return entity;
}

std::array<float, 16> imported_matrix(const aiMatrix4x4 &m) {
    return {m.a1, m.b1, m.c1, m.d1, m.a2, m.b2, m.c2, m.d2,
            m.a3, m.b3, m.c3, m.d3, m.a4, m.b4, m.c4, m.d4};
}

bool collect_model(const aiScene &source, ModelAsset &model,
                   std::unordered_map<const aiNode *, std::uint32_t> &indices, std::string &error,
                   bool animations = true) {
    std::unordered_map<std::string, std::vector<std::uint32_t>> names;
    const auto walk = [&](const auto &self, const aiNode *node, unsigned depth) -> bool {
        if (depth > 256U || model.nodes.size() >= 8192U)
            return false;
        const auto index = static_cast<std::uint32_t>(model.nodes.size());
        indices.emplace(node, index);
        model.nodes.emplace_back(node->mName.C_Str());
        const auto parent =
            node->mParent ? static_cast<std::int32_t>(indices.at(node->mParent)) : -1;
        model.parents.push_back(parent);
        model.rest_transforms.push_back(imported_transform(node->mTransformation));
        Sha256 layout;
        layout.update(node->mName.C_Str(), node->mName.length);
        layout.update(&parent, sizeof(parent));
        layout.update(&node->mTransformation, sizeof(node->mTransformation));
        model.binding_layout.push_back(layout.hex());
        names[model.nodes.back()].push_back(index);
        const auto matrix = imported_matrix(node->mTransformation);
        for (const auto value : matrix)
            if (!std::isfinite(value))
                return false;
        for (unsigned i = 0; i < node->mNumChildren; ++i)
            if (!self(self, node->mChildren[i], depth + 1U))
                return false;
        return true;
    };
    if (!walk(walk, source.mRootNode, 0U) || source.mNumAnimations > 256U ||
        source.mNumMeshes > 4096U) {
        error = "model exceeds node, hierarchy-depth, mesh or animation limits";
        return false;
    }
    std::size_t keys = 0;
    if (!animations)
        return true;
    for (unsigned c = 0; c < source.mNumAnimations; ++c) {
        const auto &animation = *source.mAnimations[c];
        if (animation.mNumChannels > 8192U || animation.mNumMorphMeshChannels > 4096U ||
            animation.mNumMeshChannels != 0U) {
            error = "unsupported vertex-cache animation or excessive animation channels";
            return false;
        }
        const double rate = animation.mTicksPerSecond == 0.0 ? 25.0 : animation.mTicksPerSecond;
        if (!std::isfinite(rate) || rate <= 0.0 || !std::isfinite(animation.mDuration) ||
            animation.mDuration < 0.0) {
            error = "invalid animation timing";
            return false;
        }
        AnimationClip clip;
        clip.name = animation.mName.length ? animation.mName.C_Str() : "Clip " + std::to_string(c);
        clip.duration_seconds = animation.mDuration / rate;
        for (unsigned t = 0; t < animation.mNumChannels; ++t) {
            const auto &channel = *animation.mChannels[t];
            const auto found = names.find(channel.mNodeName.C_Str());
            if (found == names.end() || found->second.size() != 1U) {
                error = "animation targets a missing or ambiguous node";
                return false;
            }
            keys += channel.mNumPositionKeys + static_cast<std::size_t>(channel.mNumRotationKeys) +
                    channel.mNumScalingKeys;
            if (keys > 1'000'000U) {
                error = "animation exceeds the key limit";
                return false;
            }
            NodeTrack track;
            track.node = found->second.front();
            const auto vectors = [&](const aiVectorKey *input, unsigned count,
                                     std::vector<VectorKey> &output) {
                double previous = -1.0;
                for (unsigned k = 0; k < count; ++k) {
                    const auto &key = input[k];
                    if (!std::isfinite(key.mTime) || key.mTime < 0.0 || key.mTime <= previous ||
                        !std::isfinite(key.mValue.x) || !std::isfinite(key.mValue.y) ||
                        !std::isfinite(key.mValue.z))
                        return false;
                    previous = key.mTime;
                    output.push_back(
                        {key.mTime / rate, {key.mValue.x, key.mValue.y, key.mValue.z}});
                }
                return true;
            };
            if (!vectors(channel.mPositionKeys, channel.mNumPositionKeys, track.positions) ||
                !vectors(channel.mScalingKeys, channel.mNumScalingKeys, track.scales)) {
                error = "invalid or unordered vector animation keys";
                return false;
            }
            double previous = -1.0;
            for (unsigned k = 0; k < channel.mNumRotationKeys; ++k) {
                const auto &key = channel.mRotationKeys[k];
                const auto &q = key.mValue;
                const double length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                if (!std::isfinite(key.mTime) || key.mTime < 0.0 || key.mTime <= previous ||
                    !std::isfinite(length) || length < 1e-12) {
                    error = "invalid or unordered rotation animation keys";
                    return false;
                }
                previous = key.mTime;
                track.rotations.push_back(
                    {key.mTime / rate, {q.x / length, q.y / length, q.z / length, q.w / length}});
            }
            clip.tracks.push_back(std::move(track));
        }
        for (unsigned t = 0; t < animation.mNumMorphMeshChannels; ++t) {
            const auto &channel = *animation.mMorphMeshChannels[t];
            keys += channel.mNumKeys;
            if (keys > 1'000'000U) {
                error = "morph animation exceeds key limit";
                return false;
            }
            std::vector<unsigned> targets;
            for (unsigned m = 0; m < source.mNumMeshes; ++m) {
                if (source.mMeshes[m]->mName == channel.mName)
                    targets.push_back(m);
            }
            if (targets.empty()) {
                const auto *node = source.mRootNode->FindNode(channel.mName);
                if (node)
                    targets.assign(node->mMeshes, node->mMeshes + node->mNumMeshes);
            }
            // Assimp's FBX backend names morph channels "node*local-mesh-index".
            if (targets.empty()) {
                const std::string name{channel.mName.C_Str()};
                const auto separator = name.find_last_of('*');
                if (separator != std::string::npos) {
                    const auto *node =
                        source.mRootNode->FindNode(name.substr(0, separator).c_str());
                    unsigned mesh_index = 0;
                    const auto parsed = std::from_chars(name.data() + separator + 1U,
                                                        name.data() + name.size(), mesh_index);
                    if (node && parsed.ec == std::errc{} &&
                        parsed.ptr == name.data() + name.size() && mesh_index < node->mNumMeshes)
                        targets.push_back(node->mMeshes[mesh_index]);
                }
            }
            if (targets.empty()) {
                error =
                    "morph animation targets a missing mesh: " + std::string{channel.mName.C_Str()};
                for (unsigned m = 0; m < source.mNumMeshes; ++m)
                    error += " [" + std::string{source.mMeshes[m]->mName.C_Str()} + "]";
                return false;
            }
            for (const auto m : targets) {
                MorphTrack track;
                track.mesh = m;
                double previous = -1.0;
                for (unsigned k = 0; k < channel.mNumKeys; ++k) {
                    const auto &key = channel.mKeys[k];
                    if (!std::isfinite(key.mTime) || key.mTime < 0.0 || key.mTime <= previous ||
                        key.mNumValuesAndWeights > 64U) {
                        error = "invalid morph animation key";
                        return false;
                    }
                    previous = key.mTime;
                    MorphKey output{key.mTime / rate,
                                    std::vector<double>(source.mMeshes[m]->mNumAnimMeshes)};
                    for (unsigned w = 0; w < key.mNumValuesAndWeights; ++w) {
                        if (key.mValues[w] >= output.weights.size() ||
                            !std::isfinite(key.mWeights[w]) || std::abs(key.mWeights[w]) > 100.0) {
                            error = "invalid morph animation weight";
                            return false;
                        }
                        output.weights[key.mValues[w]] = key.mWeights[w];
                    }
                    track.keys.push_back(std::move(output));
                }
                clip.morph_tracks.push_back(std::move(track));
            }
        }
        model.clips.push_back(std::move(clip));
    }
    return true;
}

bool collect_gltf_clips(const std::filesystem::path &path, SandboxedIOSystem &io,
                        const aiScene &source, ModelAsset &model,
                        const std::unordered_map<const aiNode *, std::uint32_t> &indices,
                        std::unordered_map<unsigned, std::vector<std::uint32_t>> &skin_nodes,
                        std::string &error) {
    try {
        using Reader = detail::GltfAnimationReader;
        Reader reader{path.generic_string(),
                      [&](const std::string &file, std::vector<std::uint8_t> &bytes) {
                          return io.read_bytes(file.c_str(), bytes);
                      }};
        const auto *animations = field(*reader.document().object(), "animations");
        const JsonValue::Array empty_clips;
        const auto &clips = animations ? Reader::list(*animations) : empty_clips;
        if (clips.size() > 256U)
            throw std::runtime_error("too many animation clips");
        const auto &nodes = reader.array("nodes");
        std::unordered_map<std::size_t, const aiNode *> mapped;
        const auto map_node = [&](const auto &self, std::size_t original, const aiNode *imported,
                                  unsigned depth) -> void {
            if (depth > 256U || mapped.size() >= 8192U)
                throw std::runtime_error("excessive animation hierarchy");
            if (!mapped.emplace(original, imported).second)
                throw std::runtime_error("shared or cyclic glTF nodes");
            const auto &node = Reader::at(nodes, original);
            const auto *children = Reader::optional_field(node, "children");
            const auto count = children ? Reader::list(*children).size() : 0U;
            if (count != imported->mNumChildren)
                throw std::runtime_error("glTF hierarchy does not match normalized nodes");
            for (std::size_t c = 0; c < count; ++c)
                self(self, Reader::integer(Reader::list(*children)[c]), imported->mChildren[c],
                     depth + 1U);
        };
        const auto scene_index = Reader::optional_integer(reader.document(), "scene");
        const auto &scene = Reader::at(reader.array("scenes"), scene_index);
        const auto &roots = Reader::list(Reader::get(scene, "nodes"));
        if (roots.size() == 1U)
            map_node(map_node, Reader::integer(roots.front()), source.mRootNode, 0U);
        else {
            if (roots.size() != source.mRootNode->mNumChildren)
                throw std::runtime_error("glTF scene roots do not match import");
            for (std::size_t i = 0; i < roots.size(); ++i)
                map_node(map_node, Reader::integer(roots[i]), source.mRootNode->mChildren[i], 0U);
        }
        for (const auto &[original, node] : mapped) {
            const auto *skin = Reader::optional_field(Reader::at(nodes, original), "skin");
            if (!skin)
                continue;
            const auto &input = Reader::at(reader.array("skins"), Reader::integer(*skin));
            std::vector<std::uint32_t> joints;
            for (const auto &joint : Reader::list(Reader::get(input, "joints"))) {
                const auto found = mapped.find(Reader::integer(joint));
                if (found == mapped.end())
                    throw std::runtime_error("skin joint outside selected scene");
                joints.push_back(indices.at(found->second));
            }
            for (unsigned m = 0; m < node->mNumMeshes; ++m) {
                const auto mesh = node->mMeshes[m];
                if (joints.size() != source.mMeshes[mesh]->mNumBones)
                    throw std::runtime_error("skin joint count mismatch");
                const auto found = skin_nodes.find(mesh);
                if (found != skin_nodes.end() && found->second != joints)
                    throw std::runtime_error(
                        "shared mesh with different skins needs separate source meshes");
                skin_nodes[mesh] = joints;
            }
        }
        std::size_t total_keys = 0;
        for (std::size_t c = 0; c < clips.size(); ++c) {
            const auto &input = clips[c];
            AnimationClip clip;
            const auto *name = Reader::optional_field(input, "name");
            clip.name = name ? Reader::string(*name) : "Clip " + std::to_string(c);
            const auto &samplers = Reader::list(Reader::get(input, "samplers"));
            const auto &channels = Reader::list(Reader::get(input, "channels"));
            if (channels.size() > 8192U || samplers.size() > 8192U)
                throw std::runtime_error("excessive glTF animation channels");
            std::map<std::pair<std::size_t, std::string>, bool> seen;
            for (const auto &channel : channels) {
                const auto &target = Reader::get(channel, "target");
                const auto original = Reader::integer(Reader::get(target, "node"));
                const auto target_path = Reader::string(Reader::get(target, "path"));
                if (!seen.emplace(std::make_pair(original, target_path), true).second)
                    throw std::runtime_error("duplicate animation channel");
                const auto found = mapped.find(original);
                if (found == mapped.end())
                    throw std::runtime_error("animation targets node outside selected scene");
                const auto *node = found->second;
                const auto &sampler =
                    Reader::at(samplers, Reader::integer(Reader::get(channel, "sampler")));
                const auto *mode = Reader::optional_field(sampler, "interpolation");
                const auto interpolation = mode ? Reader::string(*mode) : "LINEAR";
                const auto method = interpolation == "STEP" ? AnimationInterpolation::step
                                    : interpolation == "CUBICSPLINE"
                                        ? AnimationInterpolation::cubic
                                        : AnimationInterpolation::linear;
                if (interpolation != "LINEAR" && interpolation != "STEP" &&
                    interpolation != "CUBICSPLINE")
                    throw std::runtime_error("unsupported animation interpolation");
                const bool cubic = method == AnimationInterpolation::cubic;
                const auto times =
                    reader.accessor(Reader::integer(Reader::get(sampler, "input")), 1);
                total_keys += times.size();
                if (total_keys > 1'000'000U)
                    throw std::runtime_error("excessive animation keys");
                for (std::size_t k = 0; k < times.size(); ++k)
                    if (times[k] < 0.0 || (k && times[k] <= times[k - 1]))
                        throw std::runtime_error("invalid animation key order");
                clip.duration_seconds = std::max(clip.duration_seconds, times.back());
                unsigned components = target_path == "rotation"  ? 4U
                                      : target_path == "weights" ? 1U
                                                                 : 3U;
                const auto values =
                    reader.accessor(Reader::integer(Reader::get(sampler, "output")), components);
                if (target_path == "weights") {
                    for (unsigned m = 0; m < node->mNumMeshes; ++m) {
                        const auto mesh_index = node->mMeshes[m];
                        const auto count = source.mMeshes[mesh_index]->mNumAnimMeshes;
                        if (count == 0 || count > 64U ||
                            values.size() != times.size() * count * (cubic ? 3U : 1U))
                            throw std::runtime_error("morph animation sample count mismatch");
                        MorphTrack track;
                        track.mesh = mesh_index;
                        track.interpolation = method;
                        for (std::size_t k = 0; k < times.size(); ++k) {
                            const auto begin = k * count * (cubic ? 3U : 1U);
                            MorphKey key;
                            key.time = times[k];
                            const auto value_begin = begin + (cubic ? count : 0U);
                            key.weights.assign(values.begin() + value_begin,
                                               values.begin() + value_begin + count);
                            for (const auto w : key.weights)
                                if (std::abs(w) > 100.0)
                                    throw std::runtime_error(
                                        "morph animation weight outside bounds");
                            if (cubic) {
                                key.in_tangent.assign(values.begin() + begin,
                                                      values.begin() + begin + count);
                                key.out_tangent.assign(values.begin() + begin + 2 * count,
                                                       values.begin() + begin + 3 * count);
                            }
                            track.keys.push_back(std::move(key));
                        }
                        clip.morph_tracks.push_back(std::move(track));
                    }
                } else {
                    if (target_path != "rotation" && target_path != "translation" &&
                        target_path != "scale")
                        throw std::runtime_error("unsupported animation target");
                    if (values.size() != times.size() * components * (cubic ? 3U : 1U))
                        throw std::runtime_error("animation sample count mismatch");
                    const auto index = indices.at(node);
                    auto track = std::find_if(clip.tracks.begin(), clip.tracks.end(),
                                              [&](const NodeTrack &t) { return t.node == index; });
                    if (track == clip.tracks.end()) {
                        clip.tracks.push_back(NodeTrack{});
                        track = clip.tracks.end() - 1;
                        track->node = index;
                    }
                    for (std::size_t k = 0; k < times.size(); ++k) {
                        const auto begin = k * components * (cubic ? 3U : 1U),
                                   value = begin + (cubic ? components : 0U);
                        if (target_path == "rotation") {
                            RotationKey key;
                            key.time = times[k];
                            double length = 0;
                            for (unsigned v = 0; v < 4; ++v) {
                                key.value[v] = values[value + v];
                                length += key.value[v] * key.value[v];
                                if (cubic) {
                                    key.in_tangent[v] = values[begin + v];
                                    key.out_tangent[v] = values[begin + 8 + v];
                                }
                            }
                            if (length < 1e-12 || std::abs(length - 1.0) > 0.001)
                                throw std::runtime_error(
                                    "rotation animation quaternion is not normalized");
                            track->rotations.push_back(key);
                            track->rotation_interpolation = method;
                        } else {
                            VectorKey key{times[k],
                                          {values[value], values[value + 1], values[value + 2]}};
                            if (cubic) {
                                key.in_tangent = {values[begin], values[begin + 1],
                                                  values[begin + 2]};
                                key.out_tangent = {values[begin + 6], values[begin + 7],
                                                   values[begin + 8]};
                            }
                            if (target_path == "translation") {
                                track->positions.push_back(key);
                                track->position_interpolation = method;
                            } else {
                                track->scales.push_back(key);
                                track->scale_interpolation = method;
                            }
                        }
                    }
                }
            }
            model.clips.push_back(std::move(clip));
        }
        return true;
    } catch (const std::exception &exception) {
        error = "glTF animation import failed: " + std::string{exception.what()};
        return false;
    }
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
    output << "],\"adapter\":\"" << escape_json(source_adapter)
           << "\",\"preset\":\"" << escape_json(preset)
           << "\",\"conversion_cache_hit\":" << (conversion_cache_hit ? "true" : "false")
           << ",\"conversion_sandboxed\":" << (conversion_sandboxed ? "true" : "false")
           << ",\"conversion_diagnostics\":\"" << escape_json(conversion_diagnostics) << '"'
           << ",\"dependencies\":[";
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(dependencies[i]) << '"';
    }
    output << "],\"model\":\"" << escape_json(model) << "\",\"warnings\":[";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        if (i) output << ',';
        output << '"' << escape_json(warnings[i]) << '"';
    }
    output << "]}";
    return output.str();
}

std::string model_import_capabilities_json() {
#ifdef RELAY_HAS_ASSIMP
    return R"({"available":true,"native_recommended":[".gltf",".glb"],"formats":[".gltf",".glb",".fbx",".obj",".dae",".blend"],"backend":"Assimp","identity":"sha256-v1","image_decoders":["png","jpeg"],"presets":["scene","static_mesh"],"sandbox":{"root":"assets","max_dependency_files":64,"max_dependency_bytes":67108864},"adapters":{".blend":{"backend":"Blender glTF exporter","cache":"assets/.relay-cache/blender","autoexec":false,"timeout_seconds":120,"sandbox":"Bubblewrap on Linux; other platforms require administrator trusted-input opt-in"}},"notes":{"materials":"glTF metallic-roughness factors and base-color, metallic-roughness, normal, occlusion and emissive textures are imported","animation":"engine-owned node and morph clips with deterministic playback; native glTF LINEAR, STEP and CUBICSPLINE interpolation","skinning":"up to 256 joints per mesh and eight normalized influences per vertex; CPU deformation streamed to Vulkan","morphs":"up to 64 targets, position/normal/tangent deltas and animated or manual weights","cameras":"perspective and orthographic, initially inactive; camera-only scenes supported","lights":"directional, point and spot; up to 16 rendered scene lights"}})";
#else
    return R"({"available":false,"native_recommended":[".gltf",".glb"],"formats":[],"backend":"none","identity":"sha256-v1","reason":"Relay was built without Assimp"})";
#endif
}

ModelImportResult import_model_asset(const std::filesystem::path& assets_root,
                                     const std::string_view filename, AssetRegistry& registry,
                                     Scene* scene, std::string& error,
                                     const ModelImportSettings& settings) {
    ModelImportResult result;
    result.source_format = std::filesystem::path{filename}.extension().string();
    result.preset = settings.preset;
    if (settings.preset != "scene" && settings.preset != "static_mesh") {
        error = "model import preset must be 'scene' or 'static_mesh'";
        return result;
    }

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
    std::filesystem::path import_path = std::filesystem::path{filename};
    if (result.source_format == ".blend") {
        auto blender_settings = settings.blender;
        if (settings.preset == "static_mesh") {
            blender_settings.export_animations = false;
            blender_settings.export_cameras = false;
            blender_settings.export_lights = false;
        }
        const auto converted = convert_blend_to_glb(root, *model_path, blender_settings, error);
        if (!converted.converted) {
            if (!converted.diagnostics.empty()) {
                const auto end = std::min<std::size_t>(converted.diagnostics.size(), 1024U);
                error += ": " + converted.diagnostics.substr(0U, end);
            }
            return result;
        }
        import_path = converted.converted_model;
        result.source_adapter = "blender-glb";
        result.conversion_cache_hit = converted.cache_hit;
        result.conversion_sandboxed = converted.sandboxed;
        result.conversion_diagnostics =
            converted.diagnostics.substr(0U, std::min<std::size_t>(4096U,
                                                                   converted.diagnostics.size()));
        result.dependencies.push_back(std::filesystem::path{filename}.generic_string());
        for (const auto& dependency : converted.dependencies) {
            result.dependencies.push_back(dependency.generic_string());
        }
    }
    Assimp::Importer importer;
    // The importer takes ownership of the handler; the observer stays valid while it is alive.
    auto* sandbox = new SandboxedIOSystem(root);
    importer.SetIOHandler(sandbox);
    const auto *imported = importer.ReadFile(
        import_path.generic_string(),
        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality |
            aiProcess_SortByPType | aiProcess_ValidateDataStructure | aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace | aiProcess_PopulateArmatureData);
    if (imported == nullptr || imported->mRootNode == nullptr) {
        if (result.source_format == ".blend" && result.conversion_cache_hit) {
            std::error_code remove_error;
            std::filesystem::remove(root / import_path, remove_error);
            if (!remove_error) {
                return import_model_asset(assets_root, filename, registry, scene, error, settings);
            }
        }
        error = sandbox->rejection().empty()
                    ? "model import failed: " + std::string{importer.GetErrorString()}
                    : "model import blocked: " + sandbox->rejection();
        return result;
    }
    if (imported->mNumCameras > 4096U) {
        error = "model exceeds the 4096-camera import limit";
        return result;
    }
    if (imported->mNumLights > 4096U || imported->mNumMeshes > 4096U) {
        error = "model exceeds light or mesh limits";
        return result;
    }
    std::size_t vertices = 0, morph_bytes = 0;
    for (unsigned m = 0; m < imported->mNumMeshes; ++m) {
        const auto &mesh = *imported->mMeshes[m];
        vertices += mesh.mNumVertices;
        if (vertices > 1'000'000U || mesh.mNumBones > 256U || mesh.mNumAnimMeshes > 64U ||
            mesh.mNumFaces > 2'000'000U) {
            error = "model exceeds geometry, joint or morph-target limits";
            return result;
        }
        morph_bytes +=
            static_cast<std::size_t>(mesh.mNumAnimMeshes) * mesh.mNumVertices * sizeof(MeshVertex);
        if (morph_bytes > 64U * 1024U * 1024U) {
            error = "morph payload exceeds 64 MiB";
            return result;
        }
        for (unsigned t = 0; t < mesh.mNumAnimMeshes; ++t)
            if (mesh.mAnimMeshes[t]->mNumVertices != mesh.mNumVertices) {
                error = "morph vertex count does not match mesh";
                return result;
            }
        for (unsigned v = 0; v < mesh.mNumVertices; ++v) {
            const auto &p = mesh.mVertices[v];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
                error = "mesh contains nonfinite positions";
                return result;
            }
        }
    }
    ModelAsset new_model;
    std::unordered_map<const aiNode *, std::uint32_t> node_indices;
    const auto normalized_extension = import_path.extension().string();
    const bool gltf = normalized_extension == ".gltf" || normalized_extension == ".glb";
    std::unordered_map<unsigned, std::vector<std::uint32_t>> gltf_skin_nodes;
    if (!collect_model(*imported, new_model, node_indices, error, !gltf))
        return result;
    if (gltf && !collect_gltf_clips(import_path, *sandbox, *imported, new_model, node_indices,
                                    gltf_skin_nodes, error))
        return result;
    // Assimp's glTF2 importer stores the FULL horizontal angle, unlike aiCamera's documented
    // half-angle convention. Normalize it before using the common camera conversion path.
    const auto import_extension = import_path.extension().string();
    if (import_extension == ".gltf" || import_extension == ".glb") {
        for (unsigned index = 0; index < imported->mNumCameras; ++index) {
            imported->mCameras[index]->mHorizontalFOV *= 0.5F;
        }
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
        const auto relative = dependency.lexically_relative(root).generic_string();
        if (std::find(result.dependencies.begin(), result.dependencies.end(), relative) ==
            result.dependencies.end()) {
            result.dependencies.push_back(relative);
        }
    }
    // Identity covers normalized geometry, generated normals/tangents, material factors and decoded
    // image pixels, so changes in external image dependencies produce new durable asset IDs.
    result.content_id = imported_content_hash(*imported, new_materials, new_textures);
    // Dynamic data and presets are part of the durable identity too. Use a separately framed
    // digest so static-only models retain the existing geometry/material identity scheme.
    if (imported->HasAnimations() || imported->HasLights() || settings.preset == "static_mesh" ||
        std::any_of(imported->mMeshes, imported->mMeshes + imported->mNumMeshes,
                    [](const aiMesh *mesh) { return mesh->HasBones() || mesh->mNumAnimMeshes; })) {
        Sha256 dynamic_digest;
        const auto hash = [&](const auto &value) { dynamic_digest.update(&value, sizeof(value)); };
        const auto text = [&](const std::string_view value) {
            const auto size = value.size();
            hash(size);
            dynamic_digest.update(value.data(), size);
        };
        text(result.content_id);
        text(settings.preset);
        for (const auto &clip : new_model.clips) {
            text(clip.name);
            hash(clip.duration_seconds);
            for (const auto &track : clip.tracks) {
                hash(track.node);
                hash(track.position_interpolation);
                hash(track.scale_interpolation);
                hash(track.rotation_interpolation);
                for (const auto &key : track.positions) {
                    hash(key.time);
                    hash(key.value.x);
                    hash(key.value.y);
                    hash(key.value.z);
                    hash(key.in_tangent);
                    hash(key.out_tangent);
                }
                for (const auto &key : track.scales) {
                    hash(key.time);
                    hash(key.value.x);
                    hash(key.value.y);
                    hash(key.value.z);
                    hash(key.in_tangent);
                    hash(key.out_tangent);
                }
                for (const auto &key : track.rotations) {
                    hash(key.time);
                    for (const auto v : key.value)
                        hash(v);
                    hash(key.in_tangent);
                    hash(key.out_tangent);
                }
            }
            for (const auto &track : clip.morph_tracks) {
                hash(track.mesh);
                hash(track.interpolation);
                for (const auto &key : track.keys) {
                    hash(key.time);
                    for (const auto v : key.weights)
                        hash(v);
                    for (const auto v : key.in_tangent)
                        hash(v);
                    for (const auto v : key.out_tangent)
                        hash(v);
                }
            }
        }
        for (unsigned m = 0; m < imported->mNumMeshes; ++m) {
            const auto &mesh = *imported->mMeshes[m];
            hash(mesh.mNumBones);
            hash(mesh.mNumAnimMeshes);
            for (unsigned b = 0; b < mesh.mNumBones; ++b) {
                const auto &bone = *mesh.mBones[b];
                text(bone.mName.C_Str());
                hash(imported_matrix(bone.mOffsetMatrix));
                for (unsigned w = 0; w < bone.mNumWeights; ++w) {
                    hash(bone.mWeights[w].mVertexId);
                    hash(bone.mWeights[w].mWeight);
                }
            }
            for (unsigned t = 0; t < mesh.mNumAnimMeshes; ++t) {
                const auto &target = *mesh.mAnimMeshes[t];
                hash(target.mWeight);
                if (target.HasPositions())
                    dynamic_digest.update(target.mVertices,
                                          sizeof(aiVector3D) * target.mNumVertices);
                if (target.HasNormals())
                    dynamic_digest.update(target.mNormals,
                                          sizeof(aiVector3D) * target.mNumVertices);
                if (target.HasTangentsAndBitangents())
                    dynamic_digest.update(target.mTangents,
                                          sizeof(aiVector3D) * target.mNumVertices);
            }
        }
        for (unsigned i = 0; i < imported->mNumLights; ++i) {
            const auto &l = *imported->mLights[i];
            text(l.mName.C_Str());
            hash(l.mType);
            hash(l.mPosition);
            hash(l.mDirection);
            hash(l.mUp);
            hash(l.mColorDiffuse);
            hash(l.mAttenuationConstant);
            hash(l.mAttenuationLinear);
            hash(l.mAttenuationQuadratic);
            hash(l.mAngleInnerCone);
            hash(l.mAngleOuterCone);
        }
        result.content_id = std::string{asset_identity_prefix} + dynamic_digest.hex();
    }
    result.model = "asset." + result.content_id + ".model";
    new_model.name = result.model;
    if (settings.preset == "static_mesh")
        new_model.clips.clear();
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
    for (unsigned mesh_index = 0; mesh_index < imported->mNumMeshes; ++mesh_index) {
        const auto* source = imported->mMeshes[mesh_index];
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
            for (const auto value : {p.x, p.y, p.z, uv.x, uv.y, normal.x, normal.y, normal.z,
                                     tangent.x, tangent.y, tangent.z}) {
                if (!std::isfinite(value)) {
                    error = "nonfinite vertex attribute";
                    return result;
                }
            }
        }
        for (unsigned face = 0; face < source->mNumFaces; ++face) {
            if (source->mFaces[face].mNumIndices != 3U) {
                error = "model contains non-triangle primitives";
                return result;
            }
            for (unsigned index = 0; index < source->mFaces[face].mNumIndices; ++index) {
                if (source->mFaces[face].mIndices[index] >= source->mNumVertices) {
                    error = "mesh index outside vertex range";
                    return result;
                }
                new_indices.push_back(source->mFaces[face].mIndices[index]);
            }
        }
        const auto name = "asset." + result.content_id + ".mesh." + std::to_string(mesh_index);
        new_meshes.push_back({name, first_index,
                              static_cast<std::uint32_t>(new_indices.size()) - first_index,
                              vertex_offset});
        auto &mesh = new_meshes.back();
        mesh.vertex_count = source->mNumVertices;
        if (settings.preset == "scene" && source->HasBones()) {
            mesh.skin.resize(source->mNumVertices);
            for (unsigned b = 0; b < source->mNumBones; ++b) {
                const auto &bone = *source->mBones[b];
                const auto *node =
                    bone.mNode ? bone.mNode : imported->mRootNode->FindNode(bone.mName);
                const auto mapping = gltf_skin_nodes.find(mesh_index);
                if (mapping == gltf_skin_nodes.end() && !node) {
                    error = "skin targets missing bone node";
                    return result;
                }
                const auto offset = imported_matrix(bone.mOffsetMatrix);
                for (const auto v : offset)
                    if (!std::isfinite(v)) {
                        error = "invalid inverse bind matrix";
                        return result;
                    }
                mesh.joints.push_back(
                    {mapping != gltf_skin_nodes.end() ? mapping->second[b] : node_indices.at(node),
                     offset});
                if (bone.mNumWeights > source->mNumVertices) {
                    error = "excessive skin weights";
                    return result;
                }
                for (unsigned w = 0; w < bone.mNumWeights; ++w) {
                    const auto &weight = bone.mWeights[w];
                    if (weight.mVertexId >= mesh.skin.size() || !std::isfinite(weight.mWeight) ||
                        weight.mWeight < 0.0F) {
                        error = "invalid skin weight";
                        return result;
                    }
                    if (weight.mWeight == 0.0F)
                        continue;
                    auto &influences = mesh.skin[weight.mVertexId];
                    if (influences.size() >= 8U) {
                        error = "vertex exceeds eight skin influences";
                        return result;
                    }
                    influences.push_back({b, weight.mWeight});
                }
            }
            for (auto &influences : mesh.skin) {
                double sum = 0.0;
                for (const auto &w : influences)
                    sum += w.weight;
                if (sum > 0.0)
                    for (auto &w : influences)
                        w.weight = static_cast<float>(w.weight / sum);
            }
        }
        if (settings.preset == "scene")
            for (unsigned t = 0; t < source->mNumAnimMeshes; ++t) {
                const auto &target = *source->mAnimMeshes[t];
                if (target.mNumVertices != source->mNumVertices || !std::isfinite(target.mWeight) ||
                    std::abs(target.mWeight) > 100.0) {
                    error = "invalid morph target";
                    return result;
                }
                MorphTarget output{target.mName.C_Str(), target.mWeight, {}};
                output.deltas.resize(source->mNumVertices);
                for (unsigned v = 0; v < source->mNumVertices; ++v) {
                    auto &d = output.deltas[v];
                    d.nx = d.ny = d.nz = d.tx = d.ty = d.tz = d.tw = 0.0F;
                    const auto p = target.HasPositions()
                                       ? target.mVertices[v] - source->mVertices[v]
                                       : aiVector3D{};
                    const auto n = target.HasNormals() && source->HasNormals()
                                       ? target.mNormals[v] - source->mNormals[v]
                                       : aiVector3D{};
                    const auto tangent =
                        target.HasTangentsAndBitangents() && source->HasTangentsAndBitangents()
                            ? target.mTangents[v] - source->mTangents[v]
                            : aiVector3D{};
                    d.x = p.x;
                    d.y = p.y;
                    d.z = p.z;
                    d.nx = n.x;
                    d.ny = n.y;
                    d.nz = n.z;
                    d.tx = tangent.x;
                    d.ty = tangent.y;
                    d.tz = tangent.z;
                    for (const auto value : {d.x, d.y, d.z, d.nx, d.ny, d.nz, d.tx, d.ty, d.tz}) {
                        if (!std::isfinite(value)) {
                            error = "nonfinite morph target";
                            return result;
                        }
                    }
                }
                mesh.morph_targets.push_back(std::move(output));
            }
        result.meshes.push_back(name);
        Sha256 layout;
        for (const auto &joint : mesh.joints) {
            layout.update(&joint.node, sizeof(joint.node));
            layout.update(joint.inverse_bind.data(), sizeof(float) * 16U);
        }
        const auto target_count = mesh.morph_targets.size();
        layout.update(&target_count, sizeof(target_count));
        for (const auto &target : mesh.morph_targets)
            layout.update(target.name.data(), target.name.size());
        new_model.binding_layout.push_back("mesh." + std::to_string(mesh_index) + "." +
                                           layout.hex());
    }
    if (new_meshes.empty() && !imported->HasCameras() && !imported->HasLights() &&
        new_model.clips.empty()) {
        error = "model contains no triangle meshes";
        return result;
    }
    const bool already_registered = std::all_of(new_meshes.begin(), new_meshes.end(),
        [&](const auto& mesh) { return registry.find_mesh(mesh.name) != nullptr; });
    if (!already_registered &&
        registry.textures().size() + new_textures.size() > bindless_texture_capacity) {
        error = "model textures exceed the 16-slot texture table capacity";
        return result;
    }
    if (!new_meshes.empty() &&
        !registry.register_imported(std::move(new_vertices), std::move(new_indices),
                                    std::move(new_meshes), std::move(new_materials),
                                    std::move(new_textures))) {
        error = "model assets could not be registered";
        return result;
    }
    (void)registry.register_model(std::move(new_model));
    if (settings.preset == "scene") {
        for (unsigned index = 0; index < imported->mNumCameras; ++index) {
            const auto& camera = *imported->mCameras[index];
            if (!imported_camera(camera)) {
                result.warnings.push_back("camera '" + std::string{camera.mName.C_Str()} +
                                          "' has invalid projection or orientation");
            } else if (imported->mRootNode->FindNode(camera.mName) == nullptr) {
                result.warnings.push_back("camera '" + std::string{camera.mName.C_Str()} +
                                          "' has no matching scene node");
            }
        }
    }
    for (unsigned i = 0; i < imported->mNumLights; ++i) {
        const auto &light = *imported->mLights[i];
        if (light.mType != aiLightSource_DIRECTIONAL && light.mType != aiLightSource_POINT &&
            light.mType != aiLightSource_SPOT) {
            result.warnings.push_back("unsupported area or ambient light '" +
                                      std::string{light.mName.C_Str()} + "'");
        }
        const auto *node = imported->mRootNode->FindNode(light.mName);
        if (!node)
            result.warnings.push_back("light has no matching scene node: " +
                                      std::string{light.mName.C_Str()});
        Light check;
        check.color = {light.mColorDiffuse.r, light.mColorDiffuse.g, light.mColorDiffuse.b};
        check.attenuation = {light.mAttenuationConstant, light.mAttenuationLinear,
                             light.mAttenuationQuadratic};
        if (light.mType == aiLightSource_SPOT) {
            check.inner_cone = light.mAngleInnerCone;
            check.outer_cone = light.mAngleOuterCone;
        }
        Scene validator;
        const auto entity = validator.create();
        if (!validator.set_light(entity, check) || !std::isfinite(light.mPosition.x) ||
            !std::isfinite(light.mPosition.y) || !std::isfinite(light.mPosition.z))
            result.warnings.push_back("light has invalid color, attenuation, position or cone: " +
                                      std::string{light.mName.C_Str()});
    }
    if (scene != nullptr) {
        instantiate_node(imported->mRootNode, {}, *scene, result.meshes, result.materials,
                         *imported, result.roots, settings.preset == "scene", result.model,
                         node_indices);
    }
    result.imported = true;
    return result;
#endif
}

} // namespace relay
