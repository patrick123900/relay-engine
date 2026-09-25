#pragma once

#include "relay/render/assets.hpp"
#include "relay/scene/scene.hpp"

#include <array>
#include <cstddef>
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
    // The parts of view_projection, for effects that reconstruct positions from depth.
    RenderMatrix view{};
    RenderMatrix projection{};
    float exposure_ev{};
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
    // Off-camera objects can still enter the render list when they cast into a shadow cascade,
    // or always when build_render_scene is asked to keep culled instances.
    bool camera_visible{true};
    std::uint16_t shadow_cascade_mask{};
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

inline constexpr std::size_t directional_shadow_cascade_count = 3U;
inline constexpr std::size_t spot_shadow_map_index = directional_shadow_cascade_count;
inline constexpr std::size_t shadow_map_count = directional_shadow_cascade_count + 1U;
inline constexpr std::size_t point_shadow_face_count = 6U;
inline constexpr std::size_t point_shadow_mask_offset = shadow_map_count;
inline constexpr std::array<std::uint32_t, directional_shadow_cascade_count>
    directional_shadow_resolutions{2048U, 1024U, 1024U};
inline constexpr std::uint32_t spot_shadow_resolution = 1024U;
inline constexpr std::uint32_t point_shadow_resolution = 1024U;

struct DirectionalShadow {
    bool enabled{false};
    std::size_t light_index{};
    std::array<RenderMatrix, directional_shadow_cascade_count> view_projections{};
    std::array<float, directional_shadow_cascade_count> split_depths{};
};

struct SpotShadow {
    bool enabled{false};
    std::size_t light_index{};
    RenderMatrix view_projection{};
};

struct PointShadow {
    bool enabled{false};
    std::size_t light_index{};
    std::array<RenderMatrix, point_shadow_face_count> view_projections{};
    Vec3 position{};
    float near_plane{0.05F};
    float far_plane{50.0F};
};

struct RenderScene {
    std::vector<DrawableBounds> drawable_bounds;
    RenderCamera camera{};
    std::vector<RenderInstance> instances;
    std::vector<MeshVertex> deformed_vertices;
    std::vector<RenderLight> lights;
    // The first GPU-visible directional light owns the cascaded shadow budget. Matrices and split
    // depths live here so fitting, stabilization and fallback behavior remain headlessly testable.
    DirectionalShadow directional_shadow{};
    SpotShadow spot_shadow{};
    PointShadow point_shadow{};
    Vec3 camera_position{0.0, 0.0, 5.0};
    Vec3 camera_forward{0.0, 0.0, -1.0};
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
                                             const ViewOverride* view = nullptr, bool collect_bounds = false,
                                             bool keep_culled = false);

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
