#include "relay/render/materials.hpp"

#include "relay/core/json.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/render/sky.hpp"
#include "relay/scene/project.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace relay {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view material_suffix = ".relay-material";
constexpr std::uintmax_t maximum_material_bytes = 64U * 1024U;
constexpr std::uintmax_t maximum_image_bytes = 64U * 1024U * 1024U;

std::uint64_t next_material_revision() {
    static std::atomic<std::uint64_t> revision{0};
    return ++revision;
}

std::optional<fs::path> material_file(const fs::path& root, const std::string_view path) {
    if (!valid_material_path(path)) return std::nullopt;
    return workspace_file(root.generic_string(), path, material_suffix);
}

std::optional<std::string> read_text(const fs::path& path, const std::uintmax_t maximum, std::string& error) {
    std::error_code code;
    if (!fs::is_regular_file(path, code)) {
        error = "file not found";
        return std::nullopt;
    }
    const auto size = fs::file_size(path, code);
    if (code || size > maximum) {
        error = code ? "could not read the file size" : "file is too large";
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(size))) {
        error = "could not read the file";
        return std::nullopt;
    }
    return text;
}

} // namespace

bool valid_material_path(const std::string_view path) {
    // The sky's check is the same shape: safe characters, no hidden or absolute parts.
    return valid_sky_material_path(path);
}

std::optional<std::string> read_material_type(const fs::path& root, const std::string_view path,
                                              std::string& error) {
    const auto file = material_file(root, path);
    if (!file) {
        error = "material path must be a safe project-relative .relay-material file";
        return std::nullopt;
    }
    const auto text = read_text(*file, maximum_material_bytes, error);
    if (!text) {
        error = std::string{path} + ": " + error;
        return std::nullopt;
    }
    JsonParser parser(*text);
    const auto value = parser.parse();
    const auto* object = value ? value->object() : nullptr;
    const auto* type = object ? field(*object, "type") : nullptr;
    if (!type || !type->string()) {
        error = std::string{path} + ": not a relay.material file with a type";
        return std::nullopt;
    }
    return *type->string();
}

std::string shader_material_json(const ShaderMaterial& material) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "{\n  \"format\": \"relay.material\",\n  \"version\": 1,\n  \"type\": \""
           << shader_type_name(material.type) << "\",\n  \"shader\": \"" << json_escape(material.shader)
           << "\",\n  \"parameters\": {";
    bool first = true;
    for (const auto& [name, value] : material.parameters) {
        output << (first ? "\n    " : ",\n    ") << '"' << json_escape(name) << "\": ";
        first = false;
        if (!value.texture.empty() || value.numbers.empty()) {
            output << '"' << json_escape(value.texture) << '"';
        } else {
            output << '[';
            for (std::size_t index = 0; index < value.numbers.size(); ++index)
                output << (index ? ", " : "") << value.numbers[index];
            output << ']';
        }
    }
    output << (first ? "}" : "\n  }") << "\n}\n";
    return output.str();
}

std::optional<ShaderMaterial> parse_shader_material(const std::string_view document, std::string& error) {
    JsonParser parser(document);
    const auto root = parser.parse();
    const auto* object = root ? root->object() : nullptr;
    if (!object) {
        error = root ? "a material file must be a JSON object" : "invalid JSON: " + parser.error();
        return std::nullopt;
    }
    const auto* format = field(*object, "format");
    const auto* version = field(*object, "version");
    const auto* type = field(*object, "type");
    const auto* shader = field(*object, "shader");
    const auto* parameters = field(*object, "parameters");
    if (!format || !format->string() || *format->string() != "relay.material" || !version ||
        !version->number() || *version->number() != 1.0) {
        error = "not a version 1 relay.material file";
        return std::nullopt;
    }
    const auto parsed_type = type && type->string() ? shader_type_from_name(*type->string()) : std::nullopt;
    if (!parsed_type) {
        error = "the material is not a surface or post_process material";
        return std::nullopt;
    }
    if (!shader || !shader->string() || !parameters || !parameters->object()) {
        error = "a shader material needs shader and parameters";
        return std::nullopt;
    }
    ShaderMaterial material;
    material.type = *parsed_type;
    material.shader = *shader->string();
    if (!material.shader.empty() && !valid_shader_path(material.shader)) {
        error = "the material's shader must be a project-relative .relay-shader file";
        return std::nullopt;
    }
    if (parameters->object()->size() > maximum_material_parameters) {
        error = "a material has at most 64 parameters";
        return std::nullopt;
    }
    for (const auto& [name, value] : *parameters->object()) {
        MaterialValue parsed;
        if (const auto* text = value.string()) {
            if (!text->empty() && !valid_sky_panorama_path(*text)) {
                error = "parameter " + name + " must name a PNG or JPEG image in the project";
                return std::nullopt;
            }
            parsed.texture = *text;
        } else if (const auto* numbers = value.array(); numbers && !numbers->empty() && numbers->size() <= 4U) {
            for (const auto& number : *numbers) {
                if (!number.number() || !std::isfinite(*number.number())) {
                    error = "parameter " + name + " must be an array of numbers";
                    return std::nullopt;
                }
                parsed.numbers.push_back(*number.number());
            }
        } else if (const auto* number = value.number(); number && std::isfinite(*number)) {
            parsed.numbers.push_back(*number);
        } else if (const auto* flag = value.boolean()) {
            parsed.numbers.push_back(*flag ? 1.0 : 0.0);
        } else {
            error = "parameter " + name + " must be numbers or an image path";
            return std::nullopt;
        }
        material.parameters.emplace(name, std::move(parsed));
    }
    return material;
}

std::optional<ShaderMaterial> read_shader_material(const fs::path& root, const std::string_view path,
                                                   std::string& error) {
    const auto file = material_file(root, path);
    if (!file) {
        error = "material path must be a safe project-relative .relay-material file";
        return std::nullopt;
    }
    const auto text = read_text(*file, maximum_material_bytes, error);
    if (!text) {
        error = std::string{path} + ": " + error;
        return std::nullopt;
    }
    auto material = parse_shader_material(*text, error);
    if (!material) error = std::string{path} + ": " + error;
    return material;
}

bool write_shader_material(const fs::path& root, const std::string_view path, const ShaderMaterial& material,
                           const bool create_only, std::string& error) {
    const auto file = material_file(root, path);
    if (!file) {
        error = "material path must be a safe project-relative .relay-material file";
        return false;
    }
    std::string check;
    if (!parse_shader_material(shader_material_json(material), check)) {
        error = check;
        return false;
    }
    std::error_code code;
    if (!fs::is_directory(file->parent_path(), code)) {
        error = "the material's folder does not exist";
        return false;
    }
    if (fs::exists(*file, code) && (create_only || !fs::is_regular_file(*file, code))) {
        error = create_only ? "a file with that name already exists" : "the material path is not a file";
        return false;
    }
    const auto text = shader_material_json(material);
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

bool valid_material_value(const ShaderUniform& uniform, const MaterialValue& value, std::string& error) {
    if (uniform.type == ShaderUniform::Type::sampler2d) {
        if (!value.numbers.empty() || (!value.texture.empty() && !valid_sky_panorama_path(value.texture))) {
            error = uniform.name + " takes a PNG or JPEG image from the project";
            return false;
        }
        return true;
    }
    const auto components = shader_uniform_components(uniform.type);
    if (!value.texture.empty() || value.numbers.size() != components ||
        !std::all_of(value.numbers.begin(), value.numbers.end(), [](double v) { return std::isfinite(v); })) {
        error = uniform.name + " takes " + std::to_string(components) + " number" + (components == 1U ? "" : "s");
        return false;
    }
    return true;
}

namespace {

// Writes one numeric uniform's values into a std140 block.
void write_uniform(std::vector<std::uint8_t>& block, const ShaderUniform& uniform, const double* values) {
    const auto components = shader_uniform_components(uniform.type);
    if (uniform.offset + components * 4U > block.size()) return;
    for (std::size_t index = 0; index < components; ++index) {
        auto* destination = block.data() + uniform.offset + index * 4U;
        if (uniform.type == ShaderUniform::Type::int_value) {
            const auto value = static_cast<std::int32_t>(std::clamp(std::round(values[index]), -2147483648.0, 2147483647.0));
            std::memcpy(destination, &value, 4U);
        } else if (uniform.type == ShaderUniform::Type::bool_value) {
            const std::uint32_t value = values[index] != 0.0 ? 1U : 0U;
            std::memcpy(destination, &value, 4U);
        } else {
            const auto value = static_cast<float>(values[index]);
            std::memcpy(destination, &value, 4U);
        }
    }
}

} // namespace

std::vector<std::uint8_t> pack_material_parameters(const ParsedShader& shader, const ShaderMaterial& material) {
    std::vector<std::uint8_t> block(shader.uniform_bytes, 0U);
    for (const auto& uniform : shader.uniforms) {
        if (uniform.type == ShaderUniform::Type::sampler2d) continue;
        const auto components = shader_uniform_components(uniform.type);
        std::array<double, 4> values = uniform.default_value;
        const auto found = material.parameters.find(uniform.name);
        std::string ignored;
        if (found != material.parameters.end() && valid_material_value(uniform, found->second, ignored))
            for (std::size_t index = 0; index < components; ++index) values[index] = found->second.numbers[index];
        write_uniform(block, uniform, values.data());
    }
    return block;
}

std::vector<std::uint8_t> override_material_parameters(
    const ResolvedShaderMaterial& material, const std::map<std::string, std::vector<double>, std::less<>>& values,
    std::vector<std::string>* skipped) {
    auto block = material.parameters;
    if (!material.shader) return block;
    for (const auto& [name, numbers] : values) {
        const auto* uniform = material.shader->parsed.find_uniform(name);
        if (!uniform || uniform->type == ShaderUniform::Type::sampler2d ||
            numbers.size() != shader_uniform_components(uniform->type)) {
            if (skipped) skipped->push_back(name);
            continue;
        }
        write_uniform(block, *uniform, numbers.data());
    }
    return block;
}

MaterialLibrary::Stamp MaterialLibrary::stamp(const std::optional<fs::path>& path) {
    Stamp result;
    if (!path) return result;
    std::error_code code;
    if (!fs::is_regular_file(*path, code)) return result;
    result.time = fs::last_write_time(*path, code);
    if (code) return {};
    result.size = fs::file_size(*path, code);
    if (code) return {};
    result.exists = true;
    return result;
}

const MaterialLibrary::Image& MaterialLibrary::image(const fs::path& root, const std::string& path,
                                                     const bool srgb) {
    const auto file = valid_sky_panorama_path(path) ? workspace_file(root.generic_string(), path, "")
                                                    : std::nullopt;
    const auto current = stamp(file);
    const auto key = root.generic_string() + '|' + path + (srgb ? "|srgb" : "|linear");
    auto found = images_.find(key);
    if (found != images_.end() && found->second.stamp == current) return found->second;
    if (images_.size() >= 256U) images_.clear();
    Image loaded;
    loaded.stamp = current;
    std::string error;
    if (!file) {
        loaded.error = path + ": not a safe project-relative PNG or JPEG";
    } else if (const auto bytes = read_text(*file, maximum_image_bytes, error); !bytes) {
        loaded.error = path + ": " + error;
    } else {
        auto texture = std::make_shared<TextureAsset>();
        const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
        if (!decode_image_rgba({data, bytes->size()}, file->extension().string(), *texture, error)) {
            loaded.error = path + ": " + error;
        } else {
            texture->name = "material:" + path;
            texture->color_space = srgb ? TextureColorSpace::srgb : TextureColorSpace::linear;
            loaded.texture = std::move(texture);
        }
    }
    return images_.insert_or_assign(key, std::move(loaded)).first->second;
}

void MaterialLibrary::clear() {
    entries_.clear();
    images_.clear();
}

std::shared_ptr<const ResolvedShaderMaterial> MaterialLibrary::resolve(const fs::path& root,
                                                                      const std::string_view path,
                                                                      ShaderLibrary& shaders) {
    const auto current = stamp(material_file(root, path));
    auto found = entries_.find(path);
    // Unchanged file: the material is still current unless its shader or an image changed.
    if (found != entries_.end() && found->second.root == root && found->second.stamp == current) {
        const auto& cached = *found->second.resolved;
        std::vector<const TextureAsset*> textures;
        std::uint64_t shader_revision = 0U;
        if (!cached.material.shader.empty()) {
            const auto shader = shaders.resolve(root, cached.material.shader);
            shader_revision = shader->revision;
            for (const auto& uniform : shader->parsed.uniforms)
                if (uniform.type == ShaderUniform::Type::sampler2d) {
                    const auto value = cached.material.parameters.find(uniform.name);
                    textures.push_back(value == cached.material.parameters.end() || value->second.texture.empty()
                                           ? nullptr
                                           : image(root, value->second.texture,
                                                   uniform.hint == ShaderUniform::Hint::color)
                                                 .texture.get());
                }
        }
        if (shader_revision == found->second.shader_revision && textures == found->second.textures)
            return found->second.resolved;
    }
    if (found == entries_.end() && entries_.size() >= 256U) entries_.clear();

    auto resolved = std::make_shared<ResolvedShaderMaterial>();
    resolved->path = std::string{path};
    resolved->revision = next_material_revision();
    Entry entry;
    entry.root = root;
    entry.stamp = current;
    std::string error;
    if (const auto material = read_shader_material(root, path, error)) {
        resolved->material = *material;
        if (material->shader.empty()) {
            resolved->error = "the material has no shader";
        } else {
            resolved->shader = shaders.resolve(root, material->shader);
            entry.shader_revision = resolved->shader->revision;
            const auto& parsed = resolved->shader->parsed;
            if (!resolved->shader->ok()) {
                const auto& first = resolved->shader->errors.empty() ? ShaderMessage{0U, "it does not compile"}
                                                                       : resolved->shader->errors.front();
                resolved->error = material->shader + (first.line ? ":" + std::to_string(first.line) : "") +
                                  ": " + first.text;
            } else if (parsed.type != material->type) {
                resolved->error = "the material is a " + std::string{shader_type_name(material->type)} +
                                  " material but its shader is a " + std::string{shader_type_name(parsed.type)} +
                                  " shader";
            }
            resolved->parameters = pack_material_parameters(parsed, *material);
            for (const auto& [name, value] : material->parameters) {
                const auto* uniform = parsed.find_uniform(name);
                std::string problem;
                if (!uniform) resolved->warnings.push_back("the shader has no uniform called " + name);
                else if (!valid_material_value(*uniform, value, problem)) resolved->warnings.push_back(problem);
            }
            for (const auto& uniform : parsed.uniforms) {
                if (uniform.type != ShaderUniform::Type::sampler2d) continue;
                const auto value = material->parameters.find(uniform.name);
                if (value == material->parameters.end() || value->second.texture.empty()) {
                    resolved->textures.push_back(nullptr);
                    entry.textures.push_back(nullptr);
                    continue;
                }
                const auto& loaded = image(root, value->second.texture, uniform.hint == ShaderUniform::Hint::color);
                if (!loaded.error.empty()) resolved->warnings.push_back(loaded.error);
                resolved->textures.push_back(loaded.texture);
                entry.textures.push_back(loaded.texture.get());
            }
        }
    } else {
        resolved->error = error;
    }
    entry.resolved = std::move(resolved);
    auto result = entry.resolved;
    entries_.insert_or_assign(std::string{path}, std::move(entry));
    return result;
}

} // namespace relay
