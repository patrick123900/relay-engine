#pragma once

// Vulkan ray tracing acceleration structures for the scene: a bottom-level structure per mesh,
// built once, one per deformed (skinned or morphed) instance, rebuilt every frame, and a
// top-level structure per frame in flight. Ray queries in compute shaders trace against it.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace relay {

struct RayTracingGeometry {
    VkBuffer vertex_buffer{};
    std::uint32_t vertex_stride{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    VkBuffer index_buffer{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
};

struct RayTracingInstance {
    // Identifies a static mesh's bottom-level structure; ignored for deformed geometry.
    std::string mesh;
    bool deformed{false};
    RayTracingGeometry geometry;
    // Column-major world transform.
    std::array<float, 16> model{};
    // Returned by rayQueryGetIntersectionInstanceCustomIndexEXT; 24 bits.
    std::uint32_t custom_index{};
};

class SceneAccelerationStructures {
public:
    SceneAccelerationStructures();
    ~SceneAccelerationStructures();
    SceneAccelerationStructures(const SceneAccelerationStructures&) = delete;
    SceneAccelerationStructures& operator=(const SceneAccelerationStructures&) = delete;

    [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device,
                                  std::uint32_t frames_in_flight, std::string& error);
    // Records every needed build for frame slot `frame`, ending with a barrier that makes the
    // top-level structure readable by compute shaders. The slot's previous submission must have
    // finished.
    [[nodiscard]] bool record(VkCommandBuffer commands, std::uint32_t frame,
                              const std::vector<RayTracingInstance>& instances, std::string& error);
    [[nodiscard]] VkAccelerationStructureKHR scene(std::uint32_t frame) const;
    [[nodiscard]] std::uint32_t static_structures() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
