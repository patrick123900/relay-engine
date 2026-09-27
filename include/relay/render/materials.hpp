#pragma once

#include "relay/render/assets.hpp"
#include "relay/render/shader_language.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Shader materials: .relay-material files whose type is "surface" or "post_process". Each names
// a .relay-shader of the same type and sets its uniforms; uniforms it leaves out keep the
// shader's defaults. (Sky materials, type "sky", are in sky.hpp.)
//
//     {"format": "relay.material", "version": 1, "type": "surface",
//      "shader": "shaders/Water.relay-shader",
//      "parameters": {"tint": [0.1, 0.4, 0.6], "speed": [2], "ripples": "textures/ripples.png"}}
//
// Values are arrays of numbers (booleans as 0 or 1); images are project-relative PNG or JPEG paths.
struct MaterialValue {
    std::vector<double> numbers;
    std::string texture;
    auto operator<=>(const MaterialValue&) const = default;
};

struct ShaderMaterial {
    ShaderType type{ShaderType::surface};
    std::string shader;
    std::map<std::string, MaterialValue> parameters;
    auto operator<=>(const ShaderMaterial&) const = default;
};

inline constexpr std::size_t maximum_material_parameters = 64U;

// Any .relay-material path (sky, surface or post-processing), with the characters project files use.
[[nodiscard]] bool valid_material_path(std::string_view path);
// The "type" of a material file: sky, surface or post_process.
[[nodiscard]] std::optional<std::string> read_material_type(const std::filesystem::path& root,
                                                            std::string_view path, std::string& error);
[[nodiscard]] std::string shader_material_json(const ShaderMaterial& material);
[[nodiscard]] std::optional<ShaderMaterial> parse_shader_material(std::string_view document,
                                                                  std::string& error);
[[nodiscard]] std::optional<ShaderMaterial> read_shader_material(const std::filesystem::path& root,
                                                                 std::string_view path,
                                                                 std::string& error);
[[nodiscard]] bool write_shader_material(const std::filesystem::path& root, std::string_view path,
                                         const ShaderMaterial& material, bool create_only,
                                         std::string& error);
// Checks a value against a uniform: the right number of numbers for values, a PNG or JPEG path
// (or empty) for images.
[[nodiscard]] bool valid_material_value(const ShaderUniform& uniform, const MaterialValue& value,
                                        std::string& error);

// A material ready to draw: its compiled shader, its std140 parameter block and its images.
struct ResolvedShaderMaterial {
    std::string path;
    ShaderMaterial material;
    std::shared_ptr<const CompiledShader> shader;
    std::vector<std::uint8_t> parameters;
    // One per sampler uniform, in binding order; null shows the hint's default image.
    std::vector<std::shared_ptr<const TextureAsset>> textures;
    // Why it cannot be drawn (the renderer then shows an error color), and lesser problems such as
    // an image that failed to load or a parameter the shader no longer declares.
    std::string error;
    std::vector<std::string> warnings;
    // Changes whenever the shader, parameters or images do.
    std::uint64_t revision{};
    [[nodiscard]] bool drawable() const { return error.empty() && shader && shader->ok(); }
};

// Packs values into the shader's std140 block: floats and bools as 32-bit floats and unsigned
// integers, ints as signed integers, at the offsets the shader parser computed.
[[nodiscard]] std::vector<std::uint8_t> pack_material_parameters(const ParsedShader& shader,
                                                                 const ShaderMaterial& material);

// A material's parameter block with per-object values in place of its own: numbers by uniform
// name, as MeshRenderer::parameters holds them. Values for uniforms the shader lacks, images, or
// with the wrong count are skipped (listed in `skipped` when given).
[[nodiscard]] std::vector<std::uint8_t> override_material_parameters(
    const ResolvedShaderMaterial& material, const std::map<std::string, std::vector<double>, std::less<>>& values,
    std::vector<std::string>* skipped = nullptr);

// Loads materials, their shaders (through a ShaderLibrary, so previews apply) and their images,
// reloading whatever changed on disk.
class MaterialLibrary {
public:
    [[nodiscard]] std::shared_ptr<const ResolvedShaderMaterial> resolve(const std::filesystem::path& root,
                                                                        std::string_view path,
                                                                        ShaderLibrary& shaders);
    void clear();

private:
    struct Stamp {
        std::filesystem::file_time_type time{};
        std::uintmax_t size{};
        bool exists{};
        auto operator<=>(const Stamp&) const = default;
    };
    struct Image {
        Stamp stamp;
        std::shared_ptr<const TextureAsset> texture;
        std::string error;
    };
    struct Entry {
        std::filesystem::path root;
        Stamp stamp;
        std::uint64_t shader_revision{};
        std::vector<const TextureAsset*> textures;
        std::shared_ptr<const ResolvedShaderMaterial> resolved;
    };
    [[nodiscard]] static Stamp stamp(const std::optional<std::filesystem::path>& path);
    const Image& image(const std::filesystem::path& root, const std::string& path, bool srgb);
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, Image, std::less<>> images_;
};

} // namespace relay
