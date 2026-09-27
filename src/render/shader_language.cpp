#include "relay/render/shader_language.hpp"

#include "relay/scene/project.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

#ifdef RELAY_HAS_GLSLANG
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#endif

namespace relay {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view shader_suffix = ".relay-shader";
// Relay's own code is numbered from here, so its lines never collide with the user's.
constexpr std::uint32_t generated_line = 100000U;

std::uint64_t next_shader_revision() {
    static std::atomic<std::uint64_t> revision{0};
    return ++revision;
}

bool identifier_start(const char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool identifier_char(const char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The text with comments replaced by spaces (newlines kept), so scanning ignores them.
std::string mask_comments(const std::string_view text) {
    std::string masked{text};
    for (std::size_t index = 0; index < masked.size(); ++index) {
        if (masked.compare(index, 2, "//") == 0) {
            while (index < masked.size() && masked[index] != '\n') masked[index++] = ' ';
        } else if (masked.compare(index, 2, "/*") == 0) {
            const auto end = masked.find("*/", index + 2U);
            const auto stop = end == std::string::npos ? masked.size() : end + 2U;
            for (; index < stop; ++index)
                if (masked[index] != '\n') masked[index] = ' ';
            --index;
        }
    }
    return masked;
}

std::uint32_t line_at(const std::string_view text, const std::size_t offset) {
    return 1U + static_cast<std::uint32_t>(
                    std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(
                                                                 std::min(offset, text.size())),
                               '\n'));
}

void blank(std::string& text, const std::size_t begin, const std::size_t end) {
    for (std::size_t index = begin; index < end && index < text.size(); ++index)
        if (text[index] != '\n') text[index] = ' ';
}

// Splits a declaration into identifiers, numbers and single punctuation characters.
std::vector<std::string> tokens(const std::string_view text) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < text.size();) {
        const char c = text[index];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            ++index;
        } else if (identifier_start(c)) {
            const auto start = index;
            while (index < text.size() && identifier_char(text[index])) ++index;
            result.emplace_back(text.substr(start, index - start));
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0 || c == '.') {
            const auto start = index;
            while (index < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[index])) != 0 || text[index] == '.' ||
                    ((text[index] == '-' || text[index] == '+') && index > start &&
                     (text[index - 1U] == 'e' || text[index - 1U] == 'E'))))
                ++index;
            result.emplace_back(text.substr(start, index - start));
        } else {
            result.emplace_back(1U, c);
            ++index;
        }
    }
    return result;
}

std::optional<double> parse_number(std::string token) {
    if (!token.empty() && (token.back() == 'f' || token.back() == 'F')) token.pop_back();
    if (token.empty()) return std::nullopt;
    double value{};
    const auto* end = token.data() + token.size();
    const auto [pointer, error] = std::from_chars(token.data(), end, value);
    if (error != std::errc{} || pointer != end || !std::isfinite(value)) return std::nullopt;
    return value;
}

std::uint32_t std140_alignment(const ShaderUniform::Type type) {
    switch (type) {
    case ShaderUniform::Type::vec2: return 8U;
    case ShaderUniform::Type::vec3:
    case ShaderUniform::Type::vec4: return 16U;
    default: return 4U;
    }
}

std::string glsl_uniform_type(const ShaderUniform::Type type) {
    return std::string{shader_uniform_type_name(type)};
}

// Parses `uniform <type> <name> [: hint] [= default]` (without the trailing semicolon).
std::optional<ShaderUniform> parse_uniform(const std::vector<std::string>& words, std::string& error) {
    if (words.size() < 3U) {
        error = "a uniform needs a type and a name";
        return std::nullopt;
    }
    ShaderUniform uniform;
    const auto type = shader_uniform_type_from_name(words[1]);
    if (!type) {
        error = "uniforms can be float, vec2, vec3, vec4, int, bool or sampler2D, not " + words[1];
        return std::nullopt;
    }
    uniform.type = *type;
    if (!identifier_start(words[2][0]) || words[2].starts_with("gl_") ||
        words[2].starts_with("relay_") ||
        std::all_of(words[2].begin(), words[2].end(),
                    [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0 || c == '_' ||
                                        std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        error = "'" + words[2] + "' is reserved; uniform names are lower case and cannot start "
                "with gl_ or relay_";
        return std::nullopt;
    }
    uniform.name = words[2];
    const auto components = shader_uniform_components(uniform.type);
    if (uniform.type == ShaderUniform::Type::float_value) uniform.default_value = {0, 0, 0, 0};
    std::size_t at = 3U;
    if (at < words.size() && words[at] == ":") {
        ++at;
        if (at >= words.size()) {
            error = "expected a hint after ':'";
            return std::nullopt;
        }
        const auto& hint = words[at++];
        const bool vector = uniform.type == ShaderUniform::Type::vec3 || uniform.type == ShaderUniform::Type::vec4;
        const bool sampler = uniform.type == ShaderUniform::Type::sampler2d;
        const bool scalar = uniform.type == ShaderUniform::Type::float_value ||
                            uniform.type == ShaderUniform::Type::int_value;
        if (hint == "source_color" && (vector || sampler)) {
            uniform.hint = ShaderUniform::Hint::color;
        } else if (hint == "hint_normal" && sampler) {
            uniform.hint = ShaderUniform::Hint::normal;
        } else if (hint == "hint_black" && sampler) {
            uniform.hint = ShaderUniform::Hint::black;
        } else if (hint == "hint_white" && sampler) {
            uniform.hint = ShaderUniform::Hint::white;
        } else if (hint == "hint_range" && scalar) {
            uniform.hint = ShaderUniform::Hint::range;
            std::vector<double> values;
            if (at >= words.size() || words[at] != "(") {
                error = "hint_range needs (minimum, maximum[, step])";
                return std::nullopt;
            }
            ++at;
            while (at < words.size() && words[at] != ")") {
                double sign = 1.0;
                if (words[at] == "-") {
                    sign = -1.0;
                    ++at;
                }
                const auto value = at < words.size() ? parse_number(words[at]) : std::nullopt;
                if (!value) {
                    error = "hint_range values must be numbers";
                    return std::nullopt;
                }
                values.push_back(sign * *value);
                ++at;
                if (at < words.size() && words[at] == ",") ++at;
            }
            if (at >= words.size() || (values.size() != 2U && values.size() != 3U) ||
                values[1] <= values[0]) {
                error = "hint_range needs (minimum, maximum[, step]) with maximum above minimum";
                return std::nullopt;
            }
            ++at;
            uniform.minimum = values[0];
            uniform.maximum = values[1];
            uniform.step = values.size() == 3U ? values[2] : 0.0;
        } else {
            error = "'" + hint + "' is not a hint for a " + words[1] +
                    " (use source_color, hint_range, hint_normal, hint_black or hint_white)";
            return std::nullopt;
        }
    }
    if (uniform.type == ShaderUniform::Type::sampler2d) {
        if (at < words.size()) {
            error = "sampler2D uniforms take no default value; choose the image in the material";
            return std::nullopt;
        }
        return uniform;
    }
    if (uniform.hint == ShaderUniform::Hint::none && uniform.type == ShaderUniform::Type::vec4)
        uniform.default_value = {0, 0, 0, 0};
    if (uniform.hint == ShaderUniform::Hint::color)
        uniform.default_value = {1, 1, 1, 1};
    if (at < words.size()) {
        if (words[at] != "=") {
            error = "expected '=' or ';' after the uniform, found '" + words[at] + "'";
            return std::nullopt;
        }
        ++at;
        std::vector<double> values;
        const auto read_values = [&](std::size_t from, const std::size_t to) {
            while (from < to) {
                double sign = 1.0;
                if (words[from] == "-") {
                    sign = -1.0;
                    ++from;
                }
                if (from >= to) return false;
                if (words[from] == "true" || words[from] == "false") values.push_back(words[from] == "true");
                else if (const auto value = parse_number(words[from])) values.push_back(sign * *value);
                else return false;
                ++from;
                if (from < to && words[from] == ",") ++from;
            }
            return true;
        };
        bool ok = false;
        if (at < words.size() && words[at] == glsl_uniform_type(uniform.type) && at + 1U < words.size() &&
            words[at + 1U] == "(" && words.back() == ")") {
            ok = read_values(at + 2U, words.size() - 1U);
        } else {
            ok = read_values(at, words.size());
        }
        if (!ok || values.empty() || (values.size() != 1U && values.size() != components)) {
            error = "the default value of " + uniform.name + " must be a " + words[1] +
                    " made of numbers, such as " +
                    (components == 1U ? std::string{"1.0"}
                                      : words[1] + "(" + std::string(components == 2U ? "1.0, 0.5" :
                                                                     components == 3U ? "1.0, 0.5, 0.2"
                                                                                      : "1.0, 0.5, 0.2, 1.0") + ")");
            return std::nullopt;
        }
        for (std::size_t index = 0; index < components; ++index)
            uniform.default_value[index] = values.size() == 1U ? values[0] : values[index];
    }
    if (uniform.type == ShaderUniform::Type::int_value) uniform.default_value[0] = std::round(uniform.default_value[0]);
    if (uniform.type == ShaderUniform::Type::bool_value)
        uniform.default_value[0] = uniform.default_value[0] != 0.0 ? 1.0 : 0.0;
    return uniform;
}

std::string expand_includes(const std::string_view text, int depth = 0) {
    std::string result;
    std::istringstream lines{std::string{text}};
    std::string line;
    while (std::getline(lines, line)) {
        const auto start = line.find_first_not_of(" \t");
        if (depth < 4 && start != std::string::npos && line.compare(start, 8, "#include") == 0) {
            const auto open = line.find('"');
            const auto close = open == std::string::npos ? open : line.find('"', open + 1U);
            if (close != std::string::npos) {
                const auto name = line.substr(open + 1U, close - open - 1U);
                for (const auto& source : embedded_shader_sources())
                    if (source.name == name) {
                        result += expand_includes(source.text, depth + 1);
                        result += '\n';
                        break;
                    }
                continue;
            }
        }
        if (start != std::string::npos && line.compare(start, 10, "#extension") == 0 &&
            line.find("GL_GOOGLE_include_directive") != std::string::npos)
            continue;
        result += line;
        result += '\n';
    }
    return result;
}

std::string embedded(const std::string_view name) {
    for (const auto& source : embedded_shader_sources())
        if (source.name == name) return expand_includes(source.text);
    return {};
}

// The user's code with one of its stage functions removed, for the other stage.
std::string stage_code(const ParsedShader& shader, const std::string_view drop) {
    auto code = shader.code;
    for (const auto& function : shader.functions)
        if (function.name == drop) blank(code, function.begin, function.end);
    return code;
}

std::string parameter_declarations(const ParsedShader& shader) {
    std::string text = "layout(std140, set = 1, binding = 0) uniform RelayParameters {\n";
    bool any = false;
    for (const auto& uniform : shader.uniforms)
        if (uniform.type != ShaderUniform::Type::sampler2d) {
            text += "    " + glsl_uniform_type(uniform.type) + ' ' + uniform.name + ";\n";
            any = true;
        }
    if (!any) text += "    vec4 relay_unused_parameter;\n";
    text += "};\n";
    for (const auto& uniform : shader.uniforms)
        if (uniform.type == ShaderUniform::Type::sampler2d)
            text += "layout(set = 1, binding = " + std::to_string(uniform.offset) +
                    ") uniform sampler2D " + uniform.name + ";\n";
    return text;
}

constexpr std::string_view surface_varyings_out = R"(layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) out vec3 surface_normal;
layout(location = 2) out vec4 surface_tangent;
layout(location = 3) flat out uint material_index;
layout(location = 4) out vec3 world_position;
layout(location = 5) out vec4 current_clip;
layout(location = 6) out vec4 previous_clip;
)";

constexpr std::string_view surface_varyings_in = R"(layout(location = 0) in vec2 texture_coordinates;
layout(location = 1) in vec3 surface_normal;
layout(location = 2) in vec4 surface_tangent;
layout(location = 3) flat in uint material_index;
layout(location = 4) in vec3 world_position;
layout(location = 5) in vec4 current_clip;
layout(location = 6) in vec4 previous_clip;
)";

constexpr std::string_view push_constants = R"(layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;
)";

// Every stage declares every built-in, so helper functions shared by vertex() and fragment()
// compile in both.
constexpr std::string_view surface_builtins = R"(vec3 VERTEX;
vec3 NORMAL;
vec4 TANGENT;
vec2 UV;
float TIME;
mat4 MODEL_MATRIX;
vec3 WORLD_POSITION;
vec3 VIEW;
vec3 CAMERA_POSITION;
vec4 FRAGCOORD;
bool FRONT_FACING;
vec3 ALBEDO;
float ALPHA;
float METALLIC;
float ROUGHNESS;
vec3 EMISSION;
float AO;
vec3 NORMAL_MAP;
float NORMAL_MAP_DEPTH;
float ALPHA_SCISSOR_THRESHOLD;
)";

std::string surface_vertex(const ParsedShader& shader) {
    std::string text = "#version 450\n#line 200000\n";
    text += "layout(constant_id = 1) const bool shadow_pass = false;\n";
    text += push_constants;
    text += R"(struct DrawData {
    mat4 previous_model_view_projection;
    uvec4 material;
};
layout(std430, set = 0, binding = 5) readonly buffer DrawBuffer {
    DrawData draws[];
};
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 tangent;
)";
    text += surface_varyings_out;
    text += embedded("scene_bindings.glsl");
    text += parameter_declarations(shader);
    text += surface_builtins;
    text += "#line 1\n" + stage_code(shader, "fragment") + '\n';
    text += "#line " + std::to_string(generated_line) + "\nvoid main() {\n";
    text += R"(    VERTEX = position;
    NORMAL = normal;
    TANGENT = tangent;
    UV = uv;
    TIME = lighting.frame_time.x;
    MODEL_MATRIX = frame.model;
)";
    if (shader.has_vertex) text += "    vertex();\n";
    text += R"(    gl_Position = frame.model_view_projection * vec4(VERTEX, 1.0);
    current_clip = gl_Position;
    // Shadow passes draw with other instance numbering and need no motion.
    previous_clip = shadow_pass ? gl_Position
                                : draws[gl_InstanceIndex].previous_model_view_projection * vec4(VERTEX, 1.0);
    world_position = (frame.model * vec4(VERTEX, 1.0)).xyz;
    texture_coordinates = UV;
    mat3 normal_matrix = transpose(inverse(mat3(frame.model)));
    surface_normal = normalize(normal_matrix * NORMAL);
    surface_tangent = vec4(normalize(mat3(frame.model) * TANGENT.xyz), TANGENT.w);
    material_index = 0u;
}
)";
    return text;
}

std::string surface_fragment(const ParsedShader& shader, const bool shadow) {
    std::string text = "#version 450\n#line 200000\n";
    text += surface_varyings_in;
    if (!shadow) {
        text += R"(layout(location = 0) out vec4 output_color;
layout(location = 1) out vec4 output_normal_roughness;
layout(location = 2) out vec4 output_albedo_metallic;
layout(location = 3) out vec4 output_motion_occlusion;
layout(location = 4) out float output_depth;
layout(location = 5) out vec4 output_diffuse_light;
)";
    }
    text += "layout(constant_id = 0) const bool geometry_pass = false;\n";
    text += "#define SURFACE_TEXTURE(index, uv) texture(textures[index], uv)\n";
    text += embedded("surface_lighting.glsl");
    text += push_constants;
    text += parameter_declarations(shader);
    text += surface_builtins;
    text += "#line 1\n" + stage_code(shader, "vertex") + '\n';
    text += "#line " + std::to_string(generated_line) + "\nvoid main() {\n";
    text += R"(    UV = texture_coordinates;
    NORMAL = normalize(surface_normal);
    // Custom surfaces are drawn from both sides; the back of a face turns its normal round.
    if (!gl_FrontFacing) NORMAL = -NORMAL;
    TANGENT = surface_tangent;
    WORLD_POSITION = world_position;
    CAMERA_POSITION = lighting.camera_count.xyz;
    VIEW = normalize(CAMERA_POSITION - world_position);
    TIME = lighting.frame_time.x;
    FRAGCOORD = gl_FragCoord;
    FRONT_FACING = gl_FrontFacing;
    MODEL_MATRIX = frame.model;
    ALBEDO = vec3(1.0);
    ALPHA = 1.0;
    METALLIC = 0.0;
    ROUGHNESS = 0.5;
    EMISSION = vec3(0.0);
    AO = 1.0;
    NORMAL_MAP = vec3(0.5, 0.5, 1.0);
    NORMAL_MAP_DEPTH = 1.0;
    ALPHA_SCISSOR_THRESHOLD = 0.0;
)";
    if (shader.has_fragment) text += "    fragment();\n";
    text += "    if (ALPHA_SCISSOR_THRESHOLD > 0.0 && ALPHA < ALPHA_SCISSOR_THRESHOLD) discard;\n";
    if (shadow) return text + "}\n";
    text += R"(    Surface surface;
    surface.base_color = vec4(max(ALBEDO, vec3(0.0)), clamp(ALPHA, 0.0, 1.0));
    surface.metallic = clamp(METALLIC, 0.0, 1.0);
    surface.roughness = clamp(ROUGHNESS, 0.045, 1.0);
    vec3 normal = normalize(NORMAL);
    if (NORMAL_MAP != vec3(0.5, 0.5, 1.0)) {
        vec3 tangent = normalize(TANGENT.xyz - normal * dot(normal, TANGENT.xyz));
        vec3 bitangent = normalize(cross(normal, tangent)) * (TANGENT.w < 0.0 ? -1.0 : 1.0);
        vec3 mapped = NORMAL_MAP * 2.0 - 1.0;
        mapped.xy *= NORMAL_MAP_DEPTH;
        normal = normalize(mat3(tangent, bitangent, normal) * mapped);
    }
    surface.normal = normal;
    surface.occlusion = clamp(AO, 0.0, 1.0);
    surface.emissive = max(EMISSION, vec3(0.0));
    vec3 view_direction = VIEW;
    vec3 color;
    vec3 direct_diffuse;
)";
    if (shader.unshaded) {
        text += R"(    // Unshaded surfaces show their color as it is; black, fully metallic G-buffer values keep
    // global illumination and reflections from adding to it.
    color = surface.base_color.rgb + surface.emissive;
    direct_diffuse = color;
    vec4 albedo_metallic = vec4(0.0, 0.0, 0.0, 1.0);
)";
    } else {
        text += R"(    vec3 direct_color;
    direct_lighting(surface, world_position, view_direction, direct_color, direct_diffuse);
    uint indirect = geometry_pass ? uint(lighting.camera_forward.w) : 0u;
    vec3 ambient = (indirect & 1u) != 0u ? vec3(0.0)
                 : (indirect & 2u) != 0u ? ambient_diffuse(surface) * surface.occlusion
                                         : ambient_lighting(surface, view_direction);
    color = ambient + direct_color + surface.emissive;
    direct_diffuse += surface.emissive;
    vec4 albedo_metallic = vec4(surface.base_color.rgb, surface.metallic);
)";
    }
    text += R"(    if (!geometry_pass) {
        float fog = fog_amount(length(lighting.camera_count.xyz - world_position));
        color = mix(color, fog_color(fog), fog);
    }
    output_color = vec4(color, surface.base_color.a);
    vec2 current_uv = current_clip.xy / current_clip.w * 0.5 + 0.5;
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
    output_normal_roughness = vec4(surface.normal, surface.roughness);
    output_albedo_metallic = albedo_metallic;
    output_motion_occlusion = vec4(previous_uv - current_uv, surface.occlusion, 0.0);
    output_depth = gl_FragCoord.z;
    output_diffuse_light = vec4(direct_diffuse, 1.0);
}
)";
    return text;
}

std::string post_fragment(const ParsedShader& shader) {
    std::string text = "#version 450\n#line 200000\n";
    text += R"(layout(location = 0) out vec4 output_color;
layout(set = 0, binding = 0) uniform sampler2D SCREEN_TEXTURE;
layout(set = 0, binding = 1) uniform sampler2D relay_depth;
layout(set = 0, binding = 2) uniform sampler2D relay_normal_roughness;
layout(set = 0, binding = 3) uniform sampler2D relay_motion;
layout(std140, set = 0, binding = 4) uniform RelayPostCamera {
    mat4 inverse_view_projection;
    mat4 previous_view_projection;
} relay_camera;
layout(push_constant) uniform RelayPostData {
    mat4 inverse_projection;
    vec4 extent_time;
} relay_post;
)";
    text += parameter_declarations(shader);
    text += R"(vec2 SCREEN_UV;
vec2 SCREEN_PIXEL_SIZE;
vec3 COLOR;
float DEPTH;
vec3 NORMAL;
vec2 MOTION;
float TIME;
float DELTA_TIME;
vec4 FRAGCOORD;
// The lit scene's color at a screen position (0 to 1).
vec3 scene_color(vec2 uv) { return textureLod(SCREEN_TEXTURE, uv, 0.0).rgb; }
// The scene's color blurred: level 0 is sharp, and each level up averages twice as far (a copy at
// half the size), up to about 7. Fractions blend between levels.
vec3 scene_color_lod(vec2 uv, float lod) { return textureLod(SCREEN_TEXTURE, uv, max(lod, 0.0)).rgb; }
// Distance from the camera plane to the surface at a screen position, in metres.
float scene_depth(vec2 uv) {
    float z = texture(relay_depth, uv).r;
    vec4 view = relay_post.inverse_projection * vec4(uv * 2.0 - 1.0, z, 1.0);
    return -view.z / view.w;
}
// The world-space normal of the surface at a screen position; zero where there is only sky.
vec3 scene_normal(vec2 uv) { return texture(relay_normal_roughness, uv).xyz; }
// How far what is seen at a screen position moved on screen since the last frame, in screen units
// (where it was minus where it is): surfaces from their motion vectors, the sky from how the
// camera turned.
vec2 scene_motion(vec2 uv) {
    if (texture(relay_depth, uv).r < 1.0) return texture(relay_motion, uv).xy;
    vec4 far_point = relay_camera.inverse_view_projection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 previous = relay_camera.previous_view_projection * vec4(far_point.xyz / far_point.w, 1.0);
    return previous.xy / previous.w * 0.5 + 0.5 - uv;
}
)";
    text += "#line 1\n" + shader.code + '\n';
    text += "#line " + std::to_string(generated_line) + "\nvoid main() {\n";
    text += R"(    SCREEN_PIXEL_SIZE = 1.0 / relay_post.extent_time.xy;
    SCREEN_UV = gl_FragCoord.xy * SCREEN_PIXEL_SIZE;
    FRAGCOORD = gl_FragCoord;
    TIME = relay_post.extent_time.z;
    DELTA_TIME = relay_post.extent_time.w;
    COLOR = scene_color(SCREEN_UV);
    MOTION = scene_motion(SCREEN_UV);
    DEPTH = scene_depth(SCREEN_UV);
    NORMAL = scene_normal(SCREEN_UV);
)";
    if (shader.has_fragment) text += "    fragment();\n";
    text += "    output_color = vec4(max(COLOR, vec3(0.0)), 1.0);\n}\n";
    return text;
}

#ifdef RELAY_HAS_GLSLANG
void initialize_glslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

// Compiles one stage; glslang's messages go into `messages` with the user's line numbers.
bool compile_stage(const EShLanguage stage, const std::string& source,
                   std::vector<std::uint32_t>& spirv, std::vector<ShaderMessage>& messages) {
    initialize_glslang();
    glslang::TShader shader(stage);
    const char* strings[] = {source.c_str()};
    shader.setStrings(strings, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const auto collect = [&](const char* log) {
        std::istringstream lines{log ? log : ""};
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.starts_with("ERROR: ")) continue;
            auto rest = line.substr(7U);
            // "0:12: 'x' : undeclared identifier"
            const auto first = rest.find(':');
            const auto second = first == std::string::npos ? first : rest.find(':', first + 1U);
            std::uint32_t number = 0U;
            std::string text = rest;
            if (second != std::string::npos) {
                const auto digits = rest.substr(first + 1U, second - first - 1U);
                if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) {
                        return std::isdigit(static_cast<unsigned char>(c)) != 0;
                    })) {
                    number = static_cast<std::uint32_t>(std::stoul(digits));
                    text = rest.substr(second + 1U);
                }
            } else if (rest.find("compilation errors") != std::string::npos ||
                       rest.find("No code generated") != std::string::npos) {
                continue;
            }
            while (!text.empty() && text.front() == ' ') text.erase(text.begin());
            // glslang follows real errors with this note, which says nothing new.
            if (text.find("compilation terminated") != std::string::npos) continue;
            if (number >= generated_line) {
                const bool calls = text.find("vertex") != std::string::npos ||
                                   text.find("fragment") != std::string::npos;
                text = calls ? "vertex() and fragment() must be declared as void functions without "
                               "parameters (" + text + ")"
                             : "Relay's generated code could not compile around this shader: " + text;
                number = 0U;
            }
            messages.push_back({number, text});
        }
    };
    const auto resources = GetDefaultResources();
    if (!shader.parse(resources, 450, false, EShMsgDefault)) {
        collect(shader.getInfoLog());
        if (messages.empty()) messages.push_back({0U, "the shader could not be compiled"});
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        collect(program.getInfoLog());
        if (messages.empty()) messages.push_back({0U, "the shader could not be linked"});
        return false;
    }
    std::vector<unsigned int> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    options.validate = false;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    spirv.assign(words.begin(), words.end());
    return !spirv.empty();
}
#endif

} // namespace

std::string_view shader_type_name(const ShaderType type) {
    return type == ShaderType::post_process ? "post_process" : "surface";
}

std::optional<ShaderType> shader_type_from_name(const std::string_view name) {
    if (name == "surface") return ShaderType::surface;
    if (name == "post_process") return ShaderType::post_process;
    return std::nullopt;
}

std::string_view shader_uniform_type_name(const ShaderUniform::Type type) {
    switch (type) {
    case ShaderUniform::Type::float_value: return "float";
    case ShaderUniform::Type::vec2: return "vec2";
    case ShaderUniform::Type::vec3: return "vec3";
    case ShaderUniform::Type::vec4: return "vec4";
    case ShaderUniform::Type::int_value: return "int";
    case ShaderUniform::Type::bool_value: return "bool";
    case ShaderUniform::Type::sampler2d: return "sampler2D";
    }
    return "float";
}

std::optional<ShaderUniform::Type> shader_uniform_type_from_name(const std::string_view name) {
    for (const auto type : {ShaderUniform::Type::float_value, ShaderUniform::Type::vec2,
                            ShaderUniform::Type::vec3, ShaderUniform::Type::vec4,
                            ShaderUniform::Type::int_value, ShaderUniform::Type::bool_value,
                            ShaderUniform::Type::sampler2d})
        if (shader_uniform_type_name(type) == name) return type;
    return std::nullopt;
}

std::uint32_t shader_uniform_components(const ShaderUniform::Type type) {
    switch (type) {
    case ShaderUniform::Type::vec2: return 2U;
    case ShaderUniform::Type::vec3: return 3U;
    case ShaderUniform::Type::vec4: return 4U;
    case ShaderUniform::Type::sampler2d: return 0U;
    default: return 1U;
    }
}

std::string_view shader_uniform_hint_name(const ShaderUniform::Hint hint) {
    switch (hint) {
    case ShaderUniform::Hint::none: return "none";
    case ShaderUniform::Hint::color: return "source_color";
    case ShaderUniform::Hint::range: return "hint_range";
    case ShaderUniform::Hint::normal: return "hint_normal";
    case ShaderUniform::Hint::black: return "hint_black";
    case ShaderUniform::Hint::white: return "hint_white";
    }
    return "none";
}

const ShaderUniform* ParsedShader::find_uniform(const std::string_view name) const {
    const auto found = std::find_if(uniforms.begin(), uniforms.end(),
                                    [name](const ShaderUniform& uniform) { return uniform.name == name; });
    return found == uniforms.end() ? nullptr : &*found;
}

ParsedShader parse_relay_shader(const std::string_view text) {
    ParsedShader result;
    result.code = std::string{text};
    if (text.size() > maximum_shader_bytes) {
        result.errors.push_back({0U, "shaders may be at most 256 KiB"});
        return result;
    }
    const auto masked = mask_comments(text);
    bool typed = false;
    std::set<std::string> names;
    std::uint32_t offset = 0U;
    int depth = 0;
    std::size_t statement = 0U; // Start of the current top-level statement.
    const auto error = [&](const std::size_t at, std::string message) {
        result.errors.push_back({line_at(text, at), std::move(message)});
    };
    for (std::size_t index = 0; index < masked.size(); ++index) {
        const char c = masked[index];
        if (depth == 0 && c == '#') {
            // Preprocessor lines are the user's, except those Relay writes itself.
            const auto end = masked.find('\n', index);
            const auto line = std::string_view{masked}.substr(index, end == std::string::npos ? std::string::npos : end - index);
            if (line.starts_with("#version") || line.starts_with("#extension") ||
                line.starts_with("#include"))
                error(index, "Relay writes #version, #extension and #include itself; remove this line");
            index = end == std::string::npos ? masked.size() : end;
            statement = index + 1U;
            continue;
        }
        if (c == '{') {
            if (depth == 0) {
                // A function definition: `type name(parameters) {`.
                const auto head = std::string_view{masked}.substr(statement, index - statement);
                const auto open = head.find('(');
                if (open != std::string_view::npos && head.find(')') != std::string_view::npos) {
                    auto name_end = open;
                    while (name_end > 0U && std::isspace(static_cast<unsigned char>(head[name_end - 1U])) != 0)
                        --name_end;
                    auto name_begin = name_end;
                    while (name_begin > 0U && identifier_char(head[name_begin - 1U])) --name_begin;
                    ShaderFunction function;
                    function.name = std::string{head.substr(name_begin, name_end - name_begin)};
                    function.begin = statement;
                    function.line = line_at(text, statement + name_begin);
                    int inner = 0;
                    std::size_t close = index;
                    for (; close < masked.size(); ++close) {
                        if (masked[close] == '{') ++inner;
                        if (masked[close] == '}' && --inner == 0) break;
                    }
                    if (close >= masked.size()) {
                        error(index, "this { is never closed");
                        return result;
                    }
                    function.end = close + 1U;
                    if (function.name == "vertex" || function.name == "fragment") {
                        const auto words = tokens(head);
                        const bool plain = words.size() >= 4U && words[words.size() - 4U] == "void" &&
                                           words[words.size() - 2U] == "(" && words.back() == ")";
                        const bool void_parameters = words.size() >= 5U && words[words.size() - 5U] == "void" &&
                                                     words[words.size() - 3U] == "(" &&
                                                     words[words.size() - 2U] == "void" && words.back() == ")";
                        if (!plain && !void_parameters)
                            error(statement + name_begin, function.name + "() must be declared as void " +
                                                              function.name + "()");
                    }
                    if (function.name == "vertex") result.has_vertex = true;
                    if (function.name == "fragment") result.has_fragment = true;
                    if (function.name == "main")
                        error(statement + name_begin, "Relay writes main() itself; put your code in "
                                                      "vertex() or fragment()");
                    result.functions.push_back(std::move(function));
                    index = close;
                    statement = close + 1U;
                    continue;
                }
            }
            ++depth;
            continue;
        }
        if (c == '}') {
            if (depth == 0) {
                error(index, "unexpected }");
                return result;
            }
            if (--depth == 0 && index + 1U < masked.size()) {
                // A struct definition keeps going to its semicolon.
            }
            continue;
        }
        if (c != ';' || depth != 0) continue;
        const auto words = tokens(std::string_view{masked}.substr(statement, index - statement));
        const auto at = statement + masked.substr(statement, index - statement).find_first_not_of(" \t\r\n");
        if (!words.empty() && words[0] == "shader_type") {
            const auto type = words.size() == 2U ? shader_type_from_name(words[1]) : std::nullopt;
            if (typed) error(at, "the shader type is already set");
            else if (!type) error(at, "shader_type must be surface or post_process");
            else if (at != masked.find_first_not_of(" \t\r\n"))
                error(at, "shader_type must come first");
            else result.type = *type;
            typed = true;
            blank(result.code, statement, index + 1U);
        } else if (!words.empty() && words[0] == "render_mode") {
            for (std::size_t word = 1; word < words.size(); ++word) {
                if (words[word] == ",") continue;
                if (words[word] == "transparent") result.transparent = true;
                else if (words[word] == "unshaded") result.unshaded = true;
                else error(at, "unknown render mode '" + words[word] + "' (use transparent or unshaded)");
            }
            if (result.type == ShaderType::post_process)
                error(at, "post_process shaders have no render modes");
            blank(result.code, statement, index + 1U);
        } else if (!words.empty() && words[0] == "uniform") {
            std::string message;
            auto uniform = parse_uniform(words, message);
            if (!uniform) {
                error(at, message);
            } else if (!names.insert(uniform->name).second) {
                error(at, "there is already a uniform called " + uniform->name);
            } else if (result.uniforms.size() >= maximum_shader_uniforms) {
                error(at, "shaders may declare at most 64 uniforms");
            } else {
                uniform->line = line_at(text, at);
                if (uniform->type == ShaderUniform::Type::sampler2d) {
                    if (result.texture_count >= maximum_shader_textures) {
                        error(at, "shaders may sample at most 8 images");
                        uniform.reset();
                    } else {
                        uniform->offset = 1U + result.texture_count++;
                    }
                } else {
                    const auto alignment = std140_alignment(uniform->type);
                    offset = (offset + alignment - 1U) / alignment * alignment;
                    uniform->offset = offset;
                    offset += 4U * shader_uniform_components(uniform->type);
                }
                if (uniform) result.uniforms.push_back(std::move(*uniform));
            }
            blank(result.code, statement, index + 1U);
        } else if (!words.empty() && (words[0] == "layout" || words[0] == "in" || words[0] == "out")) {
            error(at, "Relay declares the shader's inputs and outputs; use uniform for parameters");
        }
        statement = index + 1U;
    }
    if (depth != 0) error(text.size(), "a { is never closed");
    if (!typed) result.errors.insert(result.errors.begin(), {1U, "the first line must be shader_type surface; "
                                                                 "or shader_type post_process;"});
    if (result.type == ShaderType::post_process && result.has_vertex) {
        for (const auto& function : result.functions)
            if (function.name == "vertex")
                result.errors.push_back({function.line, "post_process shaders have no vertex()"});
    }
    result.reads_blurred = result.type == ShaderType::post_process &&
                           result.code.find("scene_color_lod") != std::string::npos;
    result.uniform_bytes = std::max(16U, (offset + 15U) / 16U * 16U);
    if (result.uniform_bytes > maximum_shader_uniform_bytes)
        result.errors.push_back({0U, "the uniforms take more than 4 KiB"});
    std::sort(result.errors.begin(), result.errors.end());
    return result;
}

GeneratedShader generate_shader_glsl(const ParsedShader& shader) {
    GeneratedShader result;
    if (shader.type == ShaderType::post_process) {
        result.fragment = post_fragment(shader);
        return result;
    }
    result.vertex = surface_vertex(shader);
    result.fragment = surface_fragment(shader, false);
    result.shadow_fragment = surface_fragment(shader, true);
    return result;
}

bool runtime_shader_compiler_available() {
#ifdef RELAY_HAS_GLSLANG
    return true;
#else
    return false;
#endif
}

std::shared_ptr<const CompiledShader> compile_relay_shader(const std::string_view path,
                                                           const std::string_view text) {
    auto compiled = std::make_shared<CompiledShader>();
    compiled->path = std::string{path};
    compiled->revision = next_shader_revision();
    compiled->parsed = parse_relay_shader(text);
    compiled->errors = compiled->parsed.errors;
    if (!compiled->errors.empty()) return compiled;
#ifdef RELAY_HAS_GLSLANG
    const auto generated = generate_shader_glsl(compiled->parsed);
    std::vector<ShaderMessage> messages;
    if (compiled->parsed.type == ShaderType::post_process) {
        compile_stage(EShLangFragment, generated.fragment, compiled->fragment, messages);
    } else if (compile_stage(EShLangVertex, generated.vertex, compiled->vertex, messages) &&
               compile_stage(EShLangFragment, generated.fragment, compiled->fragment, messages)) {
        compile_stage(EShLangFragment, generated.shadow_fragment, compiled->shadow_fragment, messages);
    }
    std::sort(messages.begin(), messages.end());
    messages.erase(std::unique(messages.begin(), messages.end()), messages.end());
    compiled->errors = std::move(messages);
    if (!compiled->errors.empty()) {
        compiled->vertex.clear();
        compiled->fragment.clear();
        compiled->shadow_fragment.clear();
    }
#else
    compiled->errors.push_back({0U, "this build of Relay cannot compile shaders (glslang was not found)"});
#endif
    return compiled;
}

bool valid_shader_path(const std::string_view path) {
    if (path.size() <= shader_suffix.size() || path.size() > 128U || !path.ends_with(shader_suffix))
        return false;
    return workspace_file(".", path, shader_suffix).has_value() &&
           !fs::path(std::string{path}).is_absolute();
}

std::string shader_template(const ShaderType type) {
    if (type == ShaderType::post_process)
        return R"(shader_type post_process;

// A full-screen effect on the lit scene, before tone mapping. COLOR starts as the scene's color
// at this pixel; scene_color(uv), scene_depth(uv) and scene_normal(uv) read other pixels.
uniform float strength : hint_range(0.0, 1.0) = 0.5;
uniform vec3 tint : source_color = vec3(1.0, 0.85, 0.7);

void fragment() {
    float grey = dot(COLOR, vec3(0.2126, 0.7152, 0.0722));
    vec3 graded = mix(COLOR, grey * tint, strength);
    // Darken towards the corners.
    float vignette = 1.0 - 0.6 * dot(SCREEN_UV - 0.5, SCREEN_UV - 0.5);
    COLOR = graded * vignette;
}
)";
    return R"(shader_type surface;

// How a mesh's surface looks. Relay lights it, shades it and fogs it like every other surface.
// Set ALBEDO, ROUGHNESS, METALLIC, EMISSION, ALPHA, AO or NORMAL_MAP in fragment(); move VERTEX
// in vertex(). Uniforms become fields of every material using this shader.
uniform vec3 color : source_color = vec3(0.8, 0.35, 0.2);
uniform float roughness : hint_range(0.0, 1.0) = 0.4;
uniform float wave_height : hint_range(0.0, 1.0) = 0.0;

void vertex() {
    VERTEX += NORMAL * sin(TIME * 2.0 + VERTEX.x * 4.0) * wave_height;
}

void fragment() {
    ALBEDO = color;
    ROUGHNESS = roughness;
}
)";
}

const std::vector<ShaderEffect>& shader_effects() {
    static const std::vector<ShaderEffect> effects = [] {
        const auto text = [](const std::string_view file) {
            for (const auto& source : embedded_shader_sources())
                if (source.name == file) return source.text;
            return std::string_view{};
        };
        return std::vector<ShaderEffect>{
            {"bloom", "Bloom", "Bright parts of the picture glow into their surroundings", text("Bloom.relay-shader")},
            {"color_grading", "Color Grading",
             "Exposure, contrast, saturation, white balance and tints for dark and bright parts",
             text("ColorGrading.relay-shader")},
            {"vignette", "Vignette", "The picture darkens towards its edges", text("Vignette.relay-shader")},
        };
    }();
    return effects;
}

const ShaderEffect* find_shader_effect(const std::string_view id) {
    for (const auto& effect : shader_effects())
        if (effect.id == id) return &effect;
    return nullptr;
}

std::optional<std::string> read_shader_file(const fs::path& root, const std::string_view path,
                                            std::string& error) {
    const auto file = valid_shader_path(path) ? workspace_file(root.generic_string(), path, shader_suffix)
                                              : std::nullopt;
    if (!file) {
        error = "shader path must be a safe project-relative .relay-shader file";
        return std::nullopt;
    }
    std::error_code code;
    if (!fs::is_regular_file(*file, code)) {
        error = std::string{path} + ": file not found";
        return std::nullopt;
    }
    const auto size = fs::file_size(*file, code);
    if (code || size > maximum_shader_bytes) {
        error = std::string{path} + ": shaders may be at most 256 KiB";
        return std::nullopt;
    }
    std::ifstream input(*file, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(size))) {
        error = std::string{path} + ": could not read the file";
        return std::nullopt;
    }
    return text;
}

bool write_shader_file(const fs::path& root, const std::string_view path, const std::string_view text,
                       const bool create_only, std::string& error) {
    const auto file = valid_shader_path(path) ? workspace_file(root.generic_string(), path, shader_suffix)
                                              : std::nullopt;
    if (!file) {
        error = "shader path must be a safe project-relative .relay-shader file";
        return false;
    }
    if (text.size() > maximum_shader_bytes) {
        error = "shaders may be at most 256 KiB";
        return false;
    }
    std::error_code code;
    if (!fs::is_directory(file->parent_path(), code)) {
        error = "the shader's folder does not exist";
        return false;
    }
    if (fs::exists(*file, code) && (create_only || !fs::is_regular_file(*file, code))) {
        error = create_only ? "a file with that name already exists" : "the shader path is not a file";
        return false;
    }
    const auto temporary = file->parent_path() / (".relay-tmp-" + file->filename().string());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) {
            error = "could not write " + file->filename().string();
            output.close();
            fs::remove(temporary, code);
            return false;
        }
    }
    fs::rename(temporary, *file, code);
    if (code) {
        error = "could not replace " + file->filename().string() + ": " + code.message();
        fs::remove(temporary, code);
        return false;
    }
    return true;
}

std::shared_ptr<const CompiledShader> ShaderLibrary::resolve(const fs::path& root,
                                                             const std::string_view path) {
    const auto file = valid_shader_path(path) ? workspace_file(root.generic_string(), path, shader_suffix)
                                              : std::nullopt;
    Stamp stamp;
    if (file) {
        std::error_code code;
        if (fs::is_regular_file(*file, code)) {
            stamp.time = fs::last_write_time(*file, code);
            stamp.size = code ? 0U : fs::file_size(*file, code);
            stamp.exists = !code;
        }
    }
    const auto preview_found = previews_.find(path);
    const std::optional<std::string> previewed =
        preview_found == previews_.end() ? std::nullopt : std::optional<std::string>{preview_found->second};
    auto found = entries_.find(path);
    if (found != entries_.end() && found->second.root == root && found->second.stamp == stamp &&
        found->second.previewed == previewed)
        return found->second.compiled;
    if (found == entries_.end() && entries_.size() >= 64U) entries_.clear();
    Entry entry;
    entry.root = root;
    entry.stamp = stamp;
    entry.previewed = previewed;
    if (previewed) {
        entry.compiled = compile_relay_shader(path, *previewed);
    } else {
        std::string error;
        const auto text = read_shader_file(root, path, error);
        if (text) {
            entry.compiled = compile_relay_shader(path, *text);
        } else {
            auto missing = std::make_shared<CompiledShader>();
            missing->path = std::string{path};
            missing->revision = next_shader_revision();
            missing->errors.push_back({0U, error});
            entry.compiled = std::move(missing);
        }
    }
    auto result = entry.compiled;
    entries_.insert_or_assign(std::string{path}, std::move(entry));
    return result;
}

void ShaderLibrary::set_preview(const std::string_view path, std::optional<std::string> text) {
    if (text) previews_.insert_or_assign(std::string{path}, std::move(*text));
    else if (const auto found = previews_.find(path); found != previews_.end()) previews_.erase(found);
}

const std::string* ShaderLibrary::preview(const std::string_view path) const {
    const auto found = previews_.find(path);
    return found == previews_.end() ? nullptr : &found->second;
}

std::vector<std::string> ShaderLibrary::previews() const {
    std::vector<std::string> paths;
    for (const auto& [path, text] : previews_) paths.push_back(path);
    return paths;
}

} // namespace relay
