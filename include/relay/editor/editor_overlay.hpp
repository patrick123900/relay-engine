#pragma once

// The seam between the Vulkan window and any human-facing UI drawn on top of the scene.
//
// VulkanWindow owns the instance, device, swapchain and render pass; an overlay owns only the
// resources it creates itself. Keeping the interface abstract is what allows Dear ImGui to stay
// linked into relay_demo alone, leaving the engine library free of UI dependencies.

#include "relay/editor/editor_viewport.hpp"
#include "relay/scene/scene.hpp"
#include <cstdint>
#include <functional>
#include <string>

#include <vulkan/vulkan.h>

namespace relay {

struct ViewOverride;

// Everything an overlay needs to build pipelines compatible with the window's render pass.
struct OverlayContext {
    // The window's SDL_Window, type-erased so this header does not pull SDL into every consumer.
    void* sdl_window{};
    // The instance's VkApplicationInfo::apiVersion. Overlays must not assume a newer version than
    // the window actually requested.
    std::uint32_t api_version{};
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t graphics_family{};
    VkQueue graphics_queue{};
    VkRenderPass render_pass{};
    std::uint32_t image_count{};
    std::uint32_t frames_in_flight{};
};

class EditorOverlay {
public:
    virtual ~EditorOverlay() = default;

    // Called on the render thread once the swapchain and render pass exist. Swapchain recreation
    // destroys the render pass, so the window calls invalidate() and then initialize() again;
    // implementations must tolerate that cycle without leaking device resources.
    virtual bool initialize(const OverlayContext& context, std::string& error) = 0;
    virtual void invalidate() = 0;

    // Returns true when the overlay consumed the event, in which case the window must not forward
    // it to the engine as game input. Typing in an inspector field is not player input.
    virtual bool handle_event(const void* sdl_event) = 0;

    virtual void build(std::uint32_t width, std::uint32_t height) = 0;
    // Invoke draw_scene at the viewport's position in the UI draw order. This keeps floating
    // windows and docked tabs correctly layered without introducing a separate scene renderer.
    virtual void record(VkCommandBuffer commands, const std::function<void()>& draw_scene) = 0;

    // The viewpoint the human is currently looking through, or null to use the scene's active
    // camera. The returned pointer must stay valid until the next build(). Unlike the UI chrome,
    // this does affect captures: a screenshot should show the viewport the operator sees.
    [[nodiscard]] virtual EditorViewport scene_viewport() const { return {}; }
    [[nodiscard]] virtual const ViewOverride* view_override() const { return nullptr; }
    // Presentation-only selection; never written to scene state or exported captures.
    [[nodiscard]] virtual Entity selected_entity() const { return {}; }
    [[nodiscard]] virtual bool ground_grid_visible() const { return false; }
};

} // namespace relay
