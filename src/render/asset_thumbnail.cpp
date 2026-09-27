#include "relay/render/asset_thumbnail.hpp"

#include "relay/render/assets.hpp"
#include "relay/render/image_decode.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>

namespace relay {
namespace {

constexpr std::uintmax_t maximum_thumbnail_source = 128U * 1024U * 1024U;

float srgb_to_linear(const float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

std::uint8_t linear_to_srgb8(const float value) {
    const float c = std::clamp(value, 0.0F, 1.0F);
    const float encoded = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0F));
}

// Averages every source pixel under each destination pixel, so shrinking by large factors keeps
// fine detail as tone rather than aliasing.
ThumbnailImage shrink(const std::vector<std::uint8_t>& rgba, const std::uint32_t width,
                      const std::uint32_t height, const std::uint32_t size) {
    ThumbnailImage output;
    const double scale = std::min(1.0, static_cast<double>(size) / std::max(width, height));
    output.width = std::max(1U, static_cast<std::uint32_t>(std::lround(width * scale)));
    output.height = std::max(1U, static_cast<std::uint32_t>(std::lround(height * scale)));
    output.rgba.resize(static_cast<std::size_t>(output.width) * output.height * 4U);
    for (std::uint32_t y = 0; y < output.height; ++y) {
        const auto top = static_cast<std::uint32_t>(static_cast<std::uint64_t>(y) * height / output.height);
        const auto bottom = std::max(top + 1U, static_cast<std::uint32_t>(
                                                   static_cast<std::uint64_t>(y + 1U) * height / output.height));
        for (std::uint32_t x = 0; x < output.width; ++x) {
            const auto left = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * width / output.width);
            const auto right = std::max(left + 1U, static_cast<std::uint32_t>(
                                                       static_cast<std::uint64_t>(x + 1U) * width / output.width));
            std::array<std::uint64_t, 4> sum{};
            for (auto sy = top; sy < bottom; ++sy)
                for (auto sx = left; sx < right; ++sx)
                    for (std::size_t channel = 0; channel < 4U; ++channel)
                        sum[channel] += rgba[(static_cast<std::size_t>(sy) * width + sx) * 4U + channel];
            const auto count = static_cast<std::uint64_t>(bottom - top) * (right - left);
            for (std::size_t channel = 0; channel < 4U; ++channel)
                output.rgba[(static_cast<std::size_t>(y) * output.width + x) * 4U + channel] =
                    static_cast<std::uint8_t>((sum[channel] + count / 2U) / count);
        }
    }
    return output;
}

struct Point {
    float x{}, y{}, z{};
};

Point transform_point(const RenderMatrix& matrix, const float x, const float y, const float z) {
    const auto& m = matrix.values;
    return {m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14]};
}

Point transform_direction(const RenderMatrix& matrix, const float x, const float y, const float z) {
    const auto& m = matrix.values;
    return {m[0] * x + m[4] * y + m[8] * z, m[1] * x + m[5] * y + m[9] * z, m[2] * x + m[6] * y + m[10] * z};
}

Point normalized(const Point p) {
    const float length = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return length > 1e-12F ? Point{p.x / length, p.y / length, p.z / length} : Point{0.0F, 0.0F, 1.0F};
}

struct ShadedVertex {
    Point screen;  // x, y in pixels; z towards the viewer.
    Point normal;  // View space.
    float u{}, v{};
};

struct Surface {
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};  // Linear.
    const TextureAsset* texture{};
};

} // namespace

bool image_thumbnail(const std::filesystem::path& root, const std::string_view filename, const std::uint32_t size,
                     ThumbnailImage& output, std::string& error) {
    const auto path = workspace_file(root.generic_string(), filename, "");
    if (!path) {
        error = "not a project file";
        return false;
    }
    std::error_code failure;
    const auto bytes = std::filesystem::file_size(*path, failure);
    if (failure || bytes > maximum_thumbnail_source) {
        error = "image is missing or larger than 128 MiB";
        return false;
    }
    std::ifstream input(*path, std::ios::binary);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(bytes));
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        error = "image cannot be read";
        return false;
    }
    TextureAsset image;
    if (!decode_image_rgba(data, path->extension().string(), image, error)) return false;
    output = shrink(image.rgba, image.width, image.height, std::max(size, 1U));
    return true;
}

bool model_thumbnail(const std::filesystem::path& root, const std::string_view filename, const std::uint32_t size,
                     ThumbnailImage& output, std::string& error, const std::string_view only_mesh) {
    AssetRegistry registry;
    Scene scene;
    ModelImportSettings settings;
    settings.preset = "static_mesh";
    const auto imported = import_model_asset(root, filename, registry, &scene, error, settings);
    if (!imported.imported) {
        if (error.empty()) error = "model cannot be imported";
        return false;
    }
    const auto render = build_render_scene(scene, registry, 1.0F, nullptr, false, true);
    const auto vertices = registry.mesh_vertices();
    const auto indices = registry.mesh_indices();

    // A three-quarter view from the front right and above: yaw about Y, then pitch about X.
    constexpr float yaw = -0.62F, pitch = 0.42F;
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const auto to_view = [&](const Point p) {
        const float x = cy * p.x + sy * p.z, z = -sy * p.x + cy * p.z;
        return Point{x, cp * p.y - sp * z, sp * p.y + cp * z};
    };

    struct Triangle {
        std::array<ShadedVertex, 3> corners;
        const Surface* surface{};
    };
    std::vector<Surface> surfaces;
    surfaces.reserve(render.instances.size());
    std::vector<Triangle> triangles;
    Point low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), 0.0F};
    Point high{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), 0.0F};
    constexpr std::size_t maximum_triangles = 2'000'000U;
    // Imported names end in ".mesh.<index in the file>"; the content id before it depends on the
    // import preset, so a mesh is found by its index.
    const auto mesh_suffix = [](const std::string_view name) {
        const auto at = name.rfind(".mesh.");
        return at == std::string_view::npos ? std::string_view{} : name.substr(at);
    };
    const auto wanted = mesh_suffix(only_mesh);
    if (!only_mesh.empty() && wanted.empty()) {
        error = "not an imported mesh";
        return false;
    }
    bool drawn_mesh = false;
    for (const auto& instance : render.instances) {
        const auto* mesh = registry.find_mesh(instance.mesh);
        if (!mesh) continue;
        if (!only_mesh.empty()) {
            if (mesh_suffix(instance.mesh) != wanted || drawn_mesh) continue;
            drawn_mesh = true;
        }
        auto& surface = surfaces.emplace_back();
        if (const auto* material = registry.find_material(instance.material)) {
            surface.color = material->color;
            if (!material->texture.empty()) surface.texture = registry.find_texture(material->texture);
            if (surface.texture && surface.texture->rgba.size() !=
                                       static_cast<std::size_t>(surface.texture->width) * surface.texture->height * 4U)
                surface.texture = nullptr;
        } else {
            surface.color = {0.8F, 0.8F, 0.8F, 1.0F};
        }
        const bool deformed = instance.deformed_vertex_offset >= 0 &&
                              static_cast<std::size_t>(instance.deformed_vertex_offset) + mesh->vertex_count <=
                                  render.deformed_vertices.size();
        const auto vertex = [&](const std::uint32_t index) -> const MeshVertex* {
            if (deformed) {
                if (index >= mesh->vertex_count) return nullptr;
                return &render.deformed_vertices[static_cast<std::size_t>(instance.deformed_vertex_offset) + index];
            }
            const auto at = static_cast<std::size_t>(static_cast<std::int64_t>(mesh->vertex_offset) + index);
            return at < vertices.size() ? &vertices[at] : nullptr;
        };
        for (std::uint32_t corner = 0; corner + 2U < mesh->index_count && triangles.size() < maximum_triangles;
             corner += 3U) {
            Triangle triangle{{}, &surface};
            bool valid = true;
            for (std::uint32_t k = 0; k < 3U && valid; ++k) {
                const auto at = static_cast<std::size_t>(mesh->first_index) + corner + k;
                const auto* source = at < indices.size() ? vertex(indices[at]) : nullptr;
                if (!source) {
                    valid = false;
                    break;
                }
                auto& shaded = triangle.corners[k];
                shaded.screen = to_view(transform_point(instance.model, source->x, source->y, source->z));
                shaded.normal = normalized(to_view(transform_direction(instance.model, source->nx, source->ny, source->nz)));
                shaded.u = source->u;
                shaded.v = source->v;
                low = {std::min(low.x, shaded.screen.x), std::min(low.y, shaded.screen.y), 0.0F};
                high = {std::max(high.x, shaded.screen.x), std::max(high.y, shaded.screen.y), 0.0F};
            }
            if (valid) triangles.push_back(triangle);
        }
    }
    if (triangles.empty()) {
        error = only_mesh.empty() ? "model has no triangles to draw" : "no node of the model draws that mesh";
        return false;
    }

    // Drawn at twice the size and averaged down, for smooth edges.
    const std::uint32_t side = std::max(size, 8U) * 2U;
    const float extent = std::max({high.x - low.x, high.y - low.y, 1e-6F});
    const float scale = static_cast<float>(side) * 0.84F / extent;
    const float centre_x = (low.x + high.x) * 0.5F, centre_y = (low.y + high.y) * 0.5F;
    for (auto& triangle : triangles)
        for (auto& corner : triangle.corners) {
            corner.screen.x = (corner.screen.x - centre_x) * scale + static_cast<float>(side) * 0.5F;
            corner.screen.y = static_cast<float>(side) * 0.5F - (corner.screen.y - centre_y) * scale;
        }

    std::vector<float> depth(static_cast<std::size_t>(side) * side, std::numeric_limits<float>::lowest());
    std::vector<std::array<float, 4>> color(static_cast<std::size_t>(side) * side, {0.0F, 0.0F, 0.0F, 0.0F});
    const Point light = normalized({-0.45F, 0.75F, 0.5F});
    for (const auto& triangle : triangles) {
        const auto& a = triangle.corners[0].screen;
        const auto& b = triangle.corners[1].screen;
        const auto& c = triangle.corners[2].screen;
        const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::abs(area) < 1e-9F) continue;
        const auto min_x = static_cast<int>(std::max(0.0F, std::floor(std::min({a.x, b.x, c.x}))));
        const auto max_x = static_cast<int>(std::min(static_cast<float>(side - 1U), std::ceil(std::max({a.x, b.x, c.x}))));
        const auto min_y = static_cast<int>(std::max(0.0F, std::floor(std::min({a.y, b.y, c.y}))));
        const auto max_y = static_cast<int>(std::min(static_cast<float>(side - 1U), std::ceil(std::max({a.y, b.y, c.y}))));
        const auto& surface = *triangle.surface;
        for (int y = min_y; y <= max_y; ++y)
            for (int x = min_x; x <= max_x; ++x) {
                const float px = static_cast<float>(x) + 0.5F, py = static_cast<float>(y) + 0.5F;
                const float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                const float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                const float w2 = 1.0F - w0 - w1;
                if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) continue;
                const float z = w0 * a.z + w1 * b.z + w2 * c.z;
                const auto pixel = static_cast<std::size_t>(y) * side + static_cast<std::size_t>(x);
                if (z <= depth[pixel]) continue;
                depth[pixel] = z;
                const auto& n0 = triangle.corners[0].normal;
                const auto& n1 = triangle.corners[1].normal;
                const auto& n2 = triangle.corners[2].normal;
                auto normal = normalized({w0 * n0.x + w1 * n1.x + w2 * n2.x, w0 * n0.y + w1 * n1.y + w2 * n2.y,
                                          w0 * n0.z + w1 * n1.z + w2 * n2.z});
                // Seen from behind (thin or inverted surfaces), light the side the viewer sees.
                if (normal.z < 0.0F) normal = {-normal.x, -normal.y, -normal.z};
                std::array<float, 4> base = surface.color;
                if (const auto* texture = surface.texture) {
                    const float u = triangle.corners[0].u * w0 + triangle.corners[1].u * w1 + triangle.corners[2].u * w2;
                    const float v = triangle.corners[0].v * w0 + triangle.corners[1].v * w1 + triangle.corners[2].v * w2;
                    const auto wrap = [](const float value, const std::uint32_t count) {
                        const float fraction = value - std::floor(value);
                        return std::min(static_cast<std::uint32_t>(fraction * static_cast<float>(count)), count - 1U);
                    };
                    const auto* texel = &texture->rgba[(static_cast<std::size_t>(wrap(v, texture->height)) * texture->width +
                                                        wrap(u, texture->width)) * 4U];
                    for (std::size_t channel = 0; channel < 3U; ++channel) {
                        const float sample = static_cast<float>(texel[channel]) / 255.0F;
                        base[channel] *= texture->color_space == TextureColorSpace::srgb ? srgb_to_linear(sample) : sample;
                    }
                    base[3] *= static_cast<float>(texel[3]) / 255.0F;
                }
                const float diffuse = std::max(0.0F, normal.x * light.x + normal.y * light.y + normal.z * light.z);
                const float sky = 0.5F + 0.5F * normal.y;
                const float rim = std::pow(1.0F - std::clamp(normal.z, 0.0F, 1.0F), 3.0F) * 0.25F;
                const float lit = 0.18F + 0.22F * sky + 0.85F * diffuse;
                for (std::size_t channel = 0; channel < 3U; ++channel) color[pixel][channel] = base[channel] * lit + rim;
                color[pixel][3] = std::max(base[3], 0.35F);
            }
    }

    output.width = output.height = side / 2U;
    output.rgba.assign(static_cast<std::size_t>(output.width) * output.height * 4U, 0U);
    for (std::uint32_t y = 0; y < output.height; ++y)
        for (std::uint32_t x = 0; x < output.width; ++x) {
            std::array<float, 4> sum{};
            for (std::uint32_t row = 0; row < 2U; ++row)
                for (std::uint32_t column = 0; column < 2U; ++column) {
                    const auto& sample = color[static_cast<std::size_t>(y * 2U + row) * side + x * 2U + column];
                    // Premultiplied while averaging, so empty samples do not darken edges.
                    for (std::size_t channel = 0; channel < 3U; ++channel) sum[channel] += sample[channel] * sample[3];
                    sum[3] += sample[3];
                }
            auto* pixel = &output.rgba[(static_cast<std::size_t>(y) * output.width + x) * 4U];
            for (std::size_t channel = 0; channel < 3U; ++channel)
                pixel[channel] = sum[3] > 0.0F ? linear_to_srgb8(sum[channel] / sum[3]) : 0U;
            pixel[3] = static_cast<std::uint8_t>(std::lround(std::clamp(sum[3] / 4.0F, 0.0F, 1.0F) * 255.0F));
        }
    return true;
}

} // namespace relay
