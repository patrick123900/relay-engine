#include "relay/physics/collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace relay {
namespace {

constexpr std::size_t maximum_query_colliders = 4096U;
constexpr std::size_t maximum_hierarchy_depth = 4096U;
constexpr double pi = 3.14159265358979323846;

Vec3 add(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 subtract(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(const Vec3 a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 a, const Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
bool finite(const Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool bounded(const Vec3 value) {
    return finite(value) && std::abs(value.x) <= 1e12 &&
           std::abs(value.y) <= 1e12 && std::abs(value.z) <= 1e12;
}

struct Affine {
    Vec3 position{};
    std::array<Vec3, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
};

Vec3 transform_direction(const Affine& affine, const Vec3 value) {
    return add(add(scale(affine.axes[0], value.x), scale(affine.axes[1], value.y)),
               scale(affine.axes[2], value.z));
}

Vec3 rotate(const Vec3 vector, const Vec3 degrees) {
    const auto rx = degrees.x * pi / 180.0;
    const auto ry = degrees.y * pi / 180.0;
    const auto rz = degrees.z * pi / 180.0;
    const auto cx = std::cos(rx), sx = std::sin(rx);
    const auto cy = std::cos(ry), sy = std::sin(ry);
    const auto cz = std::cos(rz), sz = std::sin(rz);
    const Vec3 x{vector.x, cx * vector.y - sx * vector.z,
                 sx * vector.y + cx * vector.z};
    const Vec3 y{cy * x.x + sy * x.z, x.y, -sy * x.x + cy * x.z};
    return {cz * y.x - sz * y.y, sz * y.x + cz * y.y, y.z};
}

struct Shape {
    Entity entity{};
    BoxCollider collider{};
    Vec3 center{};
    std::array<Vec3, 3> edges{};
    Vec3 minimum{};
    Vec3 maximum{};
};

std::optional<Affine> affine_for(const Scene& scene, const Entity entity) {
    std::vector<const EntityRecord*> ancestors;
    for (auto current = entity; current.valid();) {
        const auto* node = scene.get(current);
        if (!node || ancestors.size() == maximum_hierarchy_depth) return std::nullopt;
        ancestors.push_back(node);
        current = node->parent;
    }
    Affine world;
    for (auto node = ancestors.rbegin(); node != ancestors.rend(); ++node) {
        const auto& source = **node;
        const auto transform = source.transform_animation &&
                                       !source.transform_animation->keys.empty()
                                   ? sample_transform_animation(*source.transform_animation,
                                                                source.transform)
                                   : source.transform;
        Affine next;
        next.position = add(world.position, transform_direction(world, transform.position));
        const std::array<Vec3, 3> basis{{
            {transform.scale.x, 0, 0}, {0, transform.scale.y, 0}, {0, 0, transform.scale.z}}};
        for (std::size_t i = 0; i < 3U; ++i)
            next.axes[i] = transform_direction(world, rotate(basis[i], transform.rotation_degrees));
        world = next;
    }
    return world;
}

std::optional<Shape> shape_for(const Scene& scene, const Entity entity,
                               const bool include_disabled = false) {
    const auto* record = scene.get(entity);
    if (!record || !record->collider || (!include_disabled && !record->collider->enabled))
        return std::nullopt;
    const auto affine = affine_for(scene, entity);
    if (!affine) return std::nullopt;
    Shape shape;
    shape.entity = entity;
    shape.collider = *record->collider;
    shape.center = add(affine->position, transform_direction(*affine, shape.collider.center));
    shape.edges = {{scale(affine->axes[0], shape.collider.half_extents.x),
                    scale(affine->axes[1], shape.collider.half_extents.y),
                    scale(affine->axes[2], shape.collider.half_extents.z)}};
    const auto radius = Vec3{
        std::abs(shape.edges[0].x) + std::abs(shape.edges[1].x) + std::abs(shape.edges[2].x),
        std::abs(shape.edges[0].y) + std::abs(shape.edges[1].y) + std::abs(shape.edges[2].y),
        std::abs(shape.edges[0].z) + std::abs(shape.edges[1].z) + std::abs(shape.edges[2].z)};
    shape.minimum = subtract(shape.center, radius);
    shape.maximum = add(shape.center, radius);
    if (!bounded(shape.center) || !bounded(shape.minimum) || !bounded(shape.maximum) ||
        !bounded(shape.edges[0]) || !bounded(shape.edges[1]) || !bounded(shape.edges[2]) ||
        std::abs(dot(shape.edges[0], cross(shape.edges[1], shape.edges[2]))) < 1e-30)
        return std::nullopt;
    return shape;
}

} // namespace

CollisionDebugBoxes collision_debug_boxes(const Scene& scene, bool enabled_only) {
    CollisionDebugBoxes result;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (!record || !record->collider || (enabled_only && !record->collider->enabled)) continue;
        if (result.boxes.size() == maximum_query_colliders) {
            result.truncated = true;
            break;
        }
        if (const auto shape = shape_for(scene, entity, true))
            result.boxes.push_back({entity, shape->collider.enabled, shape->center, shape->edges});
    }
    return result;
}

} // namespace relay
