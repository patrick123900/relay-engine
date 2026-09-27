#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Relay's shading language: GLSL functions with a few declarations Relay understands, in a project
// file ending in .relay-shader. A file starts with its type,
//
//     shader_type surface;          // how a mesh's surface looks, lit like every other surface
//     shader_type post_process;     // a full-screen effect on the lit scene, before tone mapping
//
// optionally lists render modes (surface shaders: `render_mode transparent, unshaded;`), declares
// parameters that materials set,
//
//     uniform vec3 tint : source_color = vec3(1.0, 0.5, 0.2);
//     uniform float speed : hint_range(0.0, 10.0) = 1.0;
//     uniform sampler2D albedo_map : source_color;
//
// and defines `void vertex()` and/or `void fragment()`, which read and write built-in variables
// such as VERTEX, ALBEDO, ROUGHNESS or COLOR (see docs/shaders.md). Relay wraps them in complete
// GLSL with its lighting, shadows, fog and G-buffer, so custom surfaces stay lit and take part in
// global illumination.

struct EmbeddedShaderSource {
    std::string_view name;
    std::string_view text;
};
// Shader include files built into Relay (scene_bindings.glsl, surface_lighting.glsl).
[[nodiscard]] const std::vector<EmbeddedShaderSource>& embedded_shader_sources();

enum class ShaderType : std::uint8_t { surface, post_process };
[[nodiscard]] std::string_view shader_type_name(ShaderType type);
[[nodiscard]] std::optional<ShaderType> shader_type_from_name(std::string_view name);

struct ShaderUniform {
    enum class Type : std::uint8_t { float_value, vec2, vec3, vec4, int_value, bool_value, sampler2d };
    // color: vectors edited as colors, and sampled images read as sRGB. range: a slider between
    // minimum and maximum. normal/black/white: the image a sampler shows until one is chosen.
    enum class Hint : std::uint8_t { none, color, range, normal, black, white };
    std::string name;
    Type type{Type::float_value};
    Hint hint{Hint::none};
    std::array<double, 4> default_value{};
    double minimum{0.0};
    double maximum{1.0};
    double step{0.0};
    // Byte offset in the std140 parameter block, or for samplers the binding in set 1.
    std::uint32_t offset{};
    std::uint32_t line{};
};
[[nodiscard]] std::string_view shader_uniform_type_name(ShaderUniform::Type type);
[[nodiscard]] std::optional<ShaderUniform::Type> shader_uniform_type_from_name(std::string_view name);
[[nodiscard]] std::uint32_t shader_uniform_components(ShaderUniform::Type type);
[[nodiscard]] std::string_view shader_uniform_hint_name(ShaderUniform::Hint hint);

// A problem at a line of the user's file (1-based); line 0 concerns the whole file.
struct ShaderMessage {
    std::uint32_t line{};
    std::string text;
    auto operator<=>(const ShaderMessage&) const = default;
};

struct ShaderFunction {
    std::string name;
    std::size_t begin{};
    std::size_t end{}; // One past the closing brace.
    std::uint32_t line{};
};

struct ParsedShader {
    ShaderType type{ShaderType::surface};
    bool transparent{};
    bool unshaded{};
    std::vector<ShaderUniform> uniforms;
    // Size of the std140 parameter block, a multiple of 16 bytes (at least 16).
    std::uint32_t uniform_bytes{16U};
    std::uint32_t texture_count{};
    // The file with Relay's declarations blanked out, keeping every line where it was.
    std::string code;
    std::vector<ShaderFunction> functions; // Top-level function definitions.
    bool has_vertex{};
    bool has_fragment{};
    // Post processing: whether the code reads blurred scene colors (scene_color_lod), for which
    // the renderer first builds the scene image's smaller copies.
    bool reads_blurred{};
    std::vector<ShaderMessage> errors;
    [[nodiscard]] const ShaderUniform* find_uniform(std::string_view name) const;
};

inline constexpr std::size_t maximum_shader_bytes = 256U * 1024U;
inline constexpr std::size_t maximum_shader_uniforms = 64U;
inline constexpr std::uint32_t maximum_shader_textures = 8U;
inline constexpr std::uint32_t maximum_shader_uniform_bytes = 4096U;

[[nodiscard]] ParsedShader parse_relay_shader(std::string_view text);

// GLSL for each stage. Surface shaders get all three; post-processing shaders only `fragment`
// (they run over Relay's full-screen triangle). The user's code follows `#line 1`; Relay's own
// code sits at lines numbered from 100000, which compile errors report as whole-file problems.
struct GeneratedShader {
    std::string vertex;
    std::string fragment;
    std::string shadow_fragment;
};
[[nodiscard]] GeneratedShader generate_shader_glsl(const ParsedShader& shader);

struct CompiledShader {
    std::string path;
    ParsedShader parsed;
    std::vector<std::uint32_t> vertex, fragment, shadow_fragment;
    // Parse and compile errors, sorted by line and without duplicates.
    std::vector<ShaderMessage> errors;
    // Distinct for every compile, so renderers know when to rebuild pipelines.
    std::uint64_t revision{};
    [[nodiscard]] bool ok() const { return errors.empty() && !fragment.empty(); }
};

// False when Relay was built without glslang; every compile then reports that.
[[nodiscard]] bool runtime_shader_compiler_available();
[[nodiscard]] std::shared_ptr<const CompiledShader> compile_relay_shader(std::string_view path,
                                                                         std::string_view text);

// A project-relative path ending in .relay-shader, with the characters project files may use.
[[nodiscard]] bool valid_shader_path(std::string_view path);
// The starting text of a new shader of this type.
[[nodiscard]] std::string shader_template(ShaderType type);

// Ready-made post-processing effects (shaders/effects/ in the repository), copied into a project
// as ordinary shaders that people and agents can then change.
struct ShaderEffect {
    std::string_view id;          // bloom, color_grading, vignette
    std::string_view name;        // Bloom, Color Grading...: the file names it is copied to
    std::string_view description;
    std::string_view text;
};
[[nodiscard]] const std::vector<ShaderEffect>& shader_effects();
[[nodiscard]] const ShaderEffect* find_shader_effect(std::string_view id);

// Reads, writes and compiles project shaders, caching each compile until its file changes. A
// preview replaces a file's text in memory, so an editor can show changes before saving them.
class ShaderLibrary {
public:
    [[nodiscard]] std::shared_ptr<const CompiledShader> resolve(const std::filesystem::path& root,
                                                                std::string_view path);
    void set_preview(std::string_view path, std::optional<std::string> text);
    [[nodiscard]] const std::string* preview(std::string_view path) const;
    [[nodiscard]] std::vector<std::string> previews() const;
    void clear_previews() { previews_.clear(); }

private:
    struct Stamp {
        std::filesystem::file_time_type time{};
        std::uintmax_t size{};
        bool exists{};
        auto operator<=>(const Stamp&) const = default;
    };
    struct Entry {
        std::filesystem::path root;
        Stamp stamp;
        std::optional<std::string> previewed;
        std::shared_ptr<const CompiledShader> compiled;
    };
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, std::string, std::less<>> previews_;
};

[[nodiscard]] std::optional<std::string> read_shader_file(const std::filesystem::path& root,
                                                          std::string_view path, std::string& error);
[[nodiscard]] bool write_shader_file(const std::filesystem::path& root, std::string_view path,
                                     std::string_view text, bool create_only, std::string& error);

} // namespace relay
