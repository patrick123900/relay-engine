#pragma once

#include "relay/render/assets.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace relay {

// A sky material: a project file ending in .relay-material whose type is "sky". It wraps an
// equirectangular (2:1 latitude-longitude) panorama image from the project, such as an exported
// photo sphere, with a tint, a brightness and a turn about the vertical axis. A Sky component
// that names one draws it instead of its color gradient.
struct SkyMaterial {
    std::string panorama;         // Project-relative PNG or JPEG.
    Vec3 tint{1.0, 1.0, 1.0};     // Linear multiplier, 0 to 1000 per channel.
    double intensity{1.0};        // 0 to 100.
    double rotation_degrees{0.0}; // -360 to 360; positive turns the image to the left.
    auto operator<=>(const SkyMaterial&) const = default;
};

inline constexpr std::uintmax_t maximum_sky_material_bytes = 64U * 1024U;
inline constexpr std::uintmax_t maximum_sky_panorama_file_bytes = 128U * 1024U * 1024U;
inline constexpr std::uint32_t maximum_sky_panorama_width = 8192U;
inline constexpr std::uint32_t maximum_sky_panorama_height = 4096U;

// A project-relative .png, .jpg or .jpeg path, with the characters project files may use.
[[nodiscard]] bool valid_sky_panorama_path(std::string_view path);
// An empty panorama is allowed: the material then draws nothing until one is chosen.
[[nodiscard]] bool valid_sky_material(const SkyMaterial& material);
// The file document, with a trailing newline.
[[nodiscard]] std::string sky_material_json(const SkyMaterial& material);
[[nodiscard]] std::optional<SkyMaterial> parse_sky_material(std::string_view document,
                                                            std::string& error);
// Reads or writes a material under the project root. Paths go through workspace_file(), so they
// cannot leave the project or pass through symbolic links. Writing replaces the file atomically;
// `create_only` refuses to replace an existing one.
[[nodiscard]] std::optional<SkyMaterial> read_sky_material(const std::filesystem::path& root,
                                                           std::string_view path,
                                                           std::string& error);
[[nodiscard]] bool write_sky_material(const std::filesystem::path& root, std::string_view path,
                                      const SkyMaterial& material, bool create_only,
                                      std::string& error);

// Where a view direction's height falls on the sky gradient: 0 at the horizon and below, 1
// straight up. It rises quickly above the horizon and levels off overhead, as daylight skies do.
// shaders/surface_lighting.glsl has the same curve.
[[nodiscard]] double sky_gradient_position(double direction_y);
// The cosine-weighted average of sky_gradient_position over the upper hemisphere: how far towards
// the zenith color the light reaching an upward-facing surface is.
inline constexpr double sky_gradient_upper_average = 5.0 / 6.0;
// Equirectangular texture coordinates for a normalized direction: u runs around the horizon with
// -Z at its centre, v from straight up (0) to straight down (1). The rotation turns the image.
[[nodiscard]] std::array<double, 2> sky_panorama_uv(const Vec3& direction, double rotation_degrees);

// A decoded panorama ready for the renderer.
struct SkyPanorama {
    TextureAsset image; // RGBA8 in sRGB.
    // Cosine-weighted average linear color over the upper and lower hemispheres, before the
    // material's tint and intensity: the ambient light the image gives up- and downward surfaces.
    std::array<float, 3> upper{}, lower{};
    // Distinct for every load, so the renderer knows when to upload again.
    std::uint64_t revision{};
};

// Reads the averages from an RGBA8 sRGB equirectangular image.
void compute_sky_panorama_averages(SkyPanorama& panorama);

// A material as the renderer and the Inspector see it. On failure `panorama` is null and `error`
// says why; the sky then falls back to its gradient.
struct ResolvedSkyMaterial {
    std::string path;
    SkyMaterial material;
    std::shared_ptr<const SkyPanorama> panorama;
    std::string error;
};

// Keeps decoded sky materials, reloading one when its file or its image changes on disk.
class SkyMaterialCache {
public:
    [[nodiscard]] std::shared_ptr<const ResolvedSkyMaterial> resolve(const std::filesystem::path& root,
                                                                     std::string_view path);
    void clear() { entries_.clear(); }

private:
    struct Stamp {
        std::filesystem::file_time_type time{};
        std::uintmax_t size{};
        bool exists{};
        auto operator<=>(const Stamp&) const = default;
    };
    struct Entry {
        std::filesystem::path root;
        Stamp material, image;
        std::shared_ptr<const ResolvedSkyMaterial> resolved;
    };
    [[nodiscard]] static Stamp stamp(const std::optional<std::filesystem::path>& path);
    std::map<std::string, Entry, std::less<>> entries_;
};

} // namespace relay
