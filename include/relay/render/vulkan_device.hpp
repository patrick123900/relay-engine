#pragma once

#include <cstdint>
#include <string>

namespace relay {

struct VulkanCapabilities {
    bool loader_available{false};
    bool device_available{false};
    bool logical_device_created{false};
    bool ray_tracing_pipeline{false};
    bool ray_query{false};
    bool mesh_shader{false};
    bool descriptor_buffer{false};
    std::uint32_t api_major{0};
    std::uint32_t api_minor{0};
    std::uint32_t api_patch{0};
    std::string device_name;
    std::string device_type;
    std::string error;
};

// Creates a short-lived instance and logical device. This is deliberately a probe rather than
// the final renderer lifetime; it verifies that Relay can use the selected adapter, not merely
// that a Vulkan loader happens to be installed.
[[nodiscard]] VulkanCapabilities probe_vulkan_capabilities();

} // namespace relay

