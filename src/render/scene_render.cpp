#include "relay/render/scene_render.hpp"
#include "relay/render/assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace relay {
namespace {

constexpr float pi = 3.14159265358979323846F;

RenderMatrix identity() {
    RenderMatrix result;
    result.values[0] = 1.0F;
    result.values[5] = 1.0F;
    result.values[10] = 1.0F;
    result.values[15] = 1.0F;
    return result;
}

float element(const RenderMatrix& matrix, const std::size_t row, const std::size_t column) {
    return matrix.values[column * 4U + row];
}

void set_element(RenderMatrix& matrix, const std::size_t row, const std::size_t column,
                 const float value) {
    matrix.values[column * 4U + row] = value;
}

RenderMatrix multiply(const RenderMatrix& left, const RenderMatrix& right) {
    RenderMatrix result;
    for (std::size_t row = 0; row < 4U; ++row) {
        for (std::size_t column = 0; column < 4U; ++column) {
            float value = 0.0F;
            for (std::size_t inner = 0; inner < 4U; ++inner) {
                value += element(left, row, inner) * element(right, inner, column);
            }
            set_element(result, row, column, value);
        }
    }
    return result;
}

RenderMatrix translation(const Vec3& value) {
    auto result = identity();
    result.values[12] = static_cast<float>(value.x);
    result.values[13] = static_cast<float>(value.y);
    result.values[14] = static_cast<float>(value.z);
    return result;
}

RenderMatrix scaling(const Vec3& value) {
    auto result = identity();
    result.values[0] = static_cast<float>(value.x);
    result.values[5] = static_cast<float>(value.y);
    result.values[10] = static_cast<float>(value.z);
    return result;
}

RenderMatrix rotation_x(const float radians) {
    auto result = identity();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.values[5] = cosine;
    result.values[6] = sine;
    result.values[9] = -sine;
    result.values[10] = cosine;
    return result;
}

RenderMatrix rotation_y(const float radians) {
    auto result = identity();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.values[0] = cosine;
    result.values[2] = -sine;
    result.values[8] = sine;
    result.values[10] = cosine;
    return result;
}

RenderMatrix rotation_z(const float radians) {
    auto result = identity();
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    result.values[0] = cosine;
    result.values[1] = sine;
    result.values[4] = -sine;
    result.values[5] = cosine;
    return result;
}

RenderMatrix local_matrix(const Transform& transform) {
    const auto radians = [](const double degrees) {
        return static_cast<float>(degrees) * pi / 180.0F;
    };
    const auto rotation = multiply(rotation_z(radians(transform.rotation_degrees.z)),
                                   multiply(rotation_y(radians(transform.rotation_degrees.y)),
                                            rotation_x(radians(transform.rotation_degrees.x))));
    return multiply(translation(transform.position), multiply(rotation, scaling(transform.scale)));
}

RenderMatrix inverse(RenderMatrix matrix) {
    std::array<std::array<float, 8>, 4> augmented{};
    for (std::size_t row = 0; row < 4U; ++row) {
        for (std::size_t column = 0; column < 4U; ++column) {
            augmented[row][column] = element(matrix, row, column);
        }
        augmented[row][row + 4U] = 1.0F;
    }
    for (std::size_t column = 0; column < 4U; ++column) {
        std::size_t pivot = column;
        for (std::size_t row = column + 1U; row < 4U; ++row) {
            if (std::abs(augmented[row][column]) > std::abs(augmented[pivot][column])) pivot = row;
        }
        if (std::abs(augmented[pivot][column]) < 1.0e-7F) return identity();
        std::swap(augmented[pivot], augmented[column]);
        const float divisor = augmented[column][column];
        for (float& value : augmented[column])
            value /= divisor;
        for (std::size_t row = 0; row < 4U; ++row) {
            if (row == column) continue;
            const float factor = augmented[row][column];
            for (std::size_t item = 0; item < 8U; ++item) {
                augmented[row][item] -= factor * augmented[column][item];
            }
        }
    }
    RenderMatrix result;
    for (std::size_t row = 0; row < 4U; ++row) {
        for (std::size_t column = 0; column < 4U; ++column) {
            set_element(result, row, column, augmented[row][column + 4U]);
        }
    }
    return result;
}

RenderMatrix perspective(const Camera& camera, const float aspect_ratio) {
    const float fov = static_cast<float>(camera.field_of_view_y_degrees) * pi / 180.0F;
    const float near_plane = static_cast<float>(camera.near_plane);
    const float far_plane = static_cast<float>(camera.far_plane);
    const float focal_length = 1.0F / std::tan(fov * 0.5F);
    RenderMatrix result;
    if (camera.orthographic_height > 0.0) {
        result = identity();
        const auto height = static_cast<float>(camera.orthographic_height);
        result.values[0] = 2.0F / (height * std::max(aspect_ratio, 0.001F));
        result.values[5] = -2.0F / height;
        result.values[10] = 1.0F / (near_plane - far_plane);
        result.values[14] = near_plane / (near_plane - far_plane);
        return result;
    }
    result.values[0] = focal_length / std::max(aspect_ratio, 0.001F);
    result.values[5] = -focal_length; // Vulkan's framebuffer Y axis points down.
    result.values[10] = far_plane / (near_plane - far_plane);
    result.values[11] = -1.0F;
    result.values[14] = near_plane * far_plane / (near_plane - far_plane);
    return result;
}

// Clip-space frustum planes from a model-view-projection matrix, using Vulkan's 0 <= z <= w depth
// range. Storage is column-major, so row i is (m[i], m[4+i], m[8+i], m[12+i]).
std::array<std::array<float, 4>, 6> frustum_planes(const RenderMatrix& mvp) {
    const auto& m = mvp.values;
    const auto row = [&m](const std::size_t index) {
        return std::array<float, 4>{m[index], m[4U + index], m[8U + index], m[12U + index]};
    };
    const auto add = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::array<float, 4>{a[0] + b[0], a[1] + b[1], a[2] + b[2], a[3] + b[3]};
    };
    const auto subtract = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::array<float, 4>{a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3]};
    };
    const auto r0 = row(0U);
    const auto r1 = row(1U);
    const auto r2 = row(2U);
    const auto r3 = row(3U);
    return {add(r3, r0), subtract(r3, r0), add(r3, r1), subtract(r3, r1), r2, subtract(r3, r2)};
}

// Rejects a local-space box only when it lies entirely outside a plane. Testing the box corner that
// maximizes each plane equation keeps this conservative: it never culls something still visible.
bool outside_frustum(const RenderMatrix& mvp, const std::array<float, 3>& low,
                     const std::array<float, 3>& high) {
    if (low[0] > high[0] || low[1] > high[1] || low[2] > high[2]) return false;
    for (const auto& plane : frustum_planes(mvp)) {
        const float best = plane[0] * (plane[0] >= 0.0F ? high[0] : low[0]) +
                           plane[1] * (plane[1] >= 0.0F ? high[1] : low[1]) +
                           plane[2] * (plane[2] >= 0.0F ? high[2] : low[2]) + plane[3];
        if (best < 0.0F) return true;
    }
    return false;
}

std::array<float, 4> entity_color(const Entity entity) {
    std::uint64_t hash = entity.packed() * 0x9E3779B97F4A7C15ULL;
    hash ^= hash >> 29U;
    const auto channel = [hash](const unsigned shift) {
        return 0.3F + static_cast<float>((hash >> shift) & 0xFFU) / 510.0F;
    };
    return {channel(0U), channel(8U), channel(16U), 1.0F};
}

template <class Key>
std::pair<std::size_t, float> key_interval(const std::vector<Key>& keys, double time) {
    if (keys.size() < 2U || time <= keys.front().time) return {0U, 0.0F};
    const auto upper = std::upper_bound(keys.begin(), keys.end(), time,
                                        [](double t, const Key& key) { return t < key.time; });
    if (upper == keys.end()) return {keys.size() - 1U, 0.0F};
    const auto index = static_cast<std::size_t>(upper - keys.begin() - 1);
    return {index, static_cast<float>((time - keys[index].time) /
                                      (keys[index + 1U].time - keys[index].time))};
}

double hermite(double x, double y, double out, double in, double a, double duration) {
    const auto a2 = a * a, a3 = a2 * a;
    return (2 * a3 - 3 * a2 + 1) * x + (a3 - 2 * a2 + a) * duration * out + (-2 * a3 + 3 * a2) * y +
           (a3 - a2) * duration * in;
}
Vec3 sample_vector(const std::vector<VectorKey>& keys, double time, Vec3 fallback,
                   AnimationInterpolation mode) {
    if (keys.empty()) return fallback;
    const auto [i, a] = key_interval(keys, time);
    const auto& x = keys[i].value;
    const auto& y = keys[std::min(i + 1U, keys.size() - 1U)].value;
    if (mode == AnimationInterpolation::step) return x;
    if (mode == AnimationInterpolation::cubic && i + 1U < keys.size()) {
        const auto duration = keys[i + 1U].time - keys[i].time;
        const auto& out = keys[i].out_tangent;
        const auto& in = keys[i + 1U].in_tangent;
        return {hermite(x.x, y.x, out.x, in.x, a, duration),
                hermite(x.y, y.y, out.y, in.y, a, duration),
                hermite(x.z, y.z, out.z, in.z, a, duration)};
    }
    return {x.x + (y.x - x.x) * a, x.y + (y.y - x.y) * a, x.z + (y.z - x.z) * a};
}

RenderMatrix animated_local(const Transform& base, const NodeTrack& track, double time) {
    auto transform = base;
    transform.position =
        sample_vector(track.positions, time, base.position, track.position_interpolation);
    transform.scale = sample_vector(track.scales, time, base.scale, track.scale_interpolation);
    if (track.rotations.empty()) return local_matrix(transform);
    const auto [i, alpha] = key_interval(track.rotations, time);
    auto q = track.rotations[i].value;
    auto target = track.rotations[std::min(i + 1U, track.rotations.size() - 1U)].value;
    const bool cubic = track.rotation_interpolation == AnimationInterpolation::cubic;
    double dot = 0.0;
    for (unsigned k = 0; k < 4; ++k)
        dot += q[k] * target[k];
    if (!cubic && dot < 0.0) {
        for (auto& v : target)
            v = -v;
        dot = -dot;
    }
    const auto blend = track.rotation_interpolation == AnimationInterpolation::step ? 0.0 : alpha;
    double a = 1.0 - blend, b = blend;
    if (!cubic && dot < 0.9995) {
        const double angle = std::acos(std::clamp(dot, -1.0, 1.0));
        a = std::sin((1.0 - blend) * angle) / std::sin(angle);
        b = std::sin(blend * angle) / std::sin(angle);
    }
    double length = 0.0;
    for (unsigned k = 0; k < 4; ++k) {
        q[k] = cubic && i + 1U < track.rotations.size()
                   ? hermite(q[k], target[k], track.rotations[i].out_tangent[k],
                             track.rotations[i + 1U].in_tangent[k], alpha,
                             track.rotations[i + 1U].time - track.rotations[i].time)
                   : a * q[k] + b * target[k];
        length += q[k] * q[k];
    }
    length = std::sqrt(length);
    if (length < 1e-12) return local_matrix(transform);
    for (auto& v : q)
        v /= length;
    const auto [x, y, z, w] = q;
    auto rotation = identity();
    rotation.values[0] = static_cast<float>(1 - 2 * (y * y + z * z));
    rotation.values[1] = static_cast<float>(2 * (x * y + w * z));
    rotation.values[2] = static_cast<float>(2 * (x * z - w * y));
    rotation.values[4] = static_cast<float>(2 * (x * y - w * z));
    rotation.values[5] = static_cast<float>(1 - 2 * (x * x + z * z));
    rotation.values[6] = static_cast<float>(2 * (y * z + w * x));
    rotation.values[8] = static_cast<float>(2 * (x * z + w * y));
    rotation.values[9] = static_cast<float>(2 * (y * z - w * x));
    rotation.values[10] = static_cast<float>(1 - 2 * (x * x + y * y));
    return multiply(translation(transform.position), multiply(rotation, scaling(transform.scale)));
}

Vec3 transform_point(const RenderMatrix& m, Vec3 p, bool direction = false) {
    const auto& v = m.values;
    return {v[0] * p.x + v[4] * p.y + v[8] * p.z + (direction ? 0.0 : v[12]),
            v[1] * p.x + v[5] * p.y + v[9] * p.z + (direction ? 0.0 : v[13]),
            v[2] * p.x + v[6] * p.y + v[10] * p.z + (direction ? 0.0 : v[14])};
}
Vec3 normalized(Vec3 v) {
    const auto length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length > 1e-12) {
        v.x /= length;
        v.y /= length;
        v.z /= length;
    }
    return v;
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Camera world matrix for an eye looking at a target. Relay cameras look down local -Z, matching
// the default viewpoint at +5Z and the direction convention used for spot and directional lights.
RenderMatrix look_at_world(const Vec3& eye, const Vec3& target) {
    const auto forward = normalized({target.x - eye.x, target.y - eye.y, target.z - eye.z});
    Vec3 up{0.0, 1.0, 0.0};
    // Looking straight up or down leaves the horizontal axis undefined; pick a stable fallback.
    if (std::abs(forward.y) > 0.9999) up = {0.0, 0.0, forward.y > 0.0 ? -1.0 : 1.0};
    const Vec3 backward{-forward.x, -forward.y, -forward.z};
    const auto right = normalized(cross(up, backward));
    const auto adjusted_up = cross(backward, right);

    auto result = identity();
    result.values[0] = static_cast<float>(right.x);
    result.values[1] = static_cast<float>(right.y);
    result.values[2] = static_cast<float>(right.z);
    result.values[4] = static_cast<float>(adjusted_up.x);
    result.values[5] = static_cast<float>(adjusted_up.y);
    result.values[6] = static_cast<float>(adjusted_up.z);
    result.values[8] = static_cast<float>(backward.x);
    result.values[9] = static_cast<float>(backward.y);
    result.values[10] = static_cast<float>(backward.z);
    result.values[12] = static_cast<float>(eye.x);
    result.values[13] = static_cast<float>(eye.y);
    result.values[14] = static_cast<float>(eye.z);
    return result;
}

// Resolves animation-aware world matrices, memoized per entity. Rendering, picking and bounds
// queries all go through this, so they cannot disagree about where an entity actually is.
class WorldResolver {
  public:
    WorldResolver(const Scene& scene, const AssetRegistry& assets)
        : scene_(scene), assets_(assets) {}

    RenderMatrix world(const Entity entity) {
        if (const auto found = cache_.find(entity.packed()); found != cache_.end()) {
            return found->second;
        }
        const auto* record = scene_.get(entity);
        auto result = record == nullptr ? identity() : local_matrix(record->transform);
        if (record != nullptr && record->model_node) {
            const auto [clip, time] = clip_for(*record->model_node);
            if (clip != nullptr) {
                for (const auto& track : clip->tracks) {
                    if (track.node == record->model_node->node) {
                        result = animated_local(record->transform, track, time);
                        break;
                    }
                }
            }
        }
        if (record != nullptr && record->parent.valid()) {
            result = multiply(world(record->parent), result);
        }
        cache_.emplace(entity.packed(), result);
        return result;
    }

    // Also used for morph tracks, which sample the same animator clip and time.
    [[nodiscard]] std::pair<const AnimationClip*, double> clip_for(const ModelNode& binding) const {
        const auto* root = scene_.get(binding.root);
        if (root == nullptr || !root->animator) return {nullptr, 0.0};
        const auto& animator = *root->animator;
        const auto* model = assets_.find_model(animator.model);
        if (model == nullptr || animator.clip >= model->clips.size()) return {nullptr, 0.0};
        return {&model->clips[animator.clip], animator.time_seconds};
    }

  private:
    const Scene& scene_;
    const AssetRegistry& assets_;
    std::unordered_map<std::uint64_t, RenderMatrix> cache_;
};

// Transforms a mesh's local axis-aligned bounds into world space by enclosing its eight corners.
void accumulate_world_bounds(const RenderMatrix& world, const std::array<float, 3>& low,
                             const std::array<float, 3>& high, Vec3& minimum, Vec3& maximum,
                             bool& any) {
    for (unsigned corner = 0; corner < 8U; ++corner) {
        const Vec3 local{(corner & 1U) != 0U ? high[0] : low[0],
                         (corner & 2U) != 0U ? high[1] : low[1],
                         (corner & 4U) != 0U ? high[2] : low[2]};
        const auto point = transform_point(world, local);
        if (!any) {
            minimum = point;
            maximum = point;
            any = true;
            continue;
        }
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }
}

// Slab test. Returns the nearest nonnegative hit distance along the ray, or a negative value when
// the ray misses.
double ray_box_distance(const Vec3& origin, const Vec3& direction, const Vec3& minimum,
                        const Vec3& maximum) {
    double near_hit = -std::numeric_limits<double>::infinity();
    double far_hit = std::numeric_limits<double>::infinity();
    for (unsigned axis = 0; axis < 3U; ++axis) {
        const double from = axis == 0 ? origin.x : axis == 1 ? origin.y : origin.z;
        const double along = axis == 0 ? direction.x : axis == 1 ? direction.y : direction.z;
        const double low = axis == 0 ? minimum.x : axis == 1 ? minimum.y : minimum.z;
        const double high = axis == 0 ? maximum.x : axis == 1 ? maximum.y : maximum.z;
        if (std::abs(along) < 1e-12) {
            if (from < low || from > high) return -1.0;
            continue;
        }
        double entry = (low - from) / along;
        double exit = (high - from) / along;
        if (entry > exit) std::swap(entry, exit);
        near_hit = std::max(near_hit, entry);
        far_hit = std::min(far_hit, exit);
        if (near_hit > far_hit) return -1.0;
    }
    if (far_hit < 0.0) return -1.0;
    return near_hit >= 0.0 ? near_hit : 0.0;
}

} // namespace

ScenePick pick_scene_entity(const Scene& scene, const AssetRegistry& assets, const Vec3& origin,
                            const Vec3& direction) {
    ScenePick result;
    const auto ray = normalized(direction);
    if (ray.x == 0.0 && ray.y == 0.0 && ray.z == 0.0) return result;
    const auto geometry = build_render_scene(scene, assets, 1.0F, nullptr, true);
    if (geometry.deformation_overflow) {
        result.error = "scene exceeds the deformation query budget";
        return result;
    }
    for (const auto& bounds : geometry.drawable_bounds) {
        const auto distance = ray_box_distance(origin, ray, bounds.minimum, bounds.maximum);
        if (distance < 0.0) continue;
        if (!result.hit || distance < result.distance) {
            result.hit = true;
            result.entity = bounds.entity;
            result.distance = distance;
        }
    }
    return result;
}

SceneBounds compute_scene_bounds(const Scene& scene, const AssetRegistry& assets,
                                 const Entity entity) {
    SceneBounds result;
    const auto* record = scene.get(entity);
    if (record == nullptr) return result;
    WorldResolver resolver(scene, assets);
    result.position = transform_point(resolver.world(entity), {});
    result.valid = true;

    const auto geometry = build_render_scene(scene, assets, 1.0F, nullptr, true);
    if (geometry.deformation_overflow) {
        result.valid = false;
        return result;
    }
    bool any = false;
    for (const auto& bounds : geometry.drawable_bounds) {
        bool descendant = bounds.entity == entity;
        for (auto walk = scene.get(bounds.entity); !descendant && walk && walk->parent.valid();
             walk = scene.get(walk->parent))
            if (walk->parent == entity) descendant = true;
        if (!descendant) continue;
        if (!any) {
            result.minimum = bounds.minimum;
            result.maximum = bounds.maximum;
            any = true;
        } else {
            result.minimum.x = std::min(result.minimum.x, bounds.minimum.x);
            result.minimum.y = std::min(result.minimum.y, bounds.minimum.y);
            result.minimum.z = std::min(result.minimum.z, bounds.minimum.z);
            result.maximum.x = std::max(result.maximum.x, bounds.maximum.x);
            result.maximum.y = std::max(result.maximum.y, bounds.maximum.y);
            result.maximum.z = std::max(result.maximum.z, bounds.maximum.z);
        }
    }
    result.has_geometry = any;
    if (!any) {
        result.minimum = result.position;
        result.maximum = result.position;
    }
    return result;
}

RenderScene build_render_scene(const Scene& scene, const AssetRegistry& assets,
                               const float aspect_ratio, const ViewOverride* const view,
                               const bool collect_bounds) {
    RenderScene output;
    const auto entities = scene.entities();
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint32_t, Entity>> model_nodes;
    for (const auto entity : entities) {
        if (const auto& binding = scene.get(entity)->model_node)
            model_nodes[binding->root.packed()][binding->node] = entity;
    }
    WorldResolver resolver(scene, assets);
    const auto resolve_world = [&resolver](const Entity entity) { return resolver.world(entity); };

    Camera selected_camera;
    RenderMatrix camera_world = translation({0.0, 0.0, 5.0});
    if (view != nullptr) {
        // An explicit viewpoint wins over the scene's active camera, and leaves camera.entity
        // invalid so callers can still tell that no scene camera is driving this frame.
        selected_camera = view->camera;
        camera_world = look_at_world(view->position, view->target);
    } else if (const auto active = scene.active_camera()) {
        output.camera.entity = *active;
        output.camera.using_default = false;
        selected_camera = *scene.get(*active)->camera;
        camera_world = resolve_world(*active);
    }
    output.camera.view_projection =
        multiply(perspective(selected_camera, aspect_ratio), inverse(camera_world));
    output.camera_position = transform_point(camera_world, {});
    for (const auto entity : entities)
        if (const auto& light = scene.get(entity)->light) {
            const auto world = resolve_world(entity);
            output.lights.push_back({*light, transform_point(world, {}),
                                     normalized(transform_point(world, {0, 0, -1}, true))});
        }

    std::size_t query_vertices = 0;
    output.instances.reserve(entities.size());
    for (const auto entity : entities) {
        const auto* record = scene.get(entity);
        if (record == nullptr || !record->mesh_renderer.has_value()) {
            continue;
        }
        const auto model = resolve_world(entity);
        const auto& mesh = record->mesh_renderer->mesh;
        const auto& material = record->mesh_renderer->material;
        const auto* mesh_asset = assets.find_mesh(mesh);
        const auto model_view_projection = multiply(output.camera.view_projection, model);
        std::vector<MeshVertex> deformed;
        auto low = mesh_asset ? mesh_asset->bounds_min : std::array<float, 3>{};
        auto high = mesh_asset ? mesh_asset->bounds_max : std::array<float, 3>{};
        if (mesh_asset && (!mesh_asset->skin.empty() || !mesh_asset->morph_targets.empty())) {
            if ((collect_bounds ? query_vertices : output.deformed_vertices.size()) +
                    mesh_asset->vertex_count >
                4'000'000U) {
                ++output.deformation_overflow;
                continue;
            }
            query_vertices += mesh_asset->vertex_count;
            const auto start = static_cast<std::size_t>(mesh_asset->vertex_offset);
            const auto vertices = assets.mesh_vertices();
            if (start + mesh_asset->vertex_count <= vertices.size()) {
                deformed.assign(vertices.begin() + start,
                                vertices.begin() + start + mesh_asset->vertex_count);
                const EntityRecord* binding_record = record;
                while (!binding_record->model_node && binding_record->parent.valid())
                    binding_record = scene.get(binding_record->parent);
                auto weights = record->mesh_renderer->morph_weights;
                if (weights.empty()) {
                    for (const auto& target : mesh_asset->morph_targets)
                        weights.push_back(target.weight);
                    if (binding_record->model_node) {
                        const auto [clip, time] = resolver.clip_for(*binding_record->model_node);
                        const auto suffix = mesh.find_last_of('.');
                        const auto mesh_index =
                            static_cast<std::uint32_t>(std::stoul(mesh.substr(suffix + 1U)));
                        if (clip)
                            for (const auto& track : clip->morph_tracks)
                                if (track.mesh == mesh_index && !track.keys.empty()) {
                                    const auto [i, a] = key_interval(track.keys, time);
                                    weights = track.keys[i].weights;
                                    const auto& next =
                                        track.keys[std::min(i + 1U, track.keys.size() - 1U)]
                                            .weights;
                                    for (std::size_t t = 0; t < weights.size(); ++t) {
                                        if (track.interpolation == AnimationInterpolation::cubic &&
                                            i + 1U < track.keys.size())
                                            weights[t] = hermite(
                                                weights[t], next[t], track.keys[i].out_tangent[t],
                                                track.keys[i + 1U].in_tangent[t], a,
                                                track.keys[i + 1U].time - track.keys[i].time);
                                        else if (track.interpolation !=
                                                 AnimationInterpolation::step)
                                            weights[t] += (next[t] - weights[t]) * a;
                                    }
                                }
                    }
                }
                for (std::size_t t = 0;
                     t < std::min(weights.size(), mesh_asset->morph_targets.size()); ++t) {
                    const auto& target = mesh_asset->morph_targets[t];
                    for (std::size_t v = 0; v < deformed.size(); ++v) {
                        auto& p = deformed[v];
                        const auto& d = target.deltas[v];
                        const float w = static_cast<float>(weights[t]);
                        p.x += w * d.x;
                        p.y += w * d.y;
                        p.z += w * d.z;
                        p.nx += w * d.nx;
                        p.ny += w * d.ny;
                        p.nz += w * d.nz;
                        p.tx += w * d.tx;
                        p.ty += w * d.ty;
                        p.tz += w * d.tz;
                    }
                }
                std::vector<RenderMatrix> joints;
                if (binding_record->model_node)
                    for (const auto& joint : mesh_asset->joints) {
                        const auto& nodes = model_nodes[binding_record->model_node->root.packed()];
                        const auto found = nodes.find(joint.node);
                        joints.push_back(
                            found == nodes.end()
                                ? identity()
                                : multiply(inverse(model),
                                           multiply(resolve_world(found->second),
                                                    RenderMatrix{joint.inverse_bind})));
                    }
                std::vector<RenderMatrix> normal_joints;
                for (const auto& joint : joints) {
                    const auto inverted = inverse(joint);
                    RenderMatrix normal;
                    for (unsigned r = 0; r < 4; ++r)
                        for (unsigned c = 0; c < 4; ++c)
                            normal.values[c * 4 + r] = inverted.values[r * 4 + c];
                    normal_joints.push_back(normal);
                }
                for (std::size_t v = 0; v < deformed.size(); ++v) {
                    auto& p = deformed[v];
                    if (!joints.empty() && v < mesh_asset->skin.size() &&
                        !mesh_asset->skin[v].empty()) {
                        Vec3 position{}, normal{}, tangent{};
                        for (const auto& weight : mesh_asset->skin[v]) {
                            const auto& matrix = joints[weight.joint];
                            const auto point = transform_point(matrix, {p.x, p.y, p.z});
                            const auto n = transform_point(normal_joints[weight.joint],
                                                           {p.nx, p.ny, p.nz}, true);
                            const auto t = transform_point(matrix, {p.tx, p.ty, p.tz}, true);
                            position.x += point.x * weight.weight;
                            position.y += point.y * weight.weight;
                            position.z += point.z * weight.weight;
                            normal.x += n.x * weight.weight;
                            normal.y += n.y * weight.weight;
                            normal.z += n.z * weight.weight;
                            tangent.x += t.x * weight.weight;
                            tangent.y += t.y * weight.weight;
                            tangent.z += t.z * weight.weight;
                        }
                        p.x = static_cast<float>(position.x);
                        p.y = static_cast<float>(position.y);
                        p.z = static_cast<float>(position.z);
                        normal = normalized(normal);
                        tangent = normalized(tangent);
                        p.nx = static_cast<float>(normal.x);
                        p.ny = static_cast<float>(normal.y);
                        p.nz = static_cast<float>(normal.z);
                        p.tx = static_cast<float>(tangent.x);
                        p.ty = static_cast<float>(tangent.y);
                        p.tz = static_cast<float>(tangent.z);
                    }
                    if (v == 0U) {
                        low = {p.x, p.y, p.z};
                        high = low;
                    } else
                        for (unsigned c = 0; c < 3; ++c) {
                            const float value = c == 0 ? p.x : c == 1 ? p.y : p.z;
                            low[c] = std::min(low[c], value);
                            high[c] = std::max(high[c], value);
                        }
                }
            }
        }
        if (collect_bounds) {
            if (mesh_asset) {
                DrawableBounds bounds{entity, {}, {}};
                bool any = false;
                accumulate_world_bounds(model, low, high, bounds.minimum, bounds.maximum, any);
                output.drawable_bounds.push_back(bounds);
            }
            continue;
        }
        if (mesh_asset != nullptr && outside_frustum(model_view_projection, low, high)) {
            ++output.culled;
            continue;
        }
        // The clip-space w of the bounds centre is its distance along the camera's view direction.
        float view_depth = 0.0F;
        if (mesh_asset != nullptr) {
            const auto& m = model_view_projection.values;
            const std::array<float, 3> centre{
                (mesh_asset->bounds_min[0] + mesh_asset->bounds_max[0]) * 0.5F,
                (mesh_asset->bounds_min[1] + mesh_asset->bounds_max[1]) * 0.5F,
                (mesh_asset->bounds_min[2] + mesh_asset->bounds_max[2]) * 0.5F};
            view_depth = m[3] * centre[0] + m[7] * centre[1] + m[11] * centre[2] + m[15];
        }
        const auto* material_asset = assets.find_material(material);
        const auto missing_texture = std::numeric_limits<std::uint32_t>::max();
        const auto texture_slot = [&](const std::string& name) {
            return !name.empty() && assets.find_texture(name) != nullptr
                       ? assets.texture_index(name)
                       : missing_texture;
        };
        RenderInstance instance;
        if (!deformed.empty()) {
            instance.deformed_vertex_offset =
                static_cast<std::int32_t>(output.deformed_vertices.size());
            output.deformed_vertices.insert(output.deformed_vertices.end(), deformed.begin(),
                                            deformed.end());
        }
        instance.entity = entity;
        instance.model = model;
        instance.model_view_projection = model_view_projection;
        instance.color = material_asset != nullptr ? material_asset->color : entity_color(entity);
        instance.mesh = mesh;
        instance.material = material;
        instance.texture_index =
            material_asset != nullptr ? texture_slot(material_asset->texture) : missing_texture;
        instance.material_index = material_asset != nullptr ? assets.material_index(material) : 0U;
        instance.view_depth = view_depth;
        if (material_asset != nullptr) {
            instance.emissive_metallic = {
                material_asset->emissive_factor[0], material_asset->emissive_factor[1],
                material_asset->emissive_factor[2], material_asset->metallic_factor};
            instance.surface_parameters = {
                material_asset->roughness_factor, material_asset->normal_scale,
                material_asset->occlusion_strength, material_asset->alpha_cutoff};
            const auto occlusion = texture_slot(material_asset->occlusion_texture);
            const auto emissive = texture_slot(material_asset->emissive_texture);
            const auto packed = (occlusion == missing_texture ? 0xFFU : occlusion) |
                                ((emissive == missing_texture ? 0xFFU : emissive) << 8U) |
                                (static_cast<std::uint32_t>(material_asset->alpha_mode) << 16U) |
                                (static_cast<std::uint32_t>(material_asset->double_sided) << 18U);
            instance.pbr_textures = {instance.texture_index,
                                     texture_slot(material_asset->metallic_roughness_texture),
                                     texture_slot(material_asset->normal_texture), packed};
        } else {
            instance.pbr_textures = {missing_texture, missing_texture, missing_texture,
                                     0x0000FFFFU};
        }
        output.instances.push_back(std::move(instance));
    }
    // Opaque geometry draws front to back so the depth test rejects hidden fragments early. The
    // remaining keys make the order total, so it never depends on entity creation order.
    std::sort(output.instances.begin(), output.instances.end(),
              [](const RenderInstance& a, const RenderInstance& b) {
                  if (a.view_depth != b.view_depth) return a.view_depth < b.view_depth;
                  if (a.mesh != b.mesh) return a.mesh < b.mesh;
                  if (a.material != b.material) return a.material < b.material;
                  return a.entity.packed() < b.entity.packed();
              });
    return output;
}

} // namespace relay
