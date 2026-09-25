#pragma once

// Global illumination for the Vulkan renderer, built on AMD FidelityFX Brixelizer (a sparse
// distance field of the scene, rebuilt incrementally on the GPU) and Brixelizer GI (screen probes
// traced through that field, producing diffuse and specular indirect light). Only compiled when
// the FidelityFX library is available; see third_party/fidelityfx.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace relay {

struct LightingDevice {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
};

// One mesh draw for the distance field, in world space.
struct LightingInstance {
    // Stable across frames for the same draw; see VulkanWindow's motion vectors.
    std::uint64_t key{};
    std::array<float, 16> model{};
    VkBuffer vertex_buffer{};
    VkDeviceSize vertex_buffer_bytes{};
    std::uint32_t vertex_stride{};
    std::uint32_t first_vertex{};
    std::uint32_t vertex_count{};
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::array<float, 3> bounds_min{};
    std::array<float, 3> bounds_max{};
    // Skinned or morphed geometry, rebuilt every frame from this frame's deformed vertices.
    bool deformed{false};
};

// A sampled image together with how it was created, so FidelityFX can describe it.
struct LightingImage {
    VkImage image{};
    VkImageCreateInfo info{};
};

struct LightingFrame {
    VkCommandBuffer commands{};
    std::uint64_t frame_index{};
    VkExtent2D extent{};
    // This frame's G-buffer and the previous frame's, all in SHADER_READ_ONLY_OPTIMAL.
    LightingImage depth, normal_roughness, motion, history_depth, history_normal_roughness,
        previous_lit;
    // Column-major matrices with Relay's Vulkan conventions (depth 0..1, y down in clip space).
    std::array<float, 16> view{}, projection{}, previous_view{}, previous_projection{};
    std::array<float, 3> camera_position{};
    std::array<float, 3> sky_color{}, ground_color{};
    VkBuffer index_buffer{};
    VkDeviceSize index_buffer_bytes{};
    std::vector<LightingInstance> instances;
};

struct LightingStatus {
    bool available{false};
    std::string error;
    std::uint32_t static_instances{};
    std::uint32_t dynamic_instances{};
    std::uint64_t scratch_bytes{};
};

class GlobalIllumination {
public:
    GlobalIllumination();
    ~GlobalIllumination();
    GlobalIllumination(const GlobalIllumination&) = delete;
    GlobalIllumination& operator=(const GlobalIllumination&) = delete;

    // Creates the distance field, its GPU resources and the noise and environment textures.
    [[nodiscard]] bool initialize(const LightingDevice& device, std::string& error);
    // Records the distance field update and the GI dispatch. On success the diffuse and specular
    // images are in SHADER_READ_ONLY_OPTIMAL and hold this frame's indirect light. The caller
    // must have waited for the device to be idle if the extent changed since the last call.
    [[nodiscard]] bool record(const LightingFrame& frame, std::string& error);
    // Drops the GI context and outputs, for example before the scene targets are resized.
    void release_outputs();
    [[nodiscard]] VkImageView diffuse_view() const;
    [[nodiscard]] VkImageView specular_view() const;
    [[nodiscard]] VkExtent2D extent() const;
    [[nodiscard]] LightingStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct ReflectionFrame {
    VkCommandBuffer commands{};
    std::uint64_t frame_index{};
    VkExtent2D extent{};
    // This frame's G-buffer, in SHADER_READ_ONLY_OPTIMAL.
    LightingImage depth, normal_roughness, motion;
    // Column-major matrices with Relay's Vulkan conventions, as in LightingFrame.
    std::array<float, 16> view{}, projection{}, previous_view{}, previous_projection{};
    std::array<float, 3> sky_color{}, ground_color{};
    // The history is invalid, for example after the targets were recreated.
    bool reset{false};
};

// Buffers and images the ray tracing pass between classification and denoising uses. The
// classifier fills the hardware ray list; Relay's trace shader writes each ray's radiance and
// hit distance into `radiance` and `variance`, both in the GENERAL layout.
struct ReflectionTargets {
    VkBuffer ray_list{};
    VkBuffer ray_counter{};
    VkBuffer indirect_arguments{};
    VkImageView radiance{};
    VkImageView variance{};
};

// Byte offsets into ReflectionTargets::ray_counter and indirect_arguments, shared with the
// shaders; the same layout as AMD's Hybrid Reflections sample.
inline constexpr std::uint32_t reflection_counter_hardware = 16U;
inline constexpr std::uint32_t reflection_counter_hardware_history = 20U;
inline constexpr std::uint32_t reflection_counter_denoise = 8U;
inline constexpr std::uint32_t reflection_counter_denoise_history = 12U;
inline constexpr std::uint32_t reflection_arguments_denoise = 12U;
inline constexpr std::uint32_t reflection_arguments_hardware = 36U;
// Surfaces rougher than this (GGX alpha, perceptual roughness squared) keep the reflections
// global illumination provides.
inline constexpr float reflection_roughness_threshold = 0.25F;

// Ray traced reflections: AMD FidelityFX Classifier picks the pixels that need rays and the
// FidelityFX reflection denoiser filters the traced results.
class Reflections {
public:
    Reflections();
    ~Reflections();
    Reflections(const Reflections&) = delete;
    Reflections& operator=(const Reflections&) = delete;

    [[nodiscard]] bool initialize(const LightingDevice& device, std::string& error);
    // Classifies this frame's pixels into the hardware ray list. Afterwards the ray counter holds
    // the number of rays; see the reflection_* offsets.
    [[nodiscard]] bool record_classification(const ReflectionFrame& frame, std::string& error);
    // Denoises the traced radiance into output(), left in SHADER_READ_ONLY_OPTIMAL.
    [[nodiscard]] bool record_denoising(const ReflectionFrame& frame, std::string& error);
    [[nodiscard]] ReflectionTargets targets(std::uint64_t frame_index) const;
    [[nodiscard]] VkImageView output() const;
    [[nodiscard]] VkImageView noise(std::uint64_t frame_index) const;
    [[nodiscard]] VkExtent2D extent() const;
    [[nodiscard]] LightingStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
