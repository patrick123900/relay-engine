#include "relay/render/scene_render.hpp"
#include "relay/render/assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
        for (float& value : augmented[column]) value /= divisor;
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

} // namespace

RenderScene build_render_scene(const Scene& scene, const AssetRegistry& assets,
                               const float aspect_ratio) {
    RenderScene output;
    const auto entities = scene.entities();
    std::unordered_map<std::uint64_t, RenderMatrix> world_matrices;
    world_matrices.reserve(entities.size());

    const auto resolve_world = [&](const auto& self, const Entity entity) -> RenderMatrix {
        if (const auto found = world_matrices.find(entity.packed()); found != world_matrices.end()) {
            return found->second;
        }
        const auto* record = scene.get(entity);
        auto world = record == nullptr ? identity() : local_matrix(record->transform);
        if (record != nullptr && record->parent.valid()) {
            world = multiply(self(self, record->parent), world);
        }
        world_matrices.emplace(entity.packed(), world);
        return world;
    };

    Camera selected_camera;
    RenderMatrix camera_world = translation({0.0, 0.0, 5.0});
    if (const auto active = scene.active_camera()) {
        output.camera.entity = *active;
        output.camera.using_default = false;
        selected_camera = *scene.get(*active)->camera;
        camera_world = resolve_world(resolve_world, *active);
    }
    output.camera.view_projection = multiply(perspective(selected_camera, aspect_ratio),
                                             inverse(camera_world));

    output.instances.reserve(entities.size());
    for (const auto entity : entities) {
        const auto* record = scene.get(entity);
        if (record == nullptr || record->camera.has_value() || !record->mesh_renderer.has_value()) {
            continue;
        }
        const auto model = resolve_world(resolve_world, entity);
        const auto& mesh = record->mesh_renderer->mesh;
        const auto& material = record->mesh_renderer->material;
        const auto* mesh_asset = assets.find_mesh(mesh);
        const auto model_view_projection = multiply(output.camera.view_projection, model);
        if (mesh_asset != nullptr &&
            outside_frustum(model_view_projection, mesh_asset->bounds_min, mesh_asset->bounds_max)) {
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
        output.instances.push_back({entity, model, model_view_projection,
                                    material_asset != nullptr ? material_asset->color : entity_color(entity),
                                    mesh, material,
                                    material_asset != nullptr
                                        ? assets.texture_index(material_asset->texture) : 0U,
                                    view_depth});
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
