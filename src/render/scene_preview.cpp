#include "relay/render/scene_preview.hpp"

#include "relay/render/assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace relay {
namespace {

using Color = std::array<float, 3>;

struct Vec4 {
    float x{}, y{}, z{}, w{};
};

float srgb_to_linear(const float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

std::uint8_t linear_to_srgb8(const float value) {
    const float c = std::clamp(value, 0.0F, 1.0F);
    const float encoded = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(encoded * 255.0F));
}

// A soft shoulder so bright skies and strong lights do not clip to flat white.
float tone(const float value) { return value / (1.0F + std::max(value, 0.0F) * 0.5F) * 1.1F; }

Vec4 clip_point(const RenderMatrix& m, const float x, const float y, const float z) {
    const auto& v = m.values;
    return {v[0] * x + v[4] * y + v[8] * z + v[12], v[1] * x + v[5] * y + v[9] * z + v[13],
            v[2] * x + v[6] * y + v[10] * z + v[14], v[3] * x + v[7] * y + v[11] * z + v[15]};
}

struct Normal {
    float x{}, y{}, z{};
};

Normal world_normal(const RenderMatrix& m, const float x, const float y, const float z) {
    const auto& v = m.values;
    Normal n{v[0] * x + v[4] * y + v[8] * z, v[1] * x + v[5] * y + v[9] * z, v[2] * x + v[6] * y + v[10] * z};
    const float length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return length > 1e-12F ? Normal{n.x / length, n.y / length, n.z / length} : Normal{0.0F, 1.0F, 0.0F};
}

struct Corner {
    Vec4 clip;
    Normal normal;
    float u{}, v{};
};

struct Surface {
    Color color{1.0F, 1.0F, 1.0F};
    float alpha{1.0F};
    Color emissive{};
    float metallic{};
    const TextureAsset* texture{};
};

} // namespace

bool render_scene_preview(const Scene& scene, const AssetRegistry& assets, const ViewOverride& view,
                          const std::uint32_t width, const std::uint32_t height,
                          const ScenePreviewOptions& options, ScenePreviewFrame& output, std::string& error) {
    if (width < 8U || height < 8U || width > 4096U || height > 4096U) {
        error = "the preview size must be between 8 and 4096 pixels";
        return false;
    }
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const auto render = build_render_scene(scene, assets, aspect, &view, false, true);
    output.width = width;
    output.height = height;
    output.view = render.camera.view;
    output.projection = render.camera.projection;
    output.view_projection = render.camera.view_projection;

    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> depth(pixels, std::numeric_limits<float>::max());
    std::vector<Color> color(pixels, Color{});
    std::vector<std::uint8_t> marked(pixels, 0U);

    // Background: the sky along each pixel's ray, with a grid on the ground plane.
    const auto& proj = render.camera.projection.values;
    const auto& vw = render.camera.view.values;
    const Vec3 origin = render.camera_position;
    const auto background = [&](const float px, const float py) {
        const float ndc_x = (px + 0.5F) / static_cast<float>(width) * 2.0F - 1.0F;
        const float ndc_y = (py + 0.5F) / static_cast<float>(height) * 2.0F - 1.0F;
        // View-space direction, then to the world with the transpose of the view's rotation.
        const float dx = ndc_x / proj[0], dy = ndc_y / proj[5], dz = -1.0F;
        Vec3 dir{vw[0] * dx + vw[1] * dy + vw[2] * dz, vw[4] * dx + vw[5] * dy + vw[6] * dz,
                 vw[8] * dx + vw[9] * dy + vw[10] * dz};
        const double length = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (length > 1e-12) dir = {dir.x / length, dir.y / length, dir.z / length};
        Color sky{0.06F, 0.07F, 0.09F};
        if (options.sky) {
            if (render.sky.visible) {
                sky = sky_radiance(render.sky, dir);
            } else {
                const float t = static_cast<float>(std::clamp(dir.y * 0.5 + 0.5, 0.0, 1.0));
                sky = {0.10F + 0.10F * t, 0.12F + 0.17F * t, 0.15F + 0.35F * t};
            }
        }
        if (options.grid && dir.y < -1e-4 && origin.y > 0.0) {
            const double distance = -origin.y / dir.y;
            const double gx = origin.x + dir.x * distance, gz = origin.z + dir.z * distance;
            const auto line = [&](const double value) {
                const double nearest = std::abs(value - std::round(value));
                return static_cast<float>(1.0 - std::clamp(nearest * 12.0 / std::max(1.0, distance * 0.08), 0.0, 1.0));
            };
            const float fade = static_cast<float>(std::clamp(1.0 - distance / 120.0, 0.0, 1.0));
            float amount = std::max(line(gx), line(gz)) * 0.30F * fade;
            Color tint{0.75F, 0.78F, 0.85F};
            if (std::abs(gx) < 0.02 * std::max(1.0, distance * 0.08)) { amount = 0.6F * fade; tint = {0.35F, 0.55F, 1.0F}; }
            if (std::abs(gz) < 0.02 * std::max(1.0, distance * 0.08)) { amount = 0.6F * fade; tint = {1.0F, 0.35F, 0.3F}; }
            for (std::size_t c = 0; c < 3U; ++c) sky[c] = sky[c] * (1.0F - amount) + tint[c] * amount * 0.5F;
        }
        return sky;
    };
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            color[static_cast<std::size_t>(y) * width + x] =
                background(static_cast<float>(x), static_cast<float>(y));

    // Lights: the scene's own, or a key light from above the viewer when it has none, so a bare
    // template is not drawn dark.
    struct Source {
        Vec3 to_light;      // Directional: unit vector towards the light.
        Vec3 position;      // Point and spot.
        bool directional{};
        Color radiance{};
        std::array<double, 3> attenuation{0.0, 0.0, 1.0};
        double range{};
    };
    std::vector<Source> sources;
    bool has_directional = false;
    for (const auto& light : render.lights) {
        Source source;
        source.radiance = {static_cast<float>(light.light.color.x * light.light.intensity),
                           static_cast<float>(light.light.color.y * light.light.intensity),
                           static_cast<float>(light.light.color.z * light.light.intensity)};
        if (light.light.type == Light::Type::directional) {
            source.directional = true;
            has_directional = true;
            source.to_light = {-light.direction.x, -light.direction.y, -light.direction.z};
        } else {
            source.position = light.position;
            source.attenuation = {light.light.attenuation.x, light.light.attenuation.y, light.light.attenuation.z};
            source.range = light.light.range;
        }
        sources.push_back(source);
    }
    if (!has_directional) {
        const auto& f = render.camera_forward;
        Source key;
        key.directional = true;
        key.radiance = {0.85F, 0.83F, 0.78F};
        // Mostly from the viewer's side, raised and a little to the left.
        key.to_light = {-f.x * 0.55 - 0.35, 0.8, -f.z * 0.55 + 0.25};
        const double length = std::sqrt(key.to_light.x * key.to_light.x + key.to_light.y * key.to_light.y +
                                        key.to_light.z * key.to_light.z);
        key.to_light = {key.to_light.x / length, key.to_light.y / length, key.to_light.z / length};
        sources.push_back(key);
    }

    const auto vertices = assets.mesh_vertices();
    const auto indices = assets.mesh_indices();
    std::vector<Surface> surfaces;
    surfaces.reserve(render.instances.size());
    constexpr std::size_t maximum_triangles = 3'000'000U;
    std::size_t drawn = 0;

    const float w_pixels = static_cast<float>(width), h_pixels = static_cast<float>(height);
    const auto shade = [&](const Surface& surface, Normal n, const Vec3 position, const float u, const float v,
                           float& alpha) {
        Color base = surface.color;
        alpha = surface.alpha;
        if (const auto* texture = surface.texture) {
            const auto wrap = [](const float value, const std::uint32_t count) {
                const float fraction = value - std::floor(value);
                return std::min(static_cast<std::uint32_t>(fraction * static_cast<float>(count)), count - 1U);
            };
            const auto* texel = &texture->rgba[(static_cast<std::size_t>(wrap(v, texture->height)) * texture->width +
                                                wrap(u, texture->width)) * 4U];
            for (std::size_t c = 0; c < 3U; ++c) {
                const float sample = static_cast<float>(texel[c]) / 255.0F;
                base[c] *= texture->color_space == TextureColorSpace::srgb ? srgb_to_linear(sample) : sample;
            }
            alpha *= static_cast<float>(texel[3]) / 255.0F;
        }
        // Two-sided: light the side the viewer sees.
        const Vec3 to_eye{origin.x - position.x, origin.y - position.y, origin.z - position.z};
        if (n.x * to_eye.x + n.y * to_eye.y + n.z * to_eye.z < 0.0) n = {-n.x, -n.y, -n.z};
        const auto env = sky_environment(render.sky, {n.x, n.y, n.z});
        const float diffuse_share = 1.0F - surface.metallic;
        Color lit{};
        for (std::size_t c = 0; c < 3U; ++c) lit[c] = env[c] * 0.3183F * diffuse_share;
        const double eye_length = std::sqrt(to_eye.x * to_eye.x + to_eye.y * to_eye.y + to_eye.z * to_eye.z);
        const Vec3 eye = eye_length > 1e-9 ? Vec3{to_eye.x / eye_length, to_eye.y / eye_length, to_eye.z / eye_length}
                                           : Vec3{0, 0, 1};
        Color specular{};
        for (const auto& source : sources) {
            Vec3 l = source.to_light;
            double falloff = 1.0;
            if (!source.directional) {
                l = {source.position.x - position.x, source.position.y - position.y, source.position.z - position.z};
                const double d = std::sqrt(l.x * l.x + l.y * l.y + l.z * l.z);
                if (d < 1e-9 || (source.range > 0.0 && d > source.range)) continue;
                l = {l.x / d, l.y / d, l.z / d};
                falloff = 1.0 / std::max(source.attenuation[0] + source.attenuation[1] * d + source.attenuation[2] * d * d, 1e-3);
            }
            const float ndl = std::max(0.0F, static_cast<float>(n.x * l.x + n.y * l.y + n.z * l.z));
            if (ndl <= 0.0F) continue;
            for (std::size_t c = 0; c < 3U; ++c) lit[c] += source.radiance[c] * static_cast<float>(falloff) * ndl * 0.3183F * diffuse_share;
            const Vec3 h{l.x + eye.x, l.y + eye.y, l.z + eye.z};
            const double hl = std::sqrt(h.x * h.x + h.y * h.y + h.z * h.z);
            if (hl > 1e-9) {
                const float ndh = std::max(0.0F, static_cast<float>((n.x * h.x + n.y * h.y + n.z * h.z) / hl));
                const float highlight = std::pow(ndh, 48.0F) * (0.04F + 0.5F * surface.metallic) * ndl;
                for (std::size_t c = 0; c < 3U; ++c) specular[c] += source.radiance[c] * static_cast<float>(falloff) * highlight;
            }
        }
        Color result{};
        for (std::size_t c = 0; c < 3U; ++c)
            result[c] = base[c] * lit[c] * 3.14159F + specular[c] * (surface.metallic > 0.0F ? base[c] : 1.0F) +
                        surface.emissive[c];
        return result;
    };

    const auto highlighted = [&](Entity entity) {
        for (int depth = 0; entity.valid() && depth < 256; ++depth) {
            if (std::find(options.highlight.begin(), options.highlight.end(), entity) != options.highlight.end()) return true;
            const auto* record = scene.get(entity);
            if (!record) return false;
            entity = record->parent;
        }
        return false;
    };
    for (const auto& instance : render.instances) {
        const auto* mesh = assets.find_mesh(instance.mesh);
        if (!mesh) continue;
        const bool selected = !options.highlight.empty() && highlighted(instance.entity);
        auto& surface = surfaces.emplace_back();
        surface.color = {instance.color[0], instance.color[1], instance.color[2]};
        surface.alpha = instance.color[3];
        surface.emissive = {instance.emissive_metallic[0], instance.emissive_metallic[1], instance.emissive_metallic[2]};
        surface.metallic = std::clamp(instance.emissive_metallic[3], 0.0F, 1.0F);
        if (const auto* material = assets.find_material(instance.material)) {
            if (!material->texture.empty()) surface.texture = assets.find_texture(material->texture);
            if (surface.texture && surface.texture->rgba.size() !=
                                       static_cast<std::size_t>(surface.texture->width) * surface.texture->height * 4U)
                surface.texture = nullptr;
        }
        const bool blended = instance.alpha_blended;
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
        for (std::uint32_t corner = 0; corner + 2U < mesh->index_count && drawn < maximum_triangles; corner += 3U) {
            std::array<Corner, 3> triangle;
            bool valid = true;
            for (std::uint32_t k = 0; k < 3U; ++k) {
                const auto at = static_cast<std::size_t>(mesh->first_index) + corner + k;
                const auto* source = at < indices.size() ? vertex(indices[at]) : nullptr;
                if (!source) { valid = false; break; }
                triangle[k].clip = clip_point(instance.model_view_projection, source->x, source->y, source->z);
                triangle[k].normal = world_normal(instance.model, source->nx, source->ny, source->nz);
                triangle[k].u = source->u;
                triangle[k].v = source->v;
            }
            if (!valid) continue;
            ++drawn;
            // Clip against the near plane (0 <= z) in homogeneous space: up to two triangles result.
            std::array<Corner, 4> polygon;
            std::size_t count = 0;
            for (std::size_t k = 0; k < 3U; ++k) {
                const auto& a = triangle[k];
                const auto& b = triangle[(k + 1U) % 3U];
                const bool a_in = a.clip.z >= 0.0F, b_in = b.clip.z >= 0.0F;
                if (a_in && count < polygon.size()) polygon[count++] = a;
                if (a_in != b_in && count < polygon.size()) {
                    const float t = a.clip.z / (a.clip.z - b.clip.z);
                    const auto mix = [t](const float p, const float q) { return p + (q - p) * t; };
                    Corner c;
                    c.clip = {mix(a.clip.x, b.clip.x), mix(a.clip.y, b.clip.y), mix(a.clip.z, b.clip.z), mix(a.clip.w, b.clip.w)};
                    c.normal = {mix(a.normal.x, b.normal.x), mix(a.normal.y, b.normal.y), mix(a.normal.z, b.normal.z)};
                    c.u = mix(a.u, b.u);
                    c.v = mix(a.v, b.v);
                    polygon[count++] = c;
                }
            }
            for (std::size_t fan = 1; fan + 1U < count; ++fan) {
                const std::array<const Corner*, 3> tri{&polygon[0], &polygon[fan], &polygon[fan + 1U]};
                std::array<float, 3> sx{}, sy{}, sz{}, inverse_w{};
                bool behind = false;
                for (std::size_t k = 0; k < 3U; ++k) {
                    const float w = tri[k]->clip.w;
                    if (w <= 1e-8F) { behind = true; break; }
                    inverse_w[k] = 1.0F / w;
                    sx[k] = (tri[k]->clip.x * inverse_w[k] * 0.5F + 0.5F) * w_pixels;
                    sy[k] = (tri[k]->clip.y * inverse_w[k] * 0.5F + 0.5F) * h_pixels;
                    sz[k] = tri[k]->clip.z * inverse_w[k];
                }
                if (behind) continue;
                const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
                if (std::abs(area) < 1e-9F) continue;
                const int min_x = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]}))));
                const int max_x = std::min(static_cast<int>(width) - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
                const int min_y = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]}))));
                const int max_y = std::min(static_cast<int>(height) - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
                for (int y = min_y; y <= max_y; ++y)
                    for (int x = min_x; x <= max_x; ++x) {
                        const float px = static_cast<float>(x) + 0.5F, py = static_cast<float>(y) + 0.5F;
                        const float w0 = ((sx[1] - px) * (sy[2] - py) - (sy[1] - py) * (sx[2] - px)) / area;
                        const float w1 = ((sx[2] - px) * (sy[0] - py) - (sy[2] - py) * (sx[0] - px)) / area;
                        const float w2 = 1.0F - w0 - w1;
                        if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) continue;
                        const float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                        if (z < 0.0F || z > 1.0F) continue;
                        const auto at = static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
                        if (z >= depth[at]) continue;
                        // Perspective-correct weights.
                        const float p0 = w0 * inverse_w[0], p1 = w1 * inverse_w[1], p2 = w2 * inverse_w[2];
                        const float total = p0 + p1 + p2;
                        if (total <= 0.0F) continue;
                        const float q0 = p0 / total, q1 = p1 / total, q2 = p2 / total;
                        Normal n{q0 * tri[0]->normal.x + q1 * tri[1]->normal.x + q2 * tri[2]->normal.x,
                                 q0 * tri[0]->normal.y + q1 * tri[1]->normal.y + q2 * tri[2]->normal.y,
                                 q0 * tri[0]->normal.z + q1 * tri[1]->normal.z + q2 * tri[2]->normal.z};
                        const float nl = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                        if (nl > 1e-9F) n = {n.x / nl, n.y / nl, n.z / nl};
                        const float u = q0 * tri[0]->u + q1 * tri[1]->u + q2 * tri[2]->u;
                        const float v = q0 * tri[0]->v + q1 * tri[1]->v + q2 * tri[2]->v;
                        // The world position of the pixel, from the camera ray and the view depth.
                        const float view_depth = 1.0F / total;
                        const float ndc_x = (px / w_pixels) * 2.0F - 1.0F;
                        const float ndc_y = (py / h_pixels) * 2.0F - 1.0F;
                        const float dx = ndc_x / proj[0], dy = ndc_y / proj[5];
                        const Vec3 ray{vw[0] * dx + vw[1] * dy - vw[2], vw[4] * dx + vw[5] * dy - vw[6],
                                       vw[8] * dx + vw[9] * dy - vw[10]};
                        const Vec3 position{origin.x + ray.x * view_depth, origin.y + ray.y * view_depth,
                                            origin.z + ray.z * view_depth};
                        float alpha = 1.0F;
                        const auto lit = shade(surface, n, position, u, v, alpha);
                        if (alpha < 0.02F) continue;
                        if (blended || alpha < 0.999F) {
                            for (std::size_t c = 0; c < 3U; ++c) color[at][c] = color[at][c] * (1.0F - alpha) + lit[c] * alpha;
                        } else {
                            color[at] = lit;
                            depth[at] = z;
                            marked[at] = selected ? 1U : 0U;
                        }
                    }
            }
        }
    }

    output.rgba.assign(pixels * 4U, 255U);
    for (std::size_t i = 0; i < pixels; ++i)
        for (std::size_t c = 0; c < 3U; ++c) output.rgba[i * 4U + c] = linear_to_srgb8(tone(color[i][c]));
    // Selected geometry gets a light tint inside and an orange outline where it meets anything else.
    if (!options.highlight.empty()) {
        const auto at = [&](const std::int64_t x, const std::int64_t y) {
            return static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
        };
        const std::int64_t w = width, h = height;
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) {
                if (!marked[at(x, y)]) continue;
                bool edge = false;
                for (std::int64_t dy = -1; dy <= 1 && !edge; ++dy)
                    for (std::int64_t dx = -1; dx <= 1 && !edge; ++dx) {
                        const auto nx = x + dx * 2, ny = y + dy * 2;
                        edge = nx < 0 || ny < 0 || nx >= w || ny >= h || !marked[at(nx, ny)];
                    }
                auto* pixel = &output.rgba[at(x, y) * 4U];
                if (edge) {
                    pixel[0] = 255U; pixel[1] = 158U; pixel[2] = 26U;
                } else {
                    pixel[0] = static_cast<std::uint8_t>(pixel[0] + (255 - pixel[0]) / 10);
                    pixel[1] = static_cast<std::uint8_t>(pixel[1] + (158 - pixel[1]) / 10);
                    pixel[2] = static_cast<std::uint8_t>(pixel[2] + (26 - pixel[2]) / 10);
                }
            }
    }
    error.clear();
    return true;
}

} // namespace relay
