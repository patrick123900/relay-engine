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
    // Distance from the camera plane to the instance's bounds centre. Opaque instances are drawn
    // in increasing order of this value.
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

struct RenderScene {
    RenderCamera camera{};
    std::vector<RenderInstance> instances;
    std::vector<MeshVertex> deformed_vertices;
    std::vector<RenderLight> lights;
    Vec3 camera_position{0.0, 0.0, 5.0};
    // Drawable entities rejected by frustum culling this frame.
    std::size_t culled{};
    std::size_t deformation_overflow{};
};

[[nodiscard]] RenderScene build_render_scene(const Scene& scene, const AssetRegistry& assets,
                                             float aspect_ratio);

} // namespace relay
