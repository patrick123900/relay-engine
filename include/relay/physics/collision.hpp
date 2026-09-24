#pragma once

#include "relay/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace relay {

class AssetRegistry;

struct CollisionRaycast {
    std::string error;
    bool hit{false};
    Entity entity{};
    double distance{};
    Vec3 point{};
    Vec3 normal{};
};

struct CollisionOverlaps {
    std::string error;
    std::vector<Entity> entities;
    bool truncated{false};
};

// One collider outline for the editor overlay. `center` and `edges` always bound the shape.
struct CollisionDebugBox {
    Entity entity{};
    bool enabled{};
    BoxCollider::Type type{BoxCollider::Type::box};
    Vec3 center{};
    std::array<Vec3, 3> edges{}; // World-space half edges, including parent transforms.
    double radius{};             // Sphere and capsule world radius.
    Vec3 axis{};                 // Capsule world half segment between the two end centers.
    std::vector<std::array<Vec3, 2>> lines; // Convex hull or triangle-mesh edges.
    bool lines_truncated{};
};

struct CollisionDebugBoxes {
    std::vector<CollisionDebugBox> boxes;
    bool truncated{};
};

struct ContactEvent {
    std::uint64_t sequence{};
    Entity first{};
    Entity second{};
    bool began{};
};

struct ContactEvents {
    std::vector<ContactEvent> events;
    std::uint64_t latest_sequence{};
    std::uint64_t oldest_sequence{};
};

// Queries use Jolt shapes built from authored colliders and current scene transforms. Convex and
// mesh colliders need the asset registry that owns their meshes; without it they are ignored.
[[nodiscard]] CollisionRaycast collision_raycast(const Scene& scene, Vec3 origin, Vec3 direction,
                                                 double maximum_distance,
                                                 std::uint32_t layer_mask = 0xffffffffU,
                                                 const AssetRegistry* assets = nullptr);
[[nodiscard]] CollisionOverlaps collision_overlaps(const Scene& scene, Entity entity,
                                                   const AssetRegistry* assets = nullptr);
// Outline line lists are bounded per collider and per call; truncated outlines report it.
[[nodiscard]] CollisionDebugBoxes collision_debug_boxes(const Scene& scene,
                                                        bool enabled_only = false,
                                                        const AssetRegistry* assets = nullptr);

// Jolt-backed game world. Authored scene data and runtime body state have separate lifetimes.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    void reset();
    // Registry for convex and mesh collider geometry. It must outlive the world.
    void set_assets(const AssetRegistry* assets);
    void step(Scene& scene, double fixed_delta_seconds);
    [[nodiscard]] std::optional<Vec3> velocity(const Scene& scene, Entity entity) const;
    [[nodiscard]] std::optional<Vec3> angular_velocity(const Scene& scene, Entity entity) const;
    [[nodiscard]] ContactEvents contact_events(std::uint64_t after = 0) const;
    [[nodiscard]] bool apply_impulse(const Scene& scene, Entity entity, Vec3 impulse,
                                     std::optional<Vec3> world_point = std::nullopt);
    [[nodiscard]] bool set_velocity(const Scene& scene, Entity entity, Vec3 linear);
    [[nodiscard]] bool set_angular_velocity(const Scene& scene, Entity entity, Vec3 radians);
    // Teleports the bodies of `entity` and its descendants to their scene transforms, after a
    // game-time transform edit. Collider shapes keep the scale they were built with.
    void sync_transforms(const Scene& scene, Entity entity);
    // Entities created during the game: gives `root` and its descendants their bodies.
    void add_bodies(const Scene& scene, Entity root);
    // Entities destroyed during the game: removes their bodies and ends their contacts.
    void remove_missing_bodies(const Scene& scene);
    // Enabled colliders overlapping or resting against (within Jolt's 2 cm speculative contact
    // distance) this entity's collider in the running world, filtered by both colliders' layers
    // and masks as collision_overlaps does.
    [[nodiscard]] CollisionOverlaps overlaps(const Scene& scene, Entity entity,
                                             std::size_t maximum = 1024U);
    // Enabled colliders on `layer_mask` layers overlapping a world-space sphere.
    [[nodiscard]] CollisionOverlaps overlap_sphere(const Scene& scene, Vec3 center, double radius,
                                                   std::uint32_t layer_mask = 0xffffffffU,
                                                   Entity ignore = {},
                                                   std::size_t maximum = 1024U);
    // Casts against the running world, without rebuilding it as collision_raycast does.
    [[nodiscard]] CollisionRaycast raycast(const Scene& scene, Vec3 origin, Vec3 direction,
                                           double maximum_distance,
                                           std::uint32_t layer_mask = 0xffffffffU,
                                           Entity ignore = {});
private:
    void ensure_built(const Scene& scene);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
