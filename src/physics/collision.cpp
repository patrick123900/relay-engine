#include "relay/physics/collision.hpp"

#include "collider_mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace relay {
namespace {

constexpr std::size_t maximum_query_colliders = 4096U;
constexpr std::size_t maximum_hierarchy_depth = 4096U;
constexpr std::size_t maximum_outline_lines = 768U;
constexpr std::size_t maximum_total_outline_lines = 8192U;
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
    double radius{};
    Vec3 axis{};
    std::optional<detail::ColliderMeshData> mesh; // World-space points for convex and mesh.
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
                               const bool include_disabled = false,
                               const AssetRegistry* assets = nullptr) {
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
    if (shape.collider.type != BoxCollider::Type::box) {
        const auto length = [](Vec3 value) {
            return std::sqrt(dot(value, value));
        };
        const double world_scale = std::max({length(affine->axes[0]),
                                             length(affine->axes[1]),
                                             length(affine->axes[2])});
        const double radius = shape.collider.radius * world_scale;
        const double height = shape.collider.type == BoxCollider::Type::capsule
            ? shape.collider.half_height * world_scale : 0.0;
        const double y_length = length(affine->axes[1]);
        if (!std::isfinite(radius) || radius <= 0.0 || y_length <= 0.0) return std::nullopt;
        const auto axis = scale(affine->axes[1], 1.0 / y_length);
        shape.radius = radius;
        shape.axis = scale(axis, height);
        shape.edges = {{{radius + std::abs(axis.x) * height, 0, 0},
                        {0, radius + std::abs(axis.y) * height, 0},
                        {0, 0, radius + std::abs(axis.z) * height}}};
    }
    if (shape.collider.type == BoxCollider::Type::convex ||
        shape.collider.type == BoxCollider::Type::mesh) {
        if (!assets) return std::nullopt;
        auto mesh = detail::collider_mesh(*record, *assets);
        if (!mesh) return std::nullopt;
        Vec3 low{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                 std::numeric_limits<double>::infinity()};
        Vec3 high = scale(low, -1.0);
        for (auto& point : mesh->points) {
            point = add(affine->position, transform_direction(*affine, point));
            low = {std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
            high = {std::max(high.x, point.x), std::max(high.y, point.y),
                    std::max(high.z, point.z)};
        }
        // Flat meshes are valid triangle colliders; keep their bounds visibly non-degenerate.
        const auto half = [](const double a, const double b) {
            return std::max((b - a) * 0.5, 0.0005);
        };
        shape.center = scale(add(low, high), 0.5);
        shape.edges = {{{half(low.x, high.x), 0, 0}, {0, half(low.y, high.y), 0},
                        {0, 0, half(low.z, high.z)}}};
        shape.mesh = std::move(mesh);
    }
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

std::optional<detail::ColliderMeshData> detail::collider_mesh(const EntityRecord& record,
                                                              const AssetRegistry& assets) {
    if (!record.collider) return std::nullopt;
    const auto& name = !record.collider->mesh.empty() ? record.collider->mesh :
                       record.mesh_renderer ? record.mesh_renderer->mesh : std::string{};
    const auto* mesh = name.empty() ? nullptr : assets.find_mesh(name);
    if (!mesh || mesh->index_count < 3U ||
        mesh->index_count / 3U > maximum_collider_triangles) return std::nullopt;
    const auto vertices = assets.mesh_vertices();
    const auto indices = assets.mesh_indices();
    if (static_cast<std::size_t>(mesh->first_index) + mesh->index_count > indices.size())
        return std::nullopt;
    ColliderMeshData result;
    std::vector<std::uint32_t> remap;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> used;
    used.reserve(mesh->index_count);
    for (std::uint32_t i = 0; i < mesh->index_count; ++i)
        used.emplace_back(indices[mesh->first_index + i], i);
    std::sort(used.begin(), used.end());
    remap.resize(mesh->index_count);
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (i == 0 || used[i].first != used[i - 1].first) {
            const auto vertex = static_cast<std::int64_t>(mesh->vertex_offset) + used[i].first;
            if (vertex < 0 || static_cast<std::size_t>(vertex) >= vertices.size())
                return std::nullopt;
            const auto& source = vertices[static_cast<std::size_t>(vertex)];
            const Vec3 point = add({source.x, source.y, source.z}, record.collider->center);
            if (!bounded(point)) return std::nullopt;
            result.points.push_back(point);
        }
        remap[used[i].second] = static_cast<std::uint32_t>(result.points.size() - 1U);
    }
    for (std::uint32_t i = 0; i + 2U < mesh->index_count; i += 3U)
        result.triangles.push_back({remap[i], remap[i + 1U], remap[i + 2U]});
    return result;
}

CollisionDebugBoxes collision_debug_boxes(const Scene& scene, bool enabled_only,
                                          const AssetRegistry* assets) {
    CollisionDebugBoxes result;
    std::size_t line_budget = maximum_total_outline_lines;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (!record || !record->collider || (enabled_only && !record->collider->enabled)) continue;
        if (result.boxes.size() == maximum_query_colliders) {
            result.truncated = true;
            break;
        }
        const auto shape = shape_for(scene, entity, true, assets);
        if (!shape) continue;
        CollisionDebugBox box{entity, shape->collider.enabled, shape->collider.type,
                              shape->center, shape->edges, shape->radius, shape->axis, {}, false};
        if (shape->mesh) {
            const auto limit = std::min(maximum_outline_lines, line_budget);
            const auto& points = shape->mesh->points;
            if (shape->collider.type == BoxCollider::Type::convex) {
                box.lines = detail::convex_hull_edges(points, limit, box.lines_truncated);
            } else {
                std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
                for (const auto& triangle : shape->mesh->triangles) {
                    for (std::size_t corner = 0; corner < 3U; ++corner) {
                        const auto edge = std::minmax(triangle[corner], triangle[(corner + 1U) % 3U]);
                        if (edge.first == edge.second || !seen.insert(edge).second) continue;
                        if (box.lines.size() == limit) {
                            box.lines_truncated = true;
                            break;
                        }
                        box.lines.push_back({points[edge.first], points[edge.second]});
                    }
                    if (box.lines_truncated) break;
                }
            }
            line_budget -= box.lines.size();
        }
        result.boxes.push_back(std::move(box));
    }
    return result;
}

} // namespace relay
