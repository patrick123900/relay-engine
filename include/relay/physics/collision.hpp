#pragma once

#include "relay/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace relay {

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

struct CollisionDebugBox {
    Entity entity{};
    bool enabled{};
    Vec3 center{};
    std::array<Vec3, 3> edges{}; // World-space half edges, including parent transforms.
};

struct CollisionDebugBoxes {
    std::vector<CollisionDebugBox> boxes;
    bool truncated{};
};

// Queries use Jolt shapes built from authored box colliders and current scene transforms.
[[nodiscard]] CollisionRaycast collision_raycast(const Scene& scene, Vec3 origin, Vec3 direction,
                                                 double maximum_distance,
                                                 std::uint32_t layer_mask = 0xffffffffU);
[[nodiscard]] CollisionOverlaps collision_overlaps(const Scene& scene, Entity entity);
[[nodiscard]] CollisionDebugBoxes collision_debug_boxes(const Scene& scene,
                                                        bool enabled_only = false);

// Jolt-backed game world. Authored scene data and runtime body state have separate lifetimes.
class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    void reset();
    void step(Scene& scene, double fixed_delta_seconds);
    [[nodiscard]] std::optional<Vec3> velocity(const Scene& scene, Entity entity) const;
    [[nodiscard]] std::optional<Vec3> angular_velocity(const Scene& scene, Entity entity) const;
    [[nodiscard]] bool apply_impulse(const Scene& scene, Entity entity, Vec3 impulse,
                                     std::optional<Vec3> world_point = std::nullopt);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
