#include "relay/render/sky.hpp"

#include "relay/core/json.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/scene/project.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <vector>

namespace relay {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view material_suffix = ".relay-material";

bool within(const double value, const double minimum, const double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::string lowercase(std::string_view value) {
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::optional<fs::path> material_file(const fs::path& root, const std::string_view path) {
    if (!valid_sky_material_path(path)) return std::nullopt;
    return workspace_file(root.generic_string(), path, material_suffix);
}

std::optional<fs::path> panorama_file(const fs::path& root, const std::string_view path) {
    if (!valid_sky_panorama_path(path)) return std::nullopt;
    return workspace_file(root.generic_string(), path, "");
}

std::optional<std::string> read_file(const fs::path& path, const std::uintmax_t maximum,
                                     std::string& error) {
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

float srgb_to_linear(const std::uint8_t value) {
    const float c = static_cast<float>(value) / 255.0F;
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

std::uint64_t next_panorama_revision() {
    static std::atomic<std::uint64_t> revision{0};
    return ++revision;
}

} // namespace

bool valid_sky_panorama_path(const std::string_view path) {
    const auto name = lowercase(path);
    if (!name.ends_with(".png") && !name.ends_with(".jpg") && !name.ends_with(".jpeg")) return false;
    if (path.size() > 128U) return false;
    const fs::path relative{std::string(path)};
    if (relative.is_absolute()) return false;
    for (const auto& part : relative) {
        const auto piece = part.string();
        if (piece.empty() || piece.front() == '.') return false;
        for (const char c : piece)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == ' '))
                return false;
    }
    return true;
}

bool valid_sky_material(const SkyMaterial& material) {
    return (material.panorama.empty() || valid_sky_panorama_path(material.panorama)) &&
           within(material.tint.x, 0.0, maximum_sky_color) &&
           within(material.tint.y, 0.0, maximum_sky_color) &&
           within(material.tint.z, 0.0, maximum_sky_color) &&
           within(material.intensity, 0.0, 100.0) &&
           within(material.rotation_degrees, -360.0, 360.0);
}

std::string sky_material_json(const SkyMaterial& material) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "{\n  \"format\": \"relay.material\",\n  \"version\": 1,\n  \"type\": \"sky\",\n"
           << "  \"panorama\": \"" << json_escape(material.panorama) << "\",\n"
           << "  \"tint\": {\"x\": " << material.tint.x << ", \"y\": " << material.tint.y
           << ", \"z\": " << material.tint.z << "},\n"
           << "  \"intensity\": " << material.intensity << ",\n"
           << "  \"rotation_degrees\": " << material.rotation_degrees << "\n}\n";
    return output.str();
}

std::optional<SkyMaterial> parse_sky_material(const std::string_view document, std::string& error) {
    JsonParser parser(document);
    const auto root = parser.parse();
    const auto* object = root ? root->object() : nullptr;
    if (!object) {
        error = root ? "a material file must be a JSON object" : "invalid JSON: " + parser.error();
        return std::nullopt;
    }
    const auto text = [&](const char* key) -> const std::string* {
        const auto found = object->find(key);
        return found == object->end() ? nullptr : found->second.string();
    };
    const auto number = [&](const char* key, double& target) {
        const auto found = object->find(key);
        if (found == object->end() || !found->second.number()) return false;
        target = *found->second.number();
        return true;
    };
    const auto* format = text("format");
    const auto* type = text("type");
    double version = 0.0;
    if (!format || *format != "relay.material" || !number("version", version) || version != 1.0) {
        error = "not a version 1 relay.material file";
        return std::nullopt;
    }
    if (!type || *type != "sky") {
        error = "the material is not a sky material";
        return std::nullopt;
    }
    SkyMaterial material;
    const auto* panorama = text("panorama");
    const auto tint = object->find("tint");
    const auto* tint_object = tint == object->end() ? nullptr : tint->second.object();
    const auto channel = [&](const char* key, double& target) {
        const auto found = tint_object ? tint_object->find(key) : JsonValue::Object::const_iterator{};
        if (!tint_object || found == tint_object->end() || !found->second.number()) return false;
        target = *found->second.number();
        return true;
    };
    if (!panorama || !channel("x", material.tint.x) || !channel("y", material.tint.y) ||
        !channel("z", material.tint.z) || !number("intensity", material.intensity) ||
        !number("rotation_degrees", material.rotation_degrees)) {
        error = "a sky material needs panorama, tint, intensity and rotation_degrees";
        return std::nullopt;
    }
    material.panorama = *panorama;
    if (!valid_sky_material(material)) {
        error = "sky material values outside valid ranges";
        return std::nullopt;
    }
    return material;
}

std::optional<SkyMaterial> read_sky_material(const fs::path& root, const std::string_view path,
                                             std::string& error) {
    const auto file = material_file(root, path);
    if (!file) {
        error = "sky material path must be a safe project-relative .relay-material file";
        return std::nullopt;
    }
    std::string read_error;
    const auto text = read_file(*file, maximum_sky_material_bytes, read_error);
    if (!text) {
        error = std::string{path} + ": " + read_error;
        return std::nullopt;
    }
    auto material = parse_sky_material(*text, error);
    if (!material) error = std::string{path} + ": " + error;
    return material;
}

bool write_sky_material(const fs::path& root, const std::string_view path,
                        const SkyMaterial& material, const bool create_only, std::string& error) {
    const auto file = material_file(root, path);
    if (!file) {
        error = "sky material path must be a safe project-relative .relay-material file";
        return false;
    }
    if (!valid_sky_material(material)) {
        error = "sky material values outside valid ranges";
        return false;
    }
    std::error_code code;
    if (!fs::is_directory(file->parent_path(), code)) {
        error = "the material's folder does not exist";
        return false;
    }
    if (fs::exists(*file, code)) {
        if (create_only) {
            error = "a file with that name already exists";
            return false;
        }
        if (!fs::is_regular_file(*file, code)) {
            error = "the material path is not a file";
            return false;
        }
    }
    const auto text = sky_material_json(material);
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

double sky_gradient_position(const double direction_y) {
    const double below = 1.0 - std::clamp(direction_y, 0.0, 1.0);
    return 1.0 - below * below;
}

std::array<double, 2> sky_panorama_uv(const Vec3& direction, const double rotation_degrees) {
    constexpr double pi = std::numbers::pi;
    double u = 0.5 + std::atan2(direction.x, -direction.z) / (2.0 * pi) - rotation_degrees / 360.0;
    u -= std::floor(u);
    const double v = 0.5 - std::asin(std::clamp(direction.y, -1.0, 1.0)) / pi;
    return {u, v};
}

void compute_sky_panorama_averages(SkyPanorama& panorama) {
    const auto& image = panorama.image;
    panorama.upper = panorama.lower = {};
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() < static_cast<std::size_t>(image.width) * image.height * 4U)
        return;
    // A coarse grid is plenty for an average and keeps huge images cheap.
    const std::uint32_t columns = std::min(image.width, 256U);
    const std::uint32_t rows = std::min(image.height, 128U);
    std::array<double, 3> upper{}, lower{};
    double upper_weight = 0.0, lower_weight = 0.0;
    for (std::uint32_t row = 0; row < rows; ++row) {
        const double v = (row + 0.5) / rows;
        const double latitude = (0.5 - v) * std::numbers::pi;
        // Each texel covers cos(latitude) of solid angle, and lights an up- or downward surface
        // by the height of its direction.
        const double weight = std::cos(latitude) * std::abs(std::sin(latitude));
        const auto y = std::min(static_cast<std::uint32_t>(v * image.height), image.height - 1U);
        for (std::uint32_t column = 0; column < columns; ++column) {
            const auto x = std::min(static_cast<std::uint32_t>((column + 0.5) / columns * image.width),
                                    image.width - 1U);
            const auto* texel = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4U];
            auto& sum = latitude > 0.0 ? upper : lower;
            for (std::size_t channel = 0; channel < 3U; ++channel)
                sum[channel] += weight * srgb_to_linear(texel[channel]);
            (latitude > 0.0 ? upper_weight : lower_weight) += weight;
        }
    }
    for (std::size_t channel = 0; channel < 3U; ++channel) {
        panorama.upper[channel] =
            upper_weight > 0.0 ? static_cast<float>(upper[channel] / upper_weight) : 0.0F;
        panorama.lower[channel] =
            lower_weight > 0.0 ? static_cast<float>(lower[channel] / lower_weight) : 0.0F;
    }
}

SkyMaterialCache::Stamp SkyMaterialCache::stamp(const std::optional<fs::path>& path) {
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

std::shared_ptr<const ResolvedSkyMaterial> SkyMaterialCache::resolve(const fs::path& root,
                                                                     const std::string_view path) {
    const auto material_path = material_file(root, path);
    const auto material_stamp = stamp(material_path);
    auto found = entries_.find(path);
    if (found != entries_.end() && found->second.root == root &&
        found->second.material == material_stamp) {
        const auto& resolved = *found->second.resolved;
        const auto image_path = resolved.material.panorama.empty()
                                    ? std::nullopt : panorama_file(root, resolved.material.panorama);
        if (found->second.image == stamp(image_path)) return found->second.resolved;
    }
    // An edit to the material alone, such as its tint, keeps the decoded image.
    std::shared_ptr<const SkyPanorama> previous_panorama;
    std::string previous_image;
    Stamp previous_image_stamp;
    if (found != entries_.end() && found->second.root == root) {
        previous_panorama = found->second.resolved->panorama;
        previous_image = found->second.resolved->material.panorama;
        previous_image_stamp = found->second.image;
    }
    // Nothing is kept for materials no longer in use once a few have come and gone.
    if (found == entries_.end() && entries_.size() >= 8U) entries_.clear();

    auto resolved = std::make_shared<ResolvedSkyMaterial>();
    resolved->path = std::string{path};
    Entry entry;
    entry.root = root;
    entry.material = material_stamp;
    std::string error;
    if (const auto material = read_sky_material(root, path, error)) {
        resolved->material = *material;
        const auto image_path = material->panorama.empty()
                                    ? std::nullopt : panorama_file(root, material->panorama);
        entry.image = stamp(image_path);
        std::string image_error;
        std::optional<std::string> bytes;
        if (material->panorama.empty()) {
            resolved->error = "the sky material has no panorama image";
        } else if (previous_panorama && material->panorama == previous_image &&
                   entry.image == previous_image_stamp && entry.image.exists) {
            resolved->panorama = previous_panorama;
        } else if (!image_path) {
            resolved->error = "the panorama path is not a safe project-relative PNG or JPEG";
        } else if (!(bytes = read_file(*image_path, maximum_sky_panorama_file_bytes, image_error))) {
            resolved->error = material->panorama + ": " + image_error;
        } else {
            auto panorama = std::make_shared<SkyPanorama>();
            const auto* data = reinterpret_cast<const std::uint8_t*>(bytes->data());
            if (!decode_image_rgba({data, bytes->size()}, image_path->extension().string(),
                                   panorama->image, image_error)) {
                resolved->error = material->panorama + ": " + image_error;
            } else if (panorama->image.width > maximum_sky_panorama_width ||
                       panorama->image.height > maximum_sky_panorama_height) {
                resolved->error = material->panorama + ": panoramas may be at most 8192 x 4096";
            } else {
                panorama->image.name = "sky:" + material->panorama;
                panorama->image.color_space = TextureColorSpace::srgb;
                panorama->image.wrap_u = TextureWrap::repeat;
                panorama->image.wrap_v = TextureWrap::clamp_to_edge;
                compute_sky_panorama_averages(*panorama);
                panorama->revision = next_panorama_revision();
                resolved->panorama = std::move(panorama);
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
