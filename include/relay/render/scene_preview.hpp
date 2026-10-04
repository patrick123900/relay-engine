#pragma once

// A small CPU picture of a scene from an explicit viewpoint, for editor windows that show a scene
// other than the one the main viewport draws (the template editor). It uses the same render scene
// the GPU renderer starts from, so culling, hierarchy, animation and materials' base colors match,
// but shades with a plain Lambert model: no shadows, global illumination, reflections, custom
// shaders or post processing. Safe to call from any thread that owns the scene.

#include "relay/render/scene_render.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace relay {

struct ScenePreviewOptions {
    bool grid{true};   // A ground grid on the y = 0 plane behind the geometry.
    bool sky{true};    // The scene's sky (or a plain gradient) behind it; off draws a flat color.
    // Nodes outlined in orange, with everything drawn by their descendants.
    std::vector<Entity> highlight;
};

struct ScenePreviewFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba; // Opaque 8-bit sRGB rows.
    // The matrices the picture was drawn with, so overlays (selection, gizmos, markers) can project
    // world points onto it: clip = projection * view * world, with Vulkan's depth range and a Y
    // axis that points down.
    RenderMatrix view{};
    RenderMatrix projection{};
    RenderMatrix view_projection{};
};

[[nodiscard]] bool render_scene_preview(const Scene& scene, const AssetRegistry& assets, const ViewOverride& view,
                                        std::uint32_t width, std::uint32_t height,
                                        const ScenePreviewOptions& options, ScenePreviewFrame& output,
                                        std::string& error);

} // namespace relay
