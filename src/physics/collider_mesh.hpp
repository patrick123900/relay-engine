#pragma once

// Internal geometry shared by collision queries, the Jolt world and the editor overlay.

#include "relay/physics/collision.hpp"
#include "relay/render/assets.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace relay::detail {

// A single convex or mesh collider may reference at most this many triangles. Larger meshes leave
// the collider inactive; author a simpler collision mesh instead.
inline constexpr std::size_t maximum_collider_triangles = 65536U;
// One Jolt world build accepts this many triangle-mesh triangles across all colliders.
inline constexpr std::size_t maximum_world_collider_triangles = 1048576U;

struct ColliderMeshData {
    // Collider-local positions, with the collider center already applied.
    std::vector<Vec3> points;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

// Resolves the collider mesh (or the entity renderer mesh when the collider names none) in the
// registry. Returns nothing for a missing, empty or oversized mesh.
[[nodiscard]] std::optional<ColliderMeshData> collider_mesh(const EntityRecord& record,
                                                            const AssetRegistry& assets);

// World-space box, sphere or capsule geometry of one enabled collider, as collision_debug_boxes
// reports it. Nothing for disabled, convex or mesh colliders, or degenerate transforms.
[[nodiscard]] std::optional<CollisionDebugBox> collider_geometry(const Scene& scene, Entity entity);

// World-space edges of the convex hull Jolt builds from these points, at most `limit` of them.
[[nodiscard]] std::vector<std::array<Vec3, 2>> convex_hull_edges(const std::vector<Vec3>& points,
                                                                 std::size_t limit,
                                                                 bool& truncated);

} // namespace relay::detail
