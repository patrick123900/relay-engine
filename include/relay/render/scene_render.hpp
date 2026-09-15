#pragma once

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
};

struct RenderScene {
    RenderCamera camera{};
    std::vector<RenderInstance> instances;
};

[[nodiscard]] RenderScene build_render_scene(const Scene& scene, float aspect_ratio);

} // namespace relay
