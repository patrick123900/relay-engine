#pragma once
#include "relay/observe/capture.hpp"
#include <functional>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace relay {

class AssetRegistry;
class EditorOverlay;
struct RenderInterpolation;
class Scene;

class VulkanWindow {
public:
    // The registry supplies every mesh, material and texture this window uploads, and must outlive
    // the window. Imported content is picked up by comparing the registry revision each frame.
    VulkanWindow(std::string title, std::uint32_t width, std::uint32_t height,
                 const AssetRegistry& assets, bool editor_window = false);
    ~VulkanWindow();

    VulkanWindow(const VulkanWindow&) = delete;
    VulkanWindow& operator=(const VulkanWindow&) = delete;
    VulkanWindow(VulkanWindow&&) noexcept;
    VulkanWindow& operator=(VulkanWindow&&) noexcept;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] std::string device_name() const;
    [[nodiscard]] bool poll_quit();
    [[nodiscard]] std::vector<std::string> drain_input_events();
    [[nodiscard]] bool draw(double elapsed_seconds);
    [[nodiscard]] bool draw(const Scene& scene, double elapsed_seconds);
    [[nodiscard]] bool capture_image(const std::filesystem::path& path, double elapsed_seconds);
    [[nodiscard]] bool capture_image(const std::filesystem::path& path, const Scene& scene,
                                     double elapsed_seconds);
    [[nodiscard]] double gpu_frame_milliseconds() const;
    // Whether the last draw presented an image. A minimized window draws without presenting.
    [[nodiscard]] bool presented() const;
    // Game state to blend between steps in presented frames; captures always draw the scene as
    // it is. Null draws the scene as it is. The pointer must stay valid until it is replaced.
    void set_render_interpolation(const RenderInterpolation* interpolation);
    [[nodiscard]] std::uint32_t draw_call_count() const;
    [[nodiscard]] std::uint32_t render_resource_count() const;
    [[nodiscard]] std::string render_graph_json() const;
    [[nodiscard]] std::string shader_interfaces_json() const;
    [[nodiscard]] std::string upload_status_json() const;
    using FrameReceiver = std::function<void(OwnedFrame, std::string)>;
    bool readback_async(const Scene& scene, double elapsed_seconds, FrameReceiver receiver, std::string& error);
    void flush_readbacks();
    void resize(std::uint32_t width, std::uint32_t height);
    // Turns FidelityFX global illumination on or off. When the device or build cannot provide it,
    // the renderer keeps its analytic sky light and lighting_status_json() says why.
    void set_global_illumination(bool enabled);
    // Turns hardware ray traced reflections on or off, with the same fallback and reporting.
    void set_reflections(bool enabled);
    // Presents in step with the display (FIFO) or, off, as soon as a frame is ready (immediate,
    // else mailbox). A change rebuilds the swapchain after the next present.
    void set_vsync(bool enabled);
    [[nodiscard]] std::string lighting_status_json() const;
    // Installs the human-facing UI layer. The overlay must outlive the window. It is drawn into the
    // swapchain render pass for presentation only and is deliberately excluded from every capture
    // and readback, so screenshots, recordings and golden comparisons keep showing scene pixels.
    void set_overlay(EditorOverlay* overlay);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
