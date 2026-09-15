#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace relay {

class Scene;

class VulkanWindow {
public:
    VulkanWindow(std::string title, std::uint32_t width, std::uint32_t height);
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
    [[nodiscard]] std::uint32_t draw_call_count() const;
    [[nodiscard]] std::uint32_t render_resource_count() const;
    [[nodiscard]] std::string render_graph_json() const;
    [[nodiscard]] std::string shader_interfaces_json() const;
    void resize(std::uint32_t width, std::uint32_t height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
