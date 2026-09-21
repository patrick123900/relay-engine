#pragma once

#include "relay/render/assets.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <vector>

namespace relay {

struct RenderMatrix {
    // Column-major storage, matching GLSL's default mat4 layout.
    std::array<float, 16> values{};
};

struct RenderCamera {
    Entity entity{};
    bool using_default{true};
    RenderMatrix view_projection{};
};

struct RenderInstance {
    Entity entity{};
    RenderMatrix model{};
    RenderMatrix model_view_projection{};
    std::array<float, 4> color{};
    std::string mesh;
    std::string material;
    std::uint32_t texture_index{};
    std::uint32_t material_index{};
    // Alpha-blended instances render after opaque/masked geometry, back to front.
    bool alpha_blended{false};
    // Distance from the camera plane to the instance's bounds centre. Opaque instances are drawn
    // in increasing order; alpha-blended instances use decreasing order.
    float view_depth{};
    std::array<float, 4> emissive_metallic{};
    std::array<float, 4> surface_parameters{1.0F, 1.0F, 1.0F, 0.5F};
    // Base color, metallic-roughness and normal occupy xyz. The packed w contains occlusion and
    // emissive indices plus alpha-mode/double-sided flags.
    std::array<std::uint32_t, 4> pbr_textures{};
    // Nonnegative selects an instance-specific slice in RenderScene::deformed_vertices.
    std::int32_t deformed_vertex_offset{-1};
};

struct RenderLight {
    Light light;
    Vec3 position;
    Vec3 direction;
};

struct DrawableBounds {
    Entity entity;
    Vec3 minimum, maximum;
};

struct RenderScene {
    std::vector<DrawableBounds> drawable_bounds;
    RenderCamera camera{};
    std::vector<RenderInstance> instances;
    std::vector<MeshVertex> deformed_vertices;
    std::vector<RenderLight> lights;
    Vec3 camera_position{0.0, 0.0, 5.0};
    // Drawable entities rejected by frustum culling this frame.
    std::size_t culled{};
    std::size_t deformation_overflow{};
};

// An explicit viewpoint that replaces the scene's active camera for this frame only. The editor
// viewport uses it so navigating the view is not a scene mutation: it creates no entity, is never
// saved, and produces no undo history or trace entries.
struct ViewOverride {
    Vec3 position{0.0, 0.0, 5.0};
    Vec3 target{};
    Camera camera{};
};

[[nodiscard]] RenderScene build_render_scene(const Scene& scene, const AssetRegistry& assets,
                                             float aspect_ratio,
                                             const ViewOverride* view = nullptr, bool collect_bounds = false);

struct ScenePick {
    std::string error;
    bool hit{false};
    Entity entity{};
    double distance{};
};

struct SceneBounds {
    bool valid{false};
    // False when the entity and its descendants carry no drawable geometry, in which case the
    // bounds collapse to the entity's world position.
    bool has_geometry{false};
    Vec3 minimum{};
    Vec3 maximum{};
    Vec3 position{};
};

// Nearest drawable entity whose world-space mesh bounds the ray enters. Bounds-level precision, not
// per-triangle: it answers "which object did I click", not "exactly where on the surface".
[[nodiscard]] ScenePick pick_scene_entity(const Scene& scene, const AssetRegistry& assets,
                                          const Vec3& origin, const Vec3& direction);

// World-space bounds of an entity and its descendants, used to frame a selection in the viewport.
[[nodiscard]] SceneBounds compute_scene_bounds(const Scene& scene, const AssetRegistry& assets,
                                               Entity entity);

} // namespace relay
