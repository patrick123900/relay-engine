#include "relay/platform/vulkan_window.hpp"
#include "relay/platform/sdl_input.hpp"
#include "relay/editor/editor_overlay.hpp"
#include "relay/observe/capture.hpp"
#include "relay/observe/profiler.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/shader_reflection.hpp"
#include "relay/render/upload_budget.hpp"

#ifdef RELAY_HAS_FIDELITYFX
#include "vulkan_lighting.hpp"
#include "vulkan_ray_tracing.hpp"
#endif

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace relay {
namespace {

constexpr std::size_t frames_in_flight = 2;
// Timestamps per frame: one at the start and one after each measured GPU pass.
constexpr std::uint32_t gpu_marker_capacity = 16U;

std::string vk_error(const std::string& operation, const VkResult result) {
    return operation + " failed with Vulkan result " + std::to_string(result);
}

bool extension_available(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::string_view(extension.extensionName) == name;
    });
}

// Column-major 4x4 product, matching GLSL.
std::array<float, 16> multiply_matrices(const std::array<float, 16>& left,
                                        const std::array<float, 16>& right) {
    std::array<float, 16> product{};
    for (std::size_t row = 0; row < 4U; ++row)
        for (std::size_t column = 0; column < 4U; ++column)
            for (std::size_t inner = 0; inner < 4U; ++inner)
                product[column * 4U + row] += left[inner * 4U + row] * right[column * 4U + inner];
    return product;
}

// Inverse of a column-major 4x4 matrix by cofactor expansion; the zero matrix when singular.
std::array<float, 16> invert_matrix(const std::array<float, 16>& m) {
    std::array<double, 16> inverse{};
    inverse[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
                 m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inverse[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
                 m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inverse[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
                 m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inverse[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
                  m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inverse[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
                 m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inverse[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
                 m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inverse[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
                 m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inverse[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
                  m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inverse[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
                 m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inverse[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
                 m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inverse[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
                  m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inverse[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
                  m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inverse[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
                 m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inverse[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
                 m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inverse[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
                  m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inverse[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
                  m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double determinant = m[0] * inverse[0] + m[1] * inverse[4] + m[2] * inverse[8] +
                               m[3] * inverse[12];
    std::array<float, 16> result{};
    if (std::abs(determinant) < 1e-30) return result;
    for (std::size_t index = 0; index < 16U; ++index)
        result[index] = static_cast<float>(inverse[index] / determinant);
    return result;
}

// The analytic sky and ground light in first_light.frag, which global illumination replaces
// for opaque surfaces and uses for rays that leave the scene.
constexpr std::array<float, 3> sky_radiance{0.20F, 0.31F, 0.48F};
constexpr std::array<float, 3> ground_radiance{0.055F, 0.047F, 0.039F};

constexpr std::uint32_t no_draw_slot = std::numeric_limits<std::uint32_t>::max();

// GGX alpha above which reflections come from GI or the sky rather than traced rays. The same
// value as reflection_roughness_threshold in vulkan_lighting.hpp, which only exists in builds
// with FidelityFX.
constexpr float reflection_roughness_limit = 0.25F;

std::vector<std::uint32_t> read_shader(const std::filesystem::path& path, std::string& error) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        error = "could not open compiled shader: " + path.string();
        return {};
    }
    const auto end = stream.tellg();
    if (end <= 0 || end % static_cast<std::streamoff>(sizeof(std::uint32_t)) != 0) {
        error = "compiled shader has an invalid size: " + path.string();
        return {};
    }
    std::vector<std::uint32_t> code(static_cast<std::size_t>(end) / sizeof(std::uint32_t));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), end);
    if (!stream) {
        error = "could not read compiled shader: " + path.string();
        return {};
    }
    return code;
}

} // namespace

struct VulkanWindow::Impl {
    const AssetRegistry* assets{};
    SDL_Window* window{};
    bool sdl_initialized{false};
    VkInstance instance{};
    std::uint32_t instance_api_version{VK_API_VERSION_1_0};
    std::uint32_t device_api_version{VK_API_VERSION_1_0};
    // Set when the device has everything the FidelityFX lighting effects use.
    bool lighting_features{false};
    // Set when hardware ray queries against acceleration structures are enabled.
    bool ray_query_features{false};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t graphics_family{};
    std::uint32_t present_family{};
    std::uint32_t transfer_family{};
    VkQueue graphics_queue{};
    VkQueue present_queue{};
    VkQueue transfer_queue{};
    VkCommandPool command_pool{};
    VkCommandPool transfer_command_pool{};
    VkBuffer mesh_vertex_buffer{};
    VkDeviceMemory mesh_vertex_memory{};
    VkBuffer mesh_index_buffer{};
    VkDeviceMemory mesh_index_memory{};
    VkBuffer material_buffer{};
    VkDeviceMemory material_memory{};
    VkDeviceSize mesh_vertex_capacity{}, mesh_index_capacity{}, material_capacity{};
    std::size_t uploaded_vertex_count{}, uploaded_index_count{};
    std::size_t uploaded_material_count{}, uploaded_texture_count{};
    struct GpuTexture {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        std::uint32_t mip_levels{};
    };
    std::vector<GpuTexture> textures;
    VkDescriptorSetLayout texture_layout{};
    VkDescriptorPool texture_pool{};
    std::array<VkDescriptorSet, frames_in_flight> texture_sets{};
    struct AssetResources {
        VkBuffer vertex_buffer{}, index_buffer{}, material_buffer{};
        VkDeviceMemory vertex_memory{}, index_memory{}, material_memory{};
        std::vector<GpuTexture> textures;
        VkDescriptorPool descriptor_pool{};
        std::array<VkDescriptorSet, frames_in_flight> descriptor_sets{};
        VkDeviceSize vertex_capacity{}, index_capacity{}, material_capacity{};
        std::size_t vertex_count{}, index_count{}, material_count{}, texture_count{};
        std::uint64_t revision{};
        std::uint64_t device_estimate{};
    };
    std::optional<AssetResources> retired_assets;
    std::array<VkBuffer, frames_in_flight> deformed_buffers{}, lighting_buffers{}, draw_buffers{};
    std::array<VkDeviceMemory, frames_in_flight> deformed_memories{}, lighting_memories{},
        draw_memories{};
    std::array<VkDeviceSize, frames_in_flight> deformed_capacities{}, draw_capacities{};
    struct alignas(16) GpuDraw {
        std::array<float, 16> previous_model_view_projection{};
        std::array<std::uint32_t, 4> material{};
    };
    static_assert(sizeof(GpuDraw) == 80U);
    // Last frame's model matrices by draw key and its camera, for motion vectors.
    std::unordered_map<std::uint64_t, std::array<float, 16>> previous_models;
    std::array<float, 16> previous_view_projection{};
    bool previous_frame_valid{false};
    VkCommandBuffer upload_commands{};
    VkFence upload_fence{};
    VkSemaphore upload_complete{};
    bool upload_wait_pending{};
    std::array<std::vector<VkSemaphore>, frames_in_flight> retired_upload_semaphores;
    std::array<bool, frames_in_flight> old_asset_frame_pending{};
    std::vector<VkBuffer> upload_staging_buffers;
    std::vector<VkDeviceMemory> upload_staging_memories;
    UploadBudget upload_budget{};
    std::uint64_t upload_staging_bytes{};
    std::uint64_t peak_upload_staging_bytes{};
    std::uint64_t active_device_estimate{};
    std::uint64_t last_upload_bytes{};
    std::uint64_t upload_batches{};
    std::uint64_t rejected_uploads{};
    struct alignas(16) GpuLight {
        std::array<float, 4> position_type{}, direction_inner{}, color_intensity{},
            attenuation_outer{};
        std::array<float, 4> range{};
    };
    struct alignas(16) GpuLighting {
        std::array<float, 4> camera_count{0, 0, 5, 0};
        std::array<GpuLight, 16> lights{};
        std::array<std::array<float, 16>, directional_shadow_cascade_count>
            shadow_view_projections{};
        std::array<float, 16> spot_shadow_view_projection{};
        std::array<std::array<float, 16>, point_shadow_face_count>
            point_shadow_view_projections{};
        std::array<float, 4> shadow_splits{};
        std::array<float, 4> camera_forward{};
        std::array<float, 4> point_shadow_position_far{};
        std::array<std::uint32_t, 4> shadow_parameters{};
        std::array<std::uint32_t, 4> point_shadow_parameters{};
    };
    struct alignas(16) GpuMaterial {
        std::array<float, 4> base_color_factor;
        std::array<float, 4> emissive_metallic;
        std::array<float, 4> surface_parameters;
        std::array<std::uint32_t, 4> texture_indices;
    };
    struct ToneSettings {
        float exposure{};
        std::uint32_t encode_srgb{};
        std::array<float, 2> viewport_offset{};
        std::array<float, 2> viewport_size{};
    };
    static_assert(sizeof(ToneSettings) == 24U);
    struct DrawPushConstants {
        std::array<float, 16> model_view_projection;
        std::array<float, 16> model;
    };
    static_assert(sizeof(DrawPushConstants) == 128U);
    static_assert(sizeof(GpuMaterial) == 64U);
    static_assert(sizeof(GpuLight) == 80U && sizeof(GpuLighting) == 2016U);
    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    bool transfer_source_supported{false};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> image_views;
    VkFormat hdr_format{VK_FORMAT_R16G16B16A16_SFLOAT};
    // The scene renders into viewport-sized targets, one set per frame in flight, so the other set
    // holds the previous frame for temporal effects. The opaque geometry pass writes every target;
    // the forward pass adds transparent geometry and editor overlays to the HDR image.
    static constexpr VkFormat normal_roughness_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat albedo_metallic_format = VK_FORMAT_R8G8B8A8_SRGB;
    static constexpr VkFormat motion_occlusion_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat scene_depth_value_format = VK_FORMAT_R32_SFLOAT;
    static constexpr VkFormat diffuse_light_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr std::uint32_t geometry_color_attachments = 6U;
    struct SceneImage {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkImageCreateInfo info{};
    };
    struct SceneTargets {
        SceneImage hdr, normal_roughness, albedo_metallic, motion_occlusion, depth_value,
            diffuse_light, depth_stencil;
        VkFramebuffer geometry_framebuffer{}, forward_framebuffer{};
    };
    std::array<SceneTargets, frames_in_flight> scene_targets{};
    // Whether each frame's targets hold a finished frame, and so can serve as history.
    std::array<bool, frames_in_flight> scene_targets_rendered{};
    VkExtent2D scene_extent{};
    // Global illumination: requested by the host, available when the device and the FidelityFX
    // build allow it. The composite pass adds its light to the HDR image.
#ifdef RELAY_HAS_FIDELITYFX
    std::unique_ptr<GlobalIllumination> global_illumination;
#endif
    bool global_illumination_requested{false};
    bool global_illumination_failed{false};
    bool global_illumination_active{false};
    std::string global_illumination_error;
    std::uint64_t lighting_frame_index{};
    std::array<float, 16> previous_view{}, previous_projection{};
    // Ray traced reflections: FidelityFX classifies and denoises, Relay traces in between.
#ifdef RELAY_HAS_FIDELITYFX
    std::unique_ptr<Reflections> reflections;
    std::unique_ptr<SceneAccelerationStructures> acceleration_structures;
#endif
    bool reflections_requested{false};
    bool reflections_failed{false};
    bool reflections_active{false};
    std::string reflections_error;
    std::uint64_t reflection_frame_index{};
    VkDescriptorSetLayout trace_layout{}, arguments_layout{};
    VkDescriptorPool trace_pool{};
    std::array<VkDescriptorSet, frames_in_flight> trace_sets{}, arguments_sets{};
    VkPipelineLayout trace_pipeline_layout{}, arguments_pipeline_layout{};
    VkPipeline trace_pipeline{}, arguments_pipeline{};
    std::array<VkBuffer, frames_in_flight> trace_instance_buffers{};
    std::array<VkDeviceMemory, frames_in_flight> trace_instance_memories{};
    std::array<VkDeviceSize, frames_in_flight> trace_instance_capacities{};
    struct TraceConstants {
        std::array<float, 16> inverse_view_projection{};
        std::array<float, 4> camera_position{};
        std::array<std::uint32_t, 4> extent{};
    };
    static_assert(sizeof(TraceConstants) == 96U);
    struct GpuTraceInstance {
        std::uint32_t material{};
        std::uint32_t first_index{};
        std::int32_t vertex_offset{};
        std::uint32_t deformed{};
    };
    VkDescriptorSetLayout composite_layout{};
    VkDescriptorPool composite_pool{};
    std::array<VkDescriptorSet, frames_in_flight> composite_sets{};
    VkPipelineLayout composite_pipeline_layout{};
    VkPipeline composite_pipeline{};
    struct CompositeConstants {
        std::array<float, 16> inverse_view_projection{};
        std::array<float, 4> camera_position{};
        std::array<float, 4> extent{};
    };
    static_assert(sizeof(CompositeConstants) == 96U);
    VkRenderPass geometry_render_pass{};
    // Transparent geometry, grid and selection draw here, after the geometry pass.
    VkRenderPass scene_render_pass{};
    VkRenderPass render_pass{};
    VkDescriptorSetLayout tone_layout{};
    VkDescriptorPool tone_pool{};
    VkSampler tone_sampler{};
    std::array<VkDescriptorSet, frames_in_flight> tone_sets{};
    VkPipelineLayout tone_pipeline_layout{};
    VkPipeline tone_pipeline{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    VkPipeline transparent_pipeline{};
    VkPipeline grid_pipeline{};
    VkPipeline selection_mask_pipeline{}, selection_outline_pipeline{};
    VkRenderPass shadow_render_pass{};
    VkPipelineLayout shadow_pipeline_layout{};
    VkPipeline shadow_pipeline{};
    VkSampler shadow_sampler{};
    VkSampler point_shadow_sampler{};
    VkFormat shadow_format{VK_FORMAT_UNDEFINED};
    struct ShadowAttachment {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkFramebuffer framebuffer{};
        std::uint32_t extent{};
    };
    std::array<ShadowAttachment, frames_in_flight * shadow_map_count>
        shadow_attachments{};
    struct PointShadowAttachment {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView cube_view{};
        std::array<VkImageView, point_shadow_face_count> face_views{};
        std::array<VkFramebuffer, point_shadow_face_count> framebuffers{};
    };
    std::array<PointShadowAttachment, frames_in_flight> point_shadow_attachments{};
    // Each swapchain image owns its depth attachment. Multiple frames may be executing on the GPU
    // concurrently, so sharing one depth image across their framebuffers would introduce a write
    // hazard that the per-frame fences do not prevent.
    struct DepthAttachment {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
    };
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    std::vector<DepthAttachment> depth_attachments;
    std::vector<VkFramebuffer> framebuffers;
    std::array<VkCommandBuffer, frames_in_flight> command_buffers{};
    std::array<VkSemaphore, frames_in_flight> image_available{};
    // One per swapchain image: presentation holds the semaphore until that image is reacquired,
    // which a per-frame-in-flight semaphore does not guarantee.
    std::vector<VkSemaphore> render_finished;
    std::array<VkFence, frames_in_flight> frame_fences{};
    struct Readback {
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkExtent2D extent{};
        VkFormat format{};
        std::uint64_t serial{};
        FrameReceiver receiver;
    };
    std::array<Readback, frames_in_flight> readbacks{};
    std::uint64_t readback_serial{};
    VkQueryPool timestamp_queries{};
    std::array<bool, frames_in_flight> timestamp_submitted{};
    // Each frame's timestamps, the profiler name of the pass that ends at each one and the
    // profiler frame that recorded them.
    std::array<std::uint32_t, frames_in_flight> gpu_marker_counts{};
    std::array<std::array<std::uint32_t, gpu_marker_capacity>, frames_in_flight> gpu_marker_names{};
    std::array<std::uint64_t, frames_in_flight> gpu_profile_frames{};
    float timestamp_period_nanoseconds{};
    double latest_gpu_milliseconds{};
    bool last_draw_presented{};
    const RenderInterpolation* render_interpolation{};
    std::uint32_t latest_draw_calls{};
    std::size_t current_frame{};
    bool resized{false};
    // Vsync as requested by the project and the presentation mode the swapchain actually uses.
    bool vsync_requested{false};
    VkPresentModeKHR present_mode_in_use{VK_PRESENT_MODE_FIFO_KHR};
    bool submission_failed{false};
    std::string selected_device_name;
    std::string last_error;
    std::vector<std::string> pending_input_events;
    CompiledRenderGraph render_graph;
    ShaderInterface vertex_interface;
    ShaderInterface fragment_interface;
    std::uint64_t uploaded_asset_revision{};
    EditorOverlay* overlay{};
    bool overlay_ready{false};
    bool overlay_failed{false};

    ~Impl() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        cleanup_upload_batch();
        if (retired_assets.has_value()) {
            destroy_asset_resources(*retired_assets);
            retired_assets.reset();
        }
        flush_readbacks();
        for (auto& slot : readbacks) {
            if (device) { vkDestroyBuffer(device, slot.buffer, nullptr); vkFreeMemory(device, slot.memory, nullptr); }
        }
        cleanup_swapchain();
#ifdef RELAY_HAS_FIDELITYFX
        global_illumination.reset();
        reflections.reset();
        acceleration_structures.reset();
#endif
        destroy_reflection_pipelines();
        if (device != VK_NULL_HANDLE) {
            for (std::size_t index = 0; index < frames_in_flight; ++index) {
                vkDestroyBuffer(device, deformed_buffers[index], nullptr);
                vkFreeMemory(device, deformed_memories[index], nullptr);
                vkDestroyBuffer(device, lighting_buffers[index], nullptr);
                vkFreeMemory(device, lighting_memories[index], nullptr);
                vkDestroyBuffer(device, draw_buffers[index], nullptr);
                vkFreeMemory(device, draw_memories[index], nullptr);
                vkDestroyFence(device, frame_fences[index], nullptr);
                vkDestroySemaphore(device, image_available[index], nullptr);
            }
            vkDestroyBuffer(device, mesh_index_buffer, nullptr);
            vkFreeMemory(device, mesh_index_memory, nullptr);
            vkDestroyBuffer(device, mesh_vertex_buffer, nullptr);
            vkFreeMemory(device, mesh_vertex_memory, nullptr);
            vkDestroyBuffer(device, material_buffer, nullptr);
            vkFreeMemory(device, material_memory, nullptr);
            vkDestroyDescriptorPool(device, texture_pool, nullptr);
            for (const auto& texture : textures) {
                vkDestroySampler(device, texture.sampler, nullptr);
                vkDestroyImageView(device, texture.view, nullptr);
                vkDestroyImage(device, texture.image, nullptr);
                vkFreeMemory(device, texture.memory, nullptr);
            }
            vkDestroyDescriptorSetLayout(device, texture_layout, nullptr);
            vkDestroySampler(device, shadow_sampler, nullptr);
            vkDestroyCommandPool(device, command_pool, nullptr);
            vkDestroyCommandPool(device, transfer_command_pool, nullptr);
            vkDestroySemaphore(device, upload_complete, nullptr);
            for (const auto& semaphores : retired_upload_semaphores)
                for (const auto semaphore : semaphores)
                    vkDestroySemaphore(device, semaphore, nullptr);
            vkDestroyQueryPool(device, timestamp_queries, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE && surface != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance, surface, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
        SDL_DestroyWindow(window);
        if (sdl_initialized) SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    }

    bool initialize(const std::string& title, const std::uint32_t width, const std::uint32_t height, const bool editor_window) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            last_error = SDL_GetError();
            return false;
        }
        sdl_initialized = true;
        window = SDL_CreateWindow(title.c_str(), static_cast<int>(width), static_cast<int>(height),
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                      (editor_window ? SDL_WINDOW_MAXIMIZED |
                                                           SDL_WINDOW_HIGH_PIXEL_DENSITY : 0));
        if (window == nullptr) {
            last_error = SDL_GetError();
            return false;
        }
        render_graph = make_scene_render_graph();
        if (!render_graph.valid) {
            last_error = "render graph compilation failed: " + render_graph.error;
            return false;
        }
        const auto vertex_bytes = assets->mesh_vertices().size_bytes();
        const auto index_bytes = assets->mesh_indices().size_bytes();
        const auto material_bytes = assets->materials().size() * sizeof(GpuMaterial);
        const auto initial_upload = estimate_asset_upload(
            *assets, 0U, growing_capacity(vertex_bytes), growing_capacity(index_bytes),
            growing_capacity(material_bytes), vertex_bytes + index_bytes + material_bytes);
        if (!preflight(initial_upload)) return false;
        if (!(create_instance() && create_surface() && pick_physical_device() &&
              create_logical_device() && create_timestamp_pool() && create_command_pool() &&
              begin_upload_batch() && create_mesh_buffers() && create_texture_resources() &&
              finish_upload_batch()))
            return false;
        active_device_estimate = initial_upload.device_bytes;
        return create_swapchain_resources() && create_sync_objects();
    }

    bool create_instance() {
        std::uint32_t extension_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (sdl_extensions == nullptr) {
            last_error = SDL_GetError();
            return false;
        }
        std::vector<const char*> extensions(sdl_extensions, sdl_extensions + extension_count);

        std::uint32_t available_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr);
        std::vector<VkExtensionProperties> available_extensions(available_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available_extensions.data());

        VkInstanceCreateFlags flags = 0;
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (extension_available(available_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "Relay First Light";
        application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        application.pEngineName = "Relay";
        application.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
        // Vulkan 1.3 exposes the features FidelityFX and ray queries need. Older loaders still get
        // the base renderer, which only uses Vulkan 1.0.
        std::uint32_t loader_version = VK_API_VERSION_1_0;
        if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")))
            enumerate(&loader_version);
        instance_api_version = std::min(loader_version, VK_API_VERSION_1_3);
        application.apiVersion = instance_api_version;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.flags = flags;
        create_info.pApplicationInfo = &application;
        create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        const auto result = vkCreateInstance(&create_info, nullptr, &instance);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateInstance", result);
            return false;
        }
        return true;
    }

    bool create_surface() {
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            last_error = SDL_GetError();
            return false;
        }
        return true;
    }

    bool queue_families_for(const VkPhysicalDevice candidate, std::uint32_t& graphics,
                            std::uint32_t& present) const {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
        bool graphics_found = false;
        bool present_found = false;
        for (std::uint32_t index = 0; index < count; ++index) {
            if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U && !graphics_found) {
                graphics = index;
                graphics_found = true;
            }
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &supported);
            if (supported == VK_TRUE && !present_found) {
                present = index;
                present_found = true;
            }
        }
        return graphics_found && present_found;
    }

    bool device_supports_swapchain(const VkPhysicalDevice candidate) const {
        std::uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, extensions.data());
        if (!extension_available(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return false;
        std::uint32_t format_count = 0;
        std::uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(candidate, surface, &format_count, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(candidate, surface, &mode_count, nullptr);
        return format_count > 0 && mode_count > 0;
    }

    bool pick_physical_device() {
        std::uint32_t count = 0;
        auto result = vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (result != VK_SUCCESS || count == 0) {
            last_error = "no Vulkan physical devices can present to this window";
            return false;
        }
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        int best_score = -1;
        for (const auto candidate : devices) {
            std::uint32_t graphics = 0;
            std::uint32_t present = 0;
            if (!queue_families_for(candidate, graphics, present) ||
                !device_supports_swapchain(candidate)) {
                continue;
            }
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            int score = 0;
            if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 400;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 300;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 200;
            else if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) score = 100;
            if (score > best_score) {
                best_score = score;
                physical_device = candidate;
                graphics_family = graphics;
                present_family = present;
                transfer_family = graphics;
                std::uint32_t family_count = 0U;
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
                std::vector<VkQueueFamilyProperties> families(family_count);
                vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
                for (std::uint32_t family = 0U; family < family_count; ++family) {
                    if (families[family].queueCount != 0U &&
                        (families[family].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0U &&
                        (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT |
                                                        VK_QUEUE_COMPUTE_BIT)) == 0U) {
                        transfer_family = family;
                        break;
                    }
                }
                selected_device_name = properties.deviceName;
                device_api_version = std::min(properties.apiVersion, instance_api_version);
                timestamp_period_nanoseconds = properties.limits.timestampPeriod;
            }
        }
        if (physical_device == VK_NULL_HANDLE) {
            last_error = "no Vulkan device supports graphics, presentation and swapchains";
            return false;
        }
        return true;
    }

    bool create_logical_device() {
        constexpr float priority = 1.0F;
        const std::set<std::uint32_t> unique_families{graphics_family, present_family,
                                                      transfer_family};
        std::vector<VkDeviceQueueCreateInfo> queue_infos;
        for (const auto family : unique_families) {
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = family;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            queue_infos.push_back(queue_info);
        }

        std::uint32_t available_count = 0;
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &available_count, nullptr);
        std::vector<VkExtensionProperties> available(available_count);
        vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &available_count, available.data());
        std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (extension_available(available, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
#endif

        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
        create_info.pQueueCreateInfos = queue_infos.data();
        create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        VkPhysicalDeviceFeatures supported_features{};
        vkGetPhysicalDeviceFeatures(physical_device, &supported_features);
        if (supported_features.shaderSampledImageArrayDynamicIndexing != VK_TRUE) {
            last_error = "Vulkan device does not support dynamically indexed sampled-image arrays";
            return false;
        }
        VkPhysicalDeviceProperties device_properties{};
        vkGetPhysicalDeviceProperties(physical_device, &device_properties);
        constexpr std::uint32_t required_fragment_samplers =
            bindless_texture_capacity + static_cast<std::uint32_t>(shadow_map_count) + 1U;
        if (device_properties.limits.maxPerStageDescriptorSamplers < required_fragment_samplers ||
            device_properties.limits.maxPerStageDescriptorSampledImages <
                required_fragment_samplers ||
            device_properties.limits.maxDescriptorSetSamplers < required_fragment_samplers ||
            device_properties.limits.maxDescriptorSetSampledImages < required_fragment_samplers) {
            last_error = "Vulkan device cannot bind the texture table and shadow maps together";
            return false;
        }
        if (device_properties.limits.maxColorAttachments < geometry_color_attachments) {
            last_error = "Vulkan device cannot render the scene's G-buffer (needs " +
                         std::to_string(geometry_color_attachments) + " color attachments)";
            return false;
        }
        VkPhysicalDeviceFeatures2 enabled_features{};
        enabled_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        enabled_features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        VkPhysicalDeviceVulkan11Features enabled11{};
        enabled11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceVulkan12Features enabled12{};
        enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features enabled13{};
        enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration{};
        enabled_acceleration.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        VkPhysicalDeviceRayQueryFeaturesKHR enabled_ray_query{};
        enabled_ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
        if (device_api_version >= VK_API_VERSION_1_3) {
            VkPhysicalDeviceVulkan11Features supported11{};
            supported11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            VkPhysicalDeviceVulkan12Features supported12{};
            supported12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            VkPhysicalDeviceVulkan13Features supported13{};
            supported13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceAccelerationStructureFeaturesKHR supported_acceleration{};
            supported_acceleration.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
            VkPhysicalDeviceRayQueryFeaturesKHR supported_ray_query{};
            supported_ray_query.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
            const bool ray_extensions =
                extension_available(available, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
                extension_available(available, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
                extension_available(available, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            VkPhysicalDeviceFeatures2 supported{};
            supported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            supported.pNext = &supported11;
            supported11.pNext = &supported12;
            supported12.pNext = &supported13;
            if (ray_extensions) {
                supported13.pNext = &supported_acceleration;
                supported_acceleration.pNext = &supported_ray_query;
            }
            vkGetPhysicalDeviceFeatures2(physical_device, &supported);
            lighting_features = supported.features.shaderInt64 &&
                                supported12.descriptorBindingPartiallyBound &&
                                supported11.storageBuffer16BitAccess &&
                                supported12.shaderFloat16 &&
                                supported12.shaderSubgroupExtendedTypes &&
                                supported13.subgroupSizeControl &&
                                supported13.computeFullSubgroups;
            if (lighting_features) {
                enabled_features.features.shaderInt64 = VK_TRUE;
                enabled_features.features.shaderInt16 = supported.features.shaderInt16;
                enabled11.storageBuffer16BitAccess = VK_TRUE;
                enabled12.shaderFloat16 = VK_TRUE;
                enabled12.shaderSubgroupExtendedTypes = VK_TRUE;
                // Brixelizer binds a partially filled table of mesh buffers.
                enabled12.descriptorBindingPartiallyBound = VK_TRUE;
                enabled12.shaderBufferInt64Atomics = supported12.shaderBufferInt64Atomics;
                enabled12.shaderStorageBufferArrayNonUniformIndexing =
                    supported12.shaderStorageBufferArrayNonUniformIndexing;
                enabled13.subgroupSizeControl = VK_TRUE;
                enabled13.computeFullSubgroups = VK_TRUE;
            }
            ray_query_features = lighting_features && ray_extensions &&
                                 supported12.bufferDeviceAddress &&
                                 supported12.shaderSampledImageArrayNonUniformIndexing &&
                                 supported_acceleration.accelerationStructure &&
                                 supported_ray_query.rayQuery;
            if (ray_query_features) {
                enabled12.bufferDeviceAddress = VK_TRUE;
                // Reflection hits index the texture table per ray.
                enabled12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
                enabled_acceleration.accelerationStructure = VK_TRUE;
                enabled_ray_query.rayQuery = VK_TRUE;
                extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
                extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            }
            enabled_features.pNext = &enabled11;
            enabled11.pNext = &enabled12;
            enabled12.pNext = &enabled13;
            if (ray_query_features) {
                enabled13.pNext = &enabled_acceleration;
                enabled_acceleration.pNext = &enabled_ray_query;
            }
        }
        create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        if (device_api_version >= VK_API_VERSION_1_1)
            create_info.pNext = &enabled_features;
        else
            create_info.pEnabledFeatures = &enabled_features.features;
        const auto result = vkCreateDevice(physical_device, &create_info, nullptr, &device);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateDevice", result);
            return false;
        }
        vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, present_family, 0, &present_queue);
        vkGetDeviceQueue(device, transfer_family, 0, &transfer_queue);
        return true;
    }

    bool create_command_pool() {
        VkCommandPoolCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        create_info.queueFamilyIndex = graphics_family;
        auto result = vkCreateCommandPool(device, &create_info, nullptr, &command_pool);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateCommandPool", result);
            return false;
        }
        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = static_cast<std::uint32_t>(command_buffers.size());
        result = vkAllocateCommandBuffers(device, &allocate_info, command_buffers.data());
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkAllocateCommandBuffers", result);
            return false;
        }
        if (transfer_family != graphics_family) {
            create_info.queueFamilyIndex = transfer_family;
            result = vkCreateCommandPool(device, &create_info, nullptr, &transfer_command_pool);
            if (result != VK_SUCCESS) {
                last_error = vk_error("transfer command pool creation", result);
                return false;
            }
        }
        return true;
    }

    bool create_buffer(const VkDeviceSize size, const VkBufferUsageFlags usage,
                       const VkMemoryPropertyFlags properties, VkBuffer& buffer,
                       VkDeviceMemory& memory) {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const std::array queue_indices{graphics_family, transfer_family};
        if (transfer_family != graphics_family &&
            (usage & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) != 0U) {
            buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            buffer_info.queueFamilyIndexCount = 2U;
            buffer_info.pQueueFamilyIndices = queue_indices.data();
        }
        auto result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            last_error = vk_error("mesh buffer creation", result);
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const auto memory_type = find_memory_type(requirements.memoryTypeBits, properties);
        if (!memory_type.has_value()) {
            last_error = "no compatible Vulkan memory type is available for mesh assets";
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        VkMemoryAllocateFlagsInfo address_flags{};
        address_flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        address_flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if ((usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0U)
            allocation.pNext = &address_flags;
        result = vkAllocateMemory(device, &allocation, nullptr, &memory);
        if (result == VK_SUCCESS) result = vkBindBufferMemory(device, buffer, memory, 0U);
        if (result != VK_SUCCESS) {
            last_error = vk_error("mesh buffer allocation", result);
            vkFreeMemory(device, memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            memory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void cleanup_upload_batch() {
        if (device == VK_NULL_HANDLE) return;
        for (const auto buffer : upload_staging_buffers)
            vkDestroyBuffer(device, buffer, nullptr);
        for (const auto memory : upload_staging_memories)
            vkFreeMemory(device, memory, nullptr);
        upload_staging_buffers.clear();
        upload_staging_memories.clear();
        upload_staging_bytes = 0U;
        const auto upload_pool = transfer_command_pool != VK_NULL_HANDLE
                                     ? transfer_command_pool : command_pool;
        if (upload_commands != VK_NULL_HANDLE && upload_pool != VK_NULL_HANDLE)
            vkFreeCommandBuffers(device, upload_pool, 1U, &upload_commands);
        upload_commands = VK_NULL_HANDLE;
        vkDestroyFence(device, upload_fence, nullptr);
        upload_fence = VK_NULL_HANDLE;
    }

    AssetResources release_active_assets() {
        AssetResources released{mesh_vertex_buffer, mesh_index_buffer, material_buffer,
                                mesh_vertex_memory, mesh_index_memory, material_memory,
                                std::move(textures), texture_pool, texture_sets,
                                mesh_vertex_capacity, mesh_index_capacity, material_capacity,
                                uploaded_vertex_count, uploaded_index_count,
                                uploaded_material_count, uploaded_texture_count,
                                uploaded_asset_revision, active_device_estimate};
        mesh_vertex_buffer = mesh_index_buffer = material_buffer = VK_NULL_HANDLE;
        mesh_vertex_memory = mesh_index_memory = material_memory = VK_NULL_HANDLE;
        texture_pool = VK_NULL_HANDLE;
        texture_sets.fill(VK_NULL_HANDLE);
        mesh_vertex_capacity = mesh_index_capacity = material_capacity = 0U;
        uploaded_vertex_count = uploaded_index_count = 0U;
        uploaded_material_count = uploaded_texture_count = 0U;
        uploaded_asset_revision = 0U;
        active_device_estimate = 0U;
        return released;
    }

    void restore_active_assets(AssetResources resources) {
        mesh_vertex_buffer = resources.vertex_buffer;
        mesh_index_buffer = resources.index_buffer;
        material_buffer = resources.material_buffer;
        mesh_vertex_memory = resources.vertex_memory;
        mesh_index_memory = resources.index_memory;
        material_memory = resources.material_memory;
        textures = std::move(resources.textures);
        texture_pool = resources.descriptor_pool;
        texture_sets = resources.descriptor_sets;
        mesh_vertex_capacity = resources.vertex_capacity;
        mesh_index_capacity = resources.index_capacity;
        material_capacity = resources.material_capacity;
        uploaded_vertex_count = resources.vertex_count;
        uploaded_index_count = resources.index_count;
        uploaded_material_count = resources.material_count;
        uploaded_texture_count = resources.texture_count;
        uploaded_asset_revision = resources.revision;
        active_device_estimate = resources.device_estimate;
    }

    void destroy_asset_resources(const AssetResources& resources) {
        vkDestroyBuffer(device, resources.index_buffer, nullptr);
        vkFreeMemory(device, resources.index_memory, nullptr);
        vkDestroyBuffer(device, resources.vertex_buffer, nullptr);
        vkFreeMemory(device, resources.vertex_memory, nullptr);
        vkDestroyBuffer(device, resources.material_buffer, nullptr);
        vkFreeMemory(device, resources.material_memory, nullptr);
        vkDestroyDescriptorPool(device, resources.descriptor_pool, nullptr);
        for (const auto& texture : resources.textures) {
            vkDestroySampler(device, texture.sampler, nullptr);
            vkDestroyImageView(device, texture.view, nullptr);
            vkDestroyImage(device, texture.image, nullptr);
            vkFreeMemory(device, texture.memory, nullptr);
        }
    }

    bool collect_upload_batch() {
        if (upload_fence == VK_NULL_HANDLE) return true;
        const auto result = vkGetFenceStatus(device, upload_fence);
        if (result == VK_NOT_READY) return true;
        if (result != VK_SUCCESS) {
            last_error = vk_error("checking asset upload batch", result);
            return false;
        }
        if (upload_wait_pending) return true;
        if (retired_assets.has_value() && transfer_family != graphics_family) {
            for (std::size_t frame = 0U; frame < frames_in_flight; ++frame) {
                if (!old_asset_frame_pending[frame]) continue;
                const auto frame_result = vkGetFenceStatus(device, frame_fences[frame]);
                if (frame_result == VK_NOT_READY) return true;
                if (frame_result != VK_SUCCESS) {
                    last_error = vk_error("checking retired asset frame", frame_result);
                    return false;
                }
                old_asset_frame_pending[frame] = false;
            }
        }
        cleanup_upload_batch();
        if (retired_assets.has_value()) {
            destroy_asset_resources(*retired_assets);
            retired_assets.reset();
        }
        return true;
    }

    bool begin_upload_batch() {
        cleanup_upload_batch();
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        auto result = vkCreateFence(device, &fence_info, nullptr, &upload_fence);
        VkCommandBufferAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocation.commandPool = transfer_command_pool != VK_NULL_HANDLE
                                     ? transfer_command_pool : command_pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1U;
        if (result == VK_SUCCESS)
            result = vkAllocateCommandBuffers(device, &allocation, &upload_commands);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (result == VK_SUCCESS) result = vkBeginCommandBuffer(upload_commands, &begin);
        if (result != VK_SUCCESS) {
            last_error = vk_error("starting asset upload batch", result);
            cleanup_upload_batch();
            return false;
        }
        return true;
    }

    bool may_stage(const VkDeviceSize size) {
        if (size > upload_budget.staging_bytes - upload_staging_bytes) {
            last_error = "asset upload exceeds the staging memory budget";
            ++rejected_uploads;
            return false;
        }
        return true;
    }

    void staged(const VkDeviceSize size) {
        upload_staging_bytes += size;
        peak_upload_staging_bytes = std::max(peak_upload_staging_bytes,
                                             upload_staging_bytes);
    }

    bool preflight(const UploadEstimate& estimate, const std::uint64_t resident = 0U) {
        last_error = check_upload_budget(estimate, upload_budget, resident);
        if (!last_error.empty()) {
            ++rejected_uploads;
            return false;
        }
        return true;
    }

    bool finish_upload_batch(const bool wait = true) {
        auto result = vkEndCommandBuffer(upload_commands);
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &upload_commands;
            if (transfer_family != graphics_family) {
                VkSemaphoreCreateInfo semaphore_info{};
                semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                result = vkCreateSemaphore(device, &semaphore_info, nullptr, &upload_complete);
                if (result == VK_SUCCESS) {
                    submit.signalSemaphoreCount = 1U;
                    submit.pSignalSemaphores = &upload_complete;
                }
            }
            if (result == VK_SUCCESS)
                result = vkQueueSubmit(transfer_queue, 1U, &submit, upload_fence);
            if (result == VK_SUCCESS && transfer_family != graphics_family)
                upload_wait_pending = true;
        }
        if (result == VK_SUCCESS && wait)
            result = vkWaitForFences(device, 1U, &upload_fence, VK_TRUE,
                                     std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS)
            last_error = vk_error("asset upload batch", result);
        if (result == VK_SUCCESS) {
            last_upload_bytes = upload_staging_bytes;
            ++upload_batches;
        }
        if (wait || result != VK_SUCCESS) {
            cleanup_upload_batch();
            if (result != VK_SUCCESS) {
                vkDestroySemaphore(device, upload_complete, nullptr);
                upload_complete = VK_NULL_HANDLE;
                upload_wait_pending = false;
            }
        }
        return result == VK_SUCCESS;
    }

    bool upload_buffer(const void* data, const VkDeviceSize size,
                       const VkBufferUsageFlags final_usage, VkBuffer& destination,
                       VkDeviceMemory& destination_memory,
                       const VkDeviceSize allocation_size = 0U) {
        VkBuffer staging{};
        VkDeviceMemory staging_memory{};
        if (!may_stage(size)) return false;
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging, staging_memory)) return false;
        void* mapped = nullptr;
        auto result = vkMapMemory(device, staging_memory, 0U, size, 0U, &mapped);
        if (result == VK_SUCCESS) {
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            vkUnmapMemory(device, staging_memory);
        }
        if (result == VK_SUCCESS &&
            !create_buffer(std::max(size, allocation_size),
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT | final_usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, destination, destination_memory)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        if (result == VK_SUCCESS && upload_commands == VK_NULL_HANDLE)
            result = VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            VkBufferCopy copy{};
            copy.size = size;
            vkCmdCopyBuffer(upload_commands, staging, destination, 1U, &copy);
            upload_staging_buffers.push_back(staging);
            upload_staging_memories.push_back(staging_memory);
            staged(size);
        } else {
            vkDestroyBuffer(device, staging, nullptr);
            vkFreeMemory(device, staging_memory, nullptr);
        }
        if (result != VK_SUCCESS) {
            last_error = vk_error("recording buffer asset upload", result);
            return false;
        }
        return true;
    }

    bool upload_buffer_region(const void* data, const VkDeviceSize size,
                              const VkBuffer destination, const VkDeviceSize destination_offset) {
        if (size == 0U) return true;
        VkBuffer staging{};
        VkDeviceMemory staging_memory{};
        if (!may_stage(size)) return false;
        if (!create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging, staging_memory))
            return false;
        void* mapped = nullptr;
        auto result = vkMapMemory(device, staging_memory, 0U, size, 0U, &mapped);
        if (result == VK_SUCCESS) {
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            vkUnmapMemory(device, staging_memory);
        }
        if (result == VK_SUCCESS && upload_commands == VK_NULL_HANDLE)
            result = VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            VkBufferCopy copy{};
            copy.dstOffset = destination_offset;
            copy.size = size;
            vkCmdCopyBuffer(upload_commands, staging, destination, 1U, &copy);
            upload_staging_buffers.push_back(staging);
            upload_staging_memories.push_back(staging_memory);
            staged(size);
            return true;
        }
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, staging_memory, nullptr);
        last_error = vk_error("recording incremental buffer upload", result);
        return false;
    }

    static VkDeviceSize growing_capacity(const VkDeviceSize used) {
        VkDeviceSize capacity = 1U;
        while (capacity <= used && capacity <= std::numeric_limits<VkDeviceSize>::max() / 2U)
            capacity *= 2U;
        return std::max(capacity, used);
    }

    VkBufferUsageFlags geometry_buffer_usage() const {
        VkBufferUsageFlags usage = 0U;
        if (lighting_features) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (ray_query_features)
            usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        return usage;
    }

    bool create_mesh_buffers() {
        const auto vertices = assets->mesh_vertices();
        const auto indices = assets->mesh_indices();
        mesh_vertex_capacity = growing_capacity(vertices.size_bytes());
        mesh_index_capacity = growing_capacity(indices.size_bytes());
        uploaded_vertex_count = vertices.size();
        uploaded_index_count = indices.size();
        // Lighting effects read the shared geometry buffers from compute shaders, and ray
        // tracing builds acceleration structures from them.
        const VkBufferUsageFlags storage = geometry_buffer_usage();
        return upload_buffer(vertices.data(), vertices.size_bytes(),
                             VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | storage,
                             mesh_vertex_buffer, mesh_vertex_memory, mesh_vertex_capacity) &&
               upload_buffer(indices.data(), indices.size_bytes(),
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | storage,
                             mesh_index_buffer, mesh_index_memory, mesh_index_capacity);
    }

    bool refresh_mesh_assets() {
        if (uploaded_asset_revision == assets->revision()) return true;
        // Coalesce revisions that arrive while a batch is in flight. The next frame after its fence
        // signals will submit one replacement containing the newest complete registry state.
        if (upload_fence != VK_NULL_HANDLE || upload_wait_pending) return true;
        const auto vertices = assets->mesh_vertices();
        const auto indices = assets->mesh_indices();
        const auto vertex_bytes = vertices.size_bytes();
        const auto index_bytes = indices.size_bytes();
        const auto material_bytes = assets->materials().size() * sizeof(GpuMaterial);
        const bool incremental_append =
            assets->materials().size() >= uploaded_material_count &&
            assets->textures().size() == uploaded_texture_count &&
            vertices.size() >= uploaded_vertex_count && indices.size() >= uploaded_index_count &&
            vertex_bytes <= mesh_vertex_capacity && index_bytes <= mesh_index_capacity &&
            material_bytes <= material_capacity;
        if (incremental_append) {
            const auto new_vertex_bytes = vertex_bytes -
                uploaded_vertex_count * sizeof(MeshVertex);
            const auto new_index_bytes = index_bytes -
                uploaded_index_count * sizeof(std::uint32_t);
            const auto new_material_bytes = material_bytes -
                uploaded_material_count * sizeof(GpuMaterial);
            if (!preflight(estimate_asset_upload(
                    *assets, assets->textures().size(), 0U, 0U, 0U,
                    new_vertex_bytes + new_index_bytes + new_material_bytes),
                    active_device_estimate)) return false;
            if (!begin_upload_batch()) return false;
            const auto new_vertices = vertices.subspan(uploaded_vertex_count);
            const auto new_indices = indices.subspan(uploaded_index_count);
            const auto new_materials = make_gpu_materials(uploaded_material_count);
            const bool recorded =
                upload_buffer_region(new_vertices.data(), new_vertices.size_bytes(),
                                     mesh_vertex_buffer,
                                     uploaded_vertex_count * sizeof(MeshVertex)) &&
                upload_buffer_region(new_indices.data(), new_indices.size_bytes(),
                                     mesh_index_buffer,
                                     uploaded_index_count * sizeof(std::uint32_t)) &&
                upload_buffer_region(new_materials.data(),
                                     new_materials.size() * sizeof(GpuMaterial), material_buffer,
                                     uploaded_material_count * sizeof(GpuMaterial));
            if (!recorded) {
                cleanup_upload_batch();
                return false;
            }
            if (!finish_upload_batch(false)) return false;
            uploaded_vertex_count = vertices.size();
            uploaded_index_count = indices.size();
            uploaded_material_count = assets->materials().size();
            uploaded_asset_revision = assets->revision();
            return true;
        }
        const auto candidate = estimate_asset_upload(
            *assets, textures.size(), growing_capacity(vertex_bytes),
            growing_capacity(index_bytes), growing_capacity(material_bytes),
            vertex_bytes + index_bytes + material_bytes);
        if (!preflight(candidate, active_device_estimate)) return false;
        const auto reused_texture_bytes = active_device_estimate -
            (mesh_vertex_capacity + mesh_index_capacity + material_capacity);
        if (!begin_upload_batch()) return false;
        auto previous = release_active_assets();
        // AssetRegistry is append-only. Copy the old handles into the candidate descriptor table;
        // ownership remains with `previous` until submission succeeds.
        const auto reused_texture_count = previous.textures.size();
        textures = previous.textures;
        const auto destroy_incomplete = [&] {
            auto incomplete = release_active_assets();
            incomplete.textures.erase(
                incomplete.textures.begin(),
                incomplete.textures.begin() + static_cast<std::ptrdiff_t>(
                                                  std::min(reused_texture_count,
                                                           incomplete.textures.size())));
            destroy_asset_resources(incomplete);
        };
        if (!(create_mesh_buffers() && create_texture_resources())) {
            cleanup_upload_batch();
            destroy_incomplete();
            restore_active_assets(std::move(previous));
            return false;
        }
        if (!finish_upload_batch(false)) {
            destroy_incomplete();
            restore_active_assets(std::move(previous));
            return false;
        }
        // The upload was enqueued after every frame that can reference `previous`. Later draws are
        // enqueued after the upload, so the new handles are safe immediately; the fence covers both
        // upload completion and the last possible use of the retired set.
        previous.textures.clear();
        retired_assets = std::move(previous);
        if (transfer_family != graphics_family) {
            for (std::size_t frame = 0U; frame < frames_in_flight; ++frame) {
                old_asset_frame_pending[frame] =
                    vkGetFenceStatus(device, frame_fences[frame]) != VK_SUCCESS;
            }
        }
        active_device_estimate = reused_texture_bytes + candidate.device_bytes;
        return true;
    }

    bool create_texture_image(const TextureAsset& asset, GpuTexture& texture) {
        texture.mip_levels = static_cast<std::uint32_t>(
                                 std::floor(std::log2(static_cast<double>(
                                     std::max(asset.width, asset.height))))) + 1U;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        const auto image_format = asset.color_space == TextureColorSpace::srgb
                                      ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, image_format, &format_properties);
        constexpr VkFormatFeatureFlags required_format_features =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((format_properties.optimalTilingFeatures & required_format_features) !=
            required_format_features) {
            last_error = "RGBA8 texture format does not support linear blit mip generation";
            return false;
        }
        image_info.format = image_format;
        image_info.extent = {asset.width, asset.height, 1U};
        image_info.mipLevels = texture.mip_levels;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        const std::array texture_queue_indices{graphics_family, transfer_family};
        if (transfer_family != graphics_family) {
            image_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            image_info.queueFamilyIndexCount = 2U;
            image_info.pQueueFamilyIndices = texture_queue_indices.data();
        }
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        auto result = vkCreateImage(device, &image_info, nullptr, &texture.image);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture image creation", result);
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, texture.image, &requirements);
        const auto memory_type = find_memory_type(requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type.has_value()) {
            last_error = "no device-local memory type is available for texture assets";
            return false;
        }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &texture.memory);
        if (result == VK_SUCCESS) result = vkBindImageMemory(device, texture.image, texture.memory, 0U);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture image allocation", result);
            return false;
        }

        VkBuffer staging{};
        VkDeviceMemory staging_memory{};
        const auto byte_count = static_cast<VkDeviceSize>(asset.rgba.size());
        if (!may_stage(byte_count)) return false;
        if (!create_buffer(byte_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging, staging_memory)) return false;
        void* mapped = nullptr;
        result = vkMapMemory(device, staging_memory, 0U, byte_count, 0U, &mapped);
        if (result == VK_SUCCESS) {
            std::memcpy(mapped, asset.rgba.data(), asset.rgba.size());
            vkUnmapMemory(device, staging_memory);
        }
        if (result == VK_SUCCESS && upload_commands == VK_NULL_HANDLE)
            result = VK_ERROR_INITIALIZATION_FAILED;
        if (result == VK_SUCCESS) {
            VkImageMemoryBarrier initial{};
            initial.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            initial.srcAccessMask = 0U;
            initial.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            initial.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            initial.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            initial.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            initial.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            initial.image = texture.image;
            initial.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            initial.subresourceRange.levelCount = texture.mip_levels;
            initial.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr,
                                 1U, &initial);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {asset.width, asset.height, 1U};
            vkCmdCopyBufferToImage(upload_commands, staging, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &copy);
            std::int32_t mip_width = static_cast<std::int32_t>(asset.width);
            std::int32_t mip_height = static_cast<std::int32_t>(asset.height);
            for (std::uint32_t level = 1U; level < texture.mip_levels; ++level) {
                VkImageMemoryBarrier source{};
                source.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                source.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                source.image = texture.image;
                source.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                source.subresourceRange.baseMipLevel = level - 1U;
                source.subresourceRange.levelCount = 1U;
                source.subresourceRange.layerCount = 1U;
                vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr,
                                     1U, &source);
                VkImageBlit blit{};
                blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.srcSubresource.mipLevel = level - 1U;
                blit.srcSubresource.layerCount = 1U;
                blit.srcOffsets[1] = {mip_width, mip_height, 1};
                blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                blit.dstSubresource.mipLevel = level;
                blit.dstSubresource.layerCount = 1U;
                blit.dstOffsets[1] = {std::max(mip_width / 2, 1), std::max(mip_height / 2, 1), 1};
                vkCmdBlitImage(upload_commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit,
                               VK_FILTER_LINEAR);
                source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                source.dstAccessMask = transfer_family == graphics_family
                                           ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
                                           : 0U;
                source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                source.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     transfer_family == graphics_family
                                         ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                         : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0U, 0U, nullptr,
                                     0U, nullptr, 1U, &source);
                mip_width = std::max(mip_width / 2, 1);
                mip_height = std::max(mip_height / 2, 1);
            }
            VkImageMemoryBarrier last{};
            last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            last.dstAccessMask = transfer_family == graphics_family
                                     ? static_cast<VkAccessFlags>(VK_ACCESS_SHADER_READ_BIT)
                                     : 0U;
            last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            last.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            last.image = texture.image;
            last.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            last.subresourceRange.baseMipLevel = texture.mip_levels - 1U;
            last.subresourceRange.levelCount = 1U;
            last.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(upload_commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 transfer_family == graphics_family
                                     ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                     : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0U, 0U, nullptr, 0U,
                                 nullptr, 1U, &last);
        }
        if (result == VK_SUCCESS) {
            upload_staging_buffers.push_back(staging);
            upload_staging_memories.push_back(staging_memory);
            staged(byte_count);
        } else {
            vkDestroyBuffer(device, staging, nullptr);
            vkFreeMemory(device, staging_memory, nullptr);
        }
        if (result != VK_SUCCESS) {
            last_error = vk_error("recording texture upload and mip generation", result);
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = image_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = texture.mip_levels;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &texture.view);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture image view creation", result);
            return false;
        }
        const auto filter = [](const TextureFilter value) {
            return value == TextureFilter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        };
        const auto wrap = [](const TextureWrap value) {
            switch (value) {
            case TextureWrap::clamp_to_edge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case TextureWrap::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case TextureWrap::repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            }
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        };
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = filter(asset.mag_filter);
        sampler_info.minFilter = filter(asset.min_filter);
        sampler_info.mipmapMode = asset.mip_filter == TextureFilter::nearest
                                      ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                      : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = wrap(asset.wrap_u);
        sampler_info.addressModeV = wrap(asset.wrap_v);
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = static_cast<float>(texture.mip_levels - 1U);
        result = vkCreateSampler(device, &sampler_info, nullptr, &texture.sampler);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture sampler creation", result);
            return false;
        }
        return true;
    }

    std::vector<GpuMaterial> make_gpu_materials(const std::size_t first = 0U) const {
        const auto missing_texture = std::numeric_limits<std::uint32_t>::max();
        const auto texture_slot = [&](const std::string& name) {
            return !name.empty() && assets->find_texture(name) != nullptr
                       ? assets->texture_index(name) : missing_texture;
        };
        std::vector<GpuMaterial> gpu_materials;
        const auto materials = assets->materials();
        gpu_materials.reserve(materials.size() - std::min(first, materials.size()));
        for (const auto& material : materials.subspan(std::min(first, materials.size()))) {
            const auto occlusion = texture_slot(material.occlusion_texture);
            const auto emissive = texture_slot(material.emissive_texture);
            const auto packed = (occlusion == missing_texture ? 0xFFU : occlusion) |
                                ((emissive == missing_texture ? 0xFFU : emissive) << 8U) |
                                (static_cast<std::uint32_t>(material.alpha_mode) << 16U) |
                                (static_cast<std::uint32_t>(material.double_sided) << 18U);
            gpu_materials.push_back({
                material.color,
                {material.emissive_factor[0], material.emissive_factor[1],
                 material.emissive_factor[2], material.metallic_factor},
                {material.roughness_factor, material.normal_scale,
                 material.occlusion_strength, material.alpha_cutoff},
                {texture_slot(material.texture), texture_slot(material.metallic_roughness_texture),
                 texture_slot(material.normal_texture), packed}});
        }
        return gpu_materials;
    }

    bool create_texture_resources() {
        const auto texture_assets = assets->textures();
        if (texture_assets.empty() || texture_assets.size() > bindless_texture_capacity) {
            last_error = "built-in textures exceed the bindless table capacity";
            return false;
        }
        const auto first_new_texture = textures.size();
        textures.resize(texture_assets.size());
        for (std::size_t index = first_new_texture; index < texture_assets.size(); ++index) {
            if (!create_texture_image(texture_assets[index], textures[index])) return false;
        }
        const auto gpu_materials = make_gpu_materials();
        material_capacity = growing_capacity(gpu_materials.size() * sizeof(GpuMaterial));
        if (gpu_materials.empty() ||
            !upload_buffer(gpu_materials.data(), gpu_materials.size() * sizeof(GpuMaterial),
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, material_buffer, material_memory,
                           material_capacity)) {
            return false;
        }
        auto result = VK_SUCCESS;
        if (texture_layout == VK_NULL_HANDLE) {
            std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
            bindings[0].binding = 0U;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = bindless_texture_capacity;
            // Compute sees the table too: reflection hits shade with the same materials and lights.
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[1].binding = 1U;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[1].descriptorCount = 1U;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[2] = bindings[1];
            bindings[2].binding = 2U;
            bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                     VK_SHADER_STAGE_COMPUTE_BIT;
            bindings[3] = bindings[0];
            bindings[3].binding = 3U;
            bindings[3].descriptorCount = shadow_map_count;
            bindings[4] = bindings[0];
            bindings[4].binding = 4U;
            bindings[4].descriptorCount = 1U;
            // Per-draw data for the scene shaders, rewritten every frame.
            bindings[5] = bindings[1];
            bindings[5].binding = 5U;
            bindings[5].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            VkDescriptorSetLayoutCreateInfo layout_info{};
            layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
            layout_info.pBindings = bindings.data();
            result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &texture_layout);
        }
        const std::array pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 (bindless_texture_capacity + shadow_map_count + 1U) *
                                     frames_in_flight},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3U * frames_in_flight}};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = frames_in_flight;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        if (result == VK_SUCCESS) result = vkCreateDescriptorPool(device, &pool_info, nullptr, &texture_pool);
        VkDescriptorSetAllocateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_info.descriptorPool = texture_pool;
        std::array<VkDescriptorSetLayout, frames_in_flight> layouts{};
        layouts.fill(texture_layout);
        set_info.descriptorSetCount = frames_in_flight;
        set_info.pSetLayouts = layouts.data();
        if (result == VK_SUCCESS)
            result = vkAllocateDescriptorSets(device, &set_info, texture_sets.data());
        if (result != VK_SUCCESS) {
            last_error = vk_error("bindless texture descriptor allocation", result);
            return false;
        }
        std::array<VkDescriptorImageInfo, bindless_texture_capacity> image_infos{};
        for (std::size_t index = 0; index < image_infos.size(); ++index) {
            const auto& texture = textures[index < textures.size() ? index : 0U];
            image_infos[index].sampler = texture.sampler;
            image_infos[index].imageView = texture.view;
            image_infos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkDescriptorBufferInfo material_info{};
        material_info.buffer = material_buffer;
        material_info.range = material_capacity;
        std::array<VkWriteDescriptorSet, 5> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstBinding = 0U;
        writes[0].descriptorCount = bindless_texture_capacity;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = image_infos.data();
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstBinding = 1U;
        writes[1].descriptorCount = 1U;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &material_info;
        for (std::size_t f = 0; f < frames_in_flight; ++f) {
            if (!lighting_buffers[f] &&
                !create_buffer(sizeof(GpuLighting), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               lighting_buffers[f], lighting_memories[f]))
                return false;
            VkDescriptorBufferInfo lighting_info{lighting_buffers[f], 0, sizeof(GpuLighting)};
            writes[2] = writes[1];
            writes[2].dstBinding = 2;
            writes[2].pBufferInfo = &lighting_info;
            std::array<VkDescriptorImageInfo, shadow_map_count> shadow_infos{};
            if (shadow_sampler != VK_NULL_HANDLE) {
                for (std::size_t map = 0; map < shadow_map_count; ++map) {
                    auto& shadow_info = shadow_infos[map];
                    shadow_info.sampler = shadow_sampler;
                    shadow_info.imageView =
                        shadow_attachments[f * shadow_map_count + map].view;
                    shadow_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                }
                writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[3].dstBinding = 3U;
                writes[3].descriptorCount = shadow_map_count;
                writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[3].pImageInfo = shadow_infos.data();
            }
            VkDescriptorImageInfo point_shadow_info{};
            if (point_shadow_sampler != VK_NULL_HANDLE) {
                point_shadow_info.sampler = point_shadow_sampler;
                point_shadow_info.imageView = point_shadow_attachments[f].cube_view;
                point_shadow_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[4].dstBinding = 4U;
                writes[4].descriptorCount = 1U;
                writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[4].pImageInfo = &point_shadow_info;
            }
            for (auto &write : writes)
                write.dstSet = texture_sets[f];
            vkUpdateDescriptorSets(device, point_shadow_sampler != VK_NULL_HANDLE ? 5U :
                                           shadow_sampler != VK_NULL_HANDLE ? 4U : 3U,
                                   writes.data(), 0U, nullptr);
        }
        uploaded_material_count = assets->materials().size();
        uploaded_texture_count = assets->textures().size();
        uploaded_asset_revision = assets->revision();
        return true;
    }

    bool create_timestamp_pool() {
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
        if (graphics_family >= family_count || families[graphics_family].timestampValidBits == 0U) return true;
        VkQueryPoolCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        create_info.queryCount = static_cast<std::uint32_t>(frames_in_flight) * gpu_marker_capacity;
        const auto result = vkCreateQueryPool(device, &create_info, nullptr, &timestamp_queries);
        if (result != VK_SUCCESS) {
            last_error = vk_error("timestamp query pool creation", result);
            return false;
        }
        return true;
    }

    VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) const {
        const auto preferred = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
            return format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                   format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        return preferred != formats.end() ? *preferred : formats.front();
    }

    // FIFO is vsync and always available. Without vsync, immediate presentation shows each frame
    // as soon as it is ready and may tear. Mailbox never tears but discards frames the display had
    // no time to show, which a loop not locked to the display sees as a stutter a few times a
    // second, so it is only the fallback.
    VkPresentModeKHR choose_present_mode() const {
        if (vsync_requested) return VK_PRESENT_MODE_FIFO_KHR;
        std::uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count, modes.data());
        for (const auto wanted : {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR})
            if (std::find(modes.begin(), modes.end(), wanted) != modes.end()) return wanted;
        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities) const {
        if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
            return capabilities.currentExtent;
        }
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        return {
            std::clamp(static_cast<std::uint32_t>(std::max(width, 0)),
                       capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(static_cast<std::uint32_t>(std::max(height, 0)),
                       capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
        };
    }

    bool create_swapchain() {
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &capabilities);
        std::uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &format_count, formats.data());

        const auto format = choose_surface_format(formats);
        const auto present_mode = choose_present_mode();
        present_mode_in_use = present_mode;
        const auto extent = choose_extent(capabilities);
        if (extent.width == 0 || extent.height == 0) return true;
        auto image_count = capabilities.minImageCount + 1U;
        if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
            image_count = capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface;
        create_info.minImageCount = image_count;
        create_info.imageFormat = format.format;
        create_info.imageColorSpace = format.colorSpace;
        create_info.imageExtent = extent;
        create_info.imageArrayLayers = 1;
        transfer_source_supported =
            (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 (transfer_source_supported ? static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : 0U);
        const std::array queue_indices{graphics_family, present_family};
        if (graphics_family != present_family) {
            create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            create_info.queueFamilyIndexCount = static_cast<std::uint32_t>(queue_indices.size());
            create_info.pQueueFamilyIndices = queue_indices.data();
        } else {
            create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }
        create_info.preTransform = capabilities.currentTransform;
        create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        create_info.presentMode = present_mode;
        create_info.clipped = VK_TRUE;
        const auto result = vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateSwapchainKHR", result);
            return false;
        }
        swapchain_format = format.format;
        swapchain_extent = extent;
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
        swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(device, swapchain, &image_count, swapchain_images.data());
        return true;
    }

    bool create_image_views() {
        image_views.resize(swapchain_images.size());
        for (std::size_t index = 0; index < swapchain_images.size(); ++index) {
            VkImageViewCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            create_info.image = swapchain_images[index];
            create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            create_info.format = swapchain_format;
            create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            create_info.subresourceRange.levelCount = 1;
            create_info.subresourceRange.layerCount = 1;
            const auto result = vkCreateImageView(device, &create_info, nullptr, &image_views[index]);
            if (result != VK_SUCCESS) {
                last_error = vk_error("vkCreateImageView", result);
                return false;
            }
        }
        return true;
    }

    // Picks the first depth format the device can use as an optimally tiled depth attachment.
    VkFormat select_depth_format() const {
        static constexpr std::array candidates{VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
        for (const auto candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, candidate, &properties);
            if ((properties.optimalTilingFeatures &
                 VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0U) {
                return candidate;
            }
        }
        return VK_FORMAT_UNDEFINED;
    }

    bool create_depth_resources() {
        depth_format = select_depth_format();
        if (depth_format == VK_FORMAT_UNDEFINED) {
            last_error = "no supported depth attachment format is available";
            return false;
        }
        depth_attachments.resize(swapchain_images.size());
        for (auto& depth : depth_attachments) {
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = depth_format;
            image_info.extent = {swapchain_extent.width, swapchain_extent.height, 1U};
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            auto result = vkCreateImage(device, &image_info, nullptr, &depth.image);
            if (result != VK_SUCCESS) {
                last_error = vk_error("depth image creation", result);
                return false;
            }
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, depth.image, &requirements);
            const auto memory_type = find_memory_type(requirements.memoryTypeBits,
                                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (!memory_type.has_value()) {
                last_error = "no device-local memory type is available for the depth attachment";
                return false;
            }
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = *memory_type;
            result = vkAllocateMemory(device, &allocation, nullptr, &depth.memory);
            if (result == VK_SUCCESS) {
                result = vkBindImageMemory(device, depth.image, depth.memory, 0U);
            }
            if (result != VK_SUCCESS) {
                last_error = vk_error("depth image allocation", result);
                return false;
            }
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = depth.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = depth_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            result = vkCreateImageView(device, &view_info, nullptr, &depth.view);
            if (result != VK_SUCCESS) {
                last_error = vk_error("depth image view creation", result);
                return false;
            }
        }
        return true;
    }

    bool create_directional_shadow_resources() {
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                                   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        constexpr std::array candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM,
                                        VK_FORMAT_D24_UNORM_S8_UINT};
        shadow_format = VK_FORMAT_UNDEFINED;
        for (const auto candidate : candidates) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, candidate, &properties);
            if ((properties.optimalTilingFeatures & required) == required) {
                shadow_format = candidate;
                break;
            }
        }
        if (shadow_format == VK_FORMAT_UNDEFINED) {
            last_error = "directional shadows require a linearly filterable sampled depth format";
            return false;
        }
        VkAttachmentDescription attachment{};
        attachment.format = shadow_format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference reference{0U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.pDepthStencilAttachment = &reference;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0U;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0U;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo pass_info{};
        pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        pass_info.attachmentCount = 1U;
        pass_info.pAttachments = &attachment;
        pass_info.subpassCount = 1U;
        pass_info.pSubpasses = &subpass;
        pass_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        pass_info.pDependencies = dependencies.data();
        auto result = vkCreateRenderPass(device, &pass_info, nullptr, &shadow_render_pass);
        for (std::size_t attachment_index = 0; attachment_index < shadow_attachments.size();
             ++attachment_index) {
            auto& shadow = shadow_attachments[attachment_index];
            const auto map = attachment_index % shadow_map_count;
            shadow.extent = map < directional_shadow_cascade_count
                                ? directional_shadow_resolutions[map]
                                : spot_shadow_resolution;
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = shadow_format;
            image_info.extent = {shadow.extent, shadow.extent, 1U};
            image_info.mipLevels = 1U;
            image_info.arrayLayers = 1U;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (result == VK_SUCCESS) result = vkCreateImage(device, &image_info, nullptr, &shadow.image);
            VkMemoryRequirements requirements{};
            if (result == VK_SUCCESS) vkGetImageMemoryRequirements(device, shadow.image, &requirements);
            const auto memory_type = result == VK_SUCCESS
                                         ? find_memory_type(requirements.memoryTypeBits,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                         : std::optional<std::uint32_t>{};
            if (result == VK_SUCCESS && !memory_type) result = VK_ERROR_FEATURE_NOT_PRESENT;
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type.value_or(0U);
            if (result == VK_SUCCESS) result = vkAllocateMemory(device, &allocation, nullptr, &shadow.memory);
            if (result == VK_SUCCESS) result = vkBindImageMemory(device, shadow.image, shadow.memory, 0U);
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = shadow.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = shadow_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = 1U;
            if (result == VK_SUCCESS) result = vkCreateImageView(device, &view_info, nullptr, &shadow.view);
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = shadow_render_pass;
            framebuffer_info.attachmentCount = 1U;
            framebuffer_info.pAttachments = &shadow.view;
            framebuffer_info.width = shadow.extent;
            framebuffer_info.height = shadow.extent;
            framebuffer_info.layers = 1U;
            if (result == VK_SUCCESS)
                result = vkCreateFramebuffer(device, &framebuffer_info, nullptr, &shadow.framebuffer);
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sampler_info.compareEnable = VK_TRUE;
        sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (result == VK_SUCCESS) result = vkCreateSampler(device, &sampler_info, nullptr, &shadow_sampler);

        const auto vertex_code = read_shader(RELAY_SHADOW_VERTEX_PATH, last_error);
        const auto fragment_code = read_shader(RELAY_SHADOW_FRAGMENT_PATH, last_error);
        VkShaderModule vertex_module{}, fragment_module{};
        VkShaderModuleCreateInfo module_info{};
        module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        module_info.codeSize = vertex_code.size() * sizeof(std::uint32_t);
        module_info.pCode = vertex_code.data();
        if (result == VK_SUCCESS && !vertex_code.empty())
            result = vkCreateShaderModule(device, &module_info, nullptr, &vertex_module);
        module_info.codeSize = fragment_code.size() * sizeof(std::uint32_t);
        module_info.pCode = fragment_code.data();
        if (result == VK_SUCCESS && !fragment_code.empty())
            result = vkCreateShaderModule(device, &module_info, nullptr, &fragment_module);
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.size = sizeof(DrawPushConstants);
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pushConstantRangeCount = 1U;
        layout_info.pPushConstantRanges = &push;
        layout_info.setLayoutCount = 1U;
        layout_info.pSetLayouts = &texture_layout;
        if (result == VK_SUCCESS)
            result = vkCreatePipelineLayout(device, &layout_info, nullptr, &shadow_pipeline_layout);
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            nullptr, 0U, VK_SHADER_STAGE_VERTEX_BIT, vertex_module,
                                            "main", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            nullptr, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module,
                                            "main", nullptr}};
        VkVertexInputBindingDescription binding{0U, sizeof(MeshVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array attributes{
            VkVertexInputAttributeDescription{0U, 0U, VK_FORMAT_R32G32B32_SFLOAT, 0U},
            VkVertexInputAttributeDescription{1U, 0U, VK_FORMAT_R32G32_SFLOAT,
                                              static_cast<std::uint32_t>(offsetof(MeshVertex, u))}};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1U;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = viewport.scissorCount = 1U;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.25F;
        raster.depthBiasSlopeFactor = 1.75F;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;
        VkPipelineColorBlendStateCreateInfo color_blend{};
        color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth;
        pipeline_info.pColorBlendState = &color_blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = shadow_pipeline_layout;
        pipeline_info.renderPass = shadow_render_pass;
        if (result == VK_SUCCESS)
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr,
                                               &shadow_pipeline);
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error("directional shadow resource creation", result);
            return false;
        }
        // Asset refreshes repeat this same descriptor update from create_texture_resources().
        for (std::size_t f = 0; f < frames_in_flight; ++f) {
            std::array<VkDescriptorImageInfo, shadow_map_count> image_infos{};
            for (std::size_t map = 0; map < shadow_map_count; ++map) {
                image_infos[map] = {
                    shadow_sampler,
                    shadow_attachments[f * shadow_map_count + map].view,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            }
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = texture_sets[f];
            write.dstBinding = 3U;
            write.descriptorCount = shadow_map_count;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = image_infos.data();
            vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
        }
        return true;
    }

    bool create_point_shadow_resources() {
        auto result = VK_SUCCESS;
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            auto& shadow = point_shadow_attachments[frame];
            VkImageCreateInfo image_info{};
            image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.format = shadow_format;
            image_info.extent = {point_shadow_resolution, point_shadow_resolution, 1U};
            image_info.mipLevels = 1U;
            image_info.arrayLayers = point_shadow_face_count;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT;
            image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (result == VK_SUCCESS)
                result = vkCreateImage(device, &image_info, nullptr, &shadow.image);
            VkMemoryRequirements requirements{};
            if (result == VK_SUCCESS)
                vkGetImageMemoryRequirements(device, shadow.image, &requirements);
            const auto memory_type = result == VK_SUCCESS
                                         ? find_memory_type(requirements.memoryTypeBits,
                                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                                         : std::optional<std::uint32_t>{};
            if (result == VK_SUCCESS && !memory_type) result = VK_ERROR_FEATURE_NOT_PRESENT;
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = memory_type.value_or(0U);
            if (result == VK_SUCCESS)
                result = vkAllocateMemory(device, &allocation, nullptr, &shadow.memory);
            if (result == VK_SUCCESS)
                result = vkBindImageMemory(device, shadow.image, shadow.memory, 0U);
            VkImageViewCreateInfo view_info{};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = shadow.image;
            view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
            view_info.format = shadow_format;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            view_info.subresourceRange.levelCount = 1U;
            view_info.subresourceRange.layerCount = point_shadow_face_count;
            if (result == VK_SUCCESS)
                result = vkCreateImageView(device, &view_info, nullptr, &shadow.cube_view);
            for (std::size_t face = 0; face < point_shadow_face_count; ++face) {
                view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_info.subresourceRange.baseArrayLayer = static_cast<std::uint32_t>(face);
                view_info.subresourceRange.layerCount = 1U;
                if (result == VK_SUCCESS)
                    result = vkCreateImageView(device, &view_info, nullptr,
                                               &shadow.face_views[face]);
                VkFramebufferCreateInfo framebuffer_info{};
                framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebuffer_info.renderPass = shadow_render_pass;
                framebuffer_info.attachmentCount = 1U;
                framebuffer_info.pAttachments = &shadow.face_views[face];
                framebuffer_info.width = point_shadow_resolution;
                framebuffer_info.height = point_shadow_resolution;
                framebuffer_info.layers = 1U;
                if (result == VK_SUCCESS)
                    result = vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                                 &shadow.framebuffers[face]);
            }
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.compareEnable = VK_TRUE;
        sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        if (result == VK_SUCCESS)
            result = vkCreateSampler(device, &sampler_info, nullptr, &point_shadow_sampler);
        if (result != VK_SUCCESS) {
            last_error = vk_error("point shadow resource creation", result);
            return false;
        }
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            VkDescriptorImageInfo image_info{point_shadow_sampler,
                                             point_shadow_attachments[frame].cube_view,
                                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = texture_sets[frame];
            write.dstBinding = 4U;
            write.descriptorCount = 1U;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_info;
            vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
        }
        return true;
    }

    bool create_hdr_resources() {
        const auto supports = [&](const VkFormat format, const VkFormatFeatureFlags required) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical_device, format, &properties);
            return (properties.optimalTilingFeatures & required) == required;
        };
        constexpr VkFormatFeatureFlags target = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (!supports(hdr_format, target | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
            last_error = "device lacks a blendable, sampled RGBA16F color format";
            return false;
        }
        if (!supports(albedo_metallic_format, target) ||
            !supports(scene_depth_value_format, target)) {
            last_error = "device lacks the sampled RGBA8 sRGB or R32F scene target formats";
            return false;
        }
        return true;
    }

    bool create_scene_image(const VkFormat format, const VkImageUsageFlags usage,
                            const VkImageAspectFlags aspect, SceneImage& target) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {scene_extent.width, scene_extent.height, 1U};
        image_info.mipLevels = 1U;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = usage;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        target.info = image_info;
        auto result = vkCreateImage(device, &image_info, nullptr, &target.image);
        if (result != VK_SUCCESS) { last_error = vk_error("scene target creation", result); return false; }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, target.image, &requirements);
        const auto memory_type = find_memory_type(requirements.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!memory_type) { last_error = "no device-local memory for a scene target"; return false; }
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = *memory_type;
        result = vkAllocateMemory(device, &allocation, nullptr, &target.memory);
        if (result == VK_SUCCESS) result = vkBindImageMemory(device, target.image, target.memory, 0U);
        if (result != VK_SUCCESS) { last_error = vk_error("scene target allocation", result); return false; }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = target.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = aspect;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &target.view);
        if (result != VK_SUCCESS) { last_error = vk_error("scene target view creation", result); return false; }
        return true;
    }

    void destroy_scene_targets() {
        for (auto& targets : scene_targets) {
            vkDestroyFramebuffer(device, targets.geometry_framebuffer, nullptr);
            vkDestroyFramebuffer(device, targets.forward_framebuffer, nullptr);
            for (auto* image : {&targets.hdr, &targets.normal_roughness, &targets.albedo_metallic,
                                &targets.motion_occlusion, &targets.depth_value,
                                &targets.diffuse_light, &targets.depth_stencil}) {
                vkDestroyImageView(device, image->view, nullptr);
                vkDestroyImage(device, image->image, nullptr);
                vkFreeMemory(device, image->memory, nullptr);
            }
            targets = {};
        }
        scene_extent = {};
        scene_targets_rendered.fill(false);
        previous_frame_valid = false;
    }

    // Matches the scene targets to the viewport. Resizing waits for the GPU, as swapchain
    // recreation does, because both frame sets are replaced together.
    bool ensure_scene_targets(const VkExtent2D extent) {
        if (extent.width == scene_extent.width && extent.height == scene_extent.height &&
            scene_targets[0].geometry_framebuffer != VK_NULL_HANDLE)
            return true;
        vkDeviceWaitIdle(device);
        destroy_scene_targets();
        scene_extent = extent;
        // Transfer source: the reflection denoiser copies depth, normals and roughness into its
        // own history.
        constexpr VkImageUsageFlags color_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            auto& targets = scene_targets[frame];
            if (!create_scene_image(hdr_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT, targets.hdr) ||
                !create_scene_image(normal_roughness_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    targets.normal_roughness) ||
                !create_scene_image(albedo_metallic_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    targets.albedo_metallic) ||
                !create_scene_image(motion_occlusion_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    targets.motion_occlusion) ||
                !create_scene_image(scene_depth_value_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    targets.depth_value) ||
                !create_scene_image(diffuse_light_format, color_usage, VK_IMAGE_ASPECT_COLOR_BIT,
                                    targets.diffuse_light) ||
                !create_scene_image(depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                                    targets.depth_stencil))
                return false;
            const std::array geometry_views{targets.hdr.view, targets.normal_roughness.view,
                                            targets.albedo_metallic.view,
                                            targets.motion_occlusion.view, targets.depth_value.view,
                                            targets.diffuse_light.view, targets.depth_stencil.view};
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = geometry_render_pass;
            framebuffer_info.attachmentCount = static_cast<std::uint32_t>(geometry_views.size());
            framebuffer_info.pAttachments = geometry_views.data();
            framebuffer_info.width = scene_extent.width;
            framebuffer_info.height = scene_extent.height;
            framebuffer_info.layers = 1U;
            auto result = vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                              &targets.geometry_framebuffer);
            const std::array forward_views{targets.hdr.view, targets.depth_stencil.view};
            framebuffer_info.renderPass = scene_render_pass;
            framebuffer_info.attachmentCount = static_cast<std::uint32_t>(forward_views.size());
            framebuffer_info.pAttachments = forward_views.data();
            if (result == VK_SUCCESS)
                result = vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                             &targets.forward_framebuffer);
            if (result != VK_SUCCESS) { last_error = vk_error("scene framebuffer creation", result); return false; }
            VkDescriptorImageInfo image_info{tone_sampler, targets.hdr.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = tone_sets[frame];
            write.dstBinding = 0U;
            write.descriptorCount = 1U;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &image_info;
            vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
        }
        return true;
    }

    bool create_render_pass() {
        // Geometry pass: HDR color plus the G-buffer, all cleared. Every G-buffer target ends
        // ready for sampling; the HDR image and depth continue into the forward pass.
        std::array<VkAttachmentDescription, geometry_color_attachments + 1U> geometry_attachments{};
        const std::array<VkFormat, geometry_color_attachments> color_formats{
            hdr_format, normal_roughness_format, albedo_metallic_format, motion_occlusion_format,
            scene_depth_value_format, diffuse_light_format};
        std::array<VkAttachmentReference, geometry_color_attachments> color_references{};
        for (std::uint32_t index = 0; index < geometry_color_attachments; ++index) {
            auto& attachment = geometry_attachments[index];
            attachment.format = color_formats[index];
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = index == 0U ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            color_references[index] = {index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        }
        auto& geometry_depth = geometry_attachments[geometry_color_attachments];
        geometry_depth.format = depth_format;
        geometry_depth.samples = VK_SAMPLE_COUNT_1_BIT;
        geometry_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        geometry_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        geometry_depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        geometry_depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        geometry_depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        geometry_depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference geometry_depth_reference{
            geometry_color_attachments, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription geometry_subpass{};
        geometry_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        geometry_subpass.colorAttachmentCount = geometry_color_attachments;
        geometry_subpass.pColorAttachments = color_references.data();
        geometry_subpass.pDepthStencilAttachment = &geometry_depth_reference;
        std::array<VkSubpassDependency, 2> geometry_dependencies{};
        // The previous use of these images, in either frame set, may have been a shader read.
        geometry_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        geometry_dependencies[0].dstSubpass = 0U;
        geometry_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        geometry_dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        geometry_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        geometry_dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        geometry_dependencies[1].srcSubpass = 0U;
        geometry_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        geometry_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        geometry_dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        geometry_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        geometry_dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                                 VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo geometry_info{};
        geometry_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        geometry_info.attachmentCount = static_cast<std::uint32_t>(geometry_attachments.size());
        geometry_info.pAttachments = geometry_attachments.data();
        geometry_info.subpassCount = 1U;
        geometry_info.pSubpasses = &geometry_subpass;
        geometry_info.dependencyCount = static_cast<std::uint32_t>(geometry_dependencies.size());
        geometry_info.pDependencies = geometry_dependencies.data();
        auto result = vkCreateRenderPass(device, &geometry_info, nullptr, &geometry_render_pass);
        if (result != VK_SUCCESS) { last_error = vk_error("geometry render pass creation", result); return false; }

        // Forward pass: keeps the geometry pass's color, depth and stencil.
        VkAttachmentDescription hdr_attachment = geometry_attachments[0];
        hdr_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        hdr_attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        hdr_attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentDescription scene_depth = geometry_depth;
        scene_depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        scene_depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        scene_depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        scene_depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        scene_depth.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference hdr_reference{0U, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference scene_depth_reference{1U, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription scene_subpass{};
        scene_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        scene_subpass.colorAttachmentCount = 1U;
        scene_subpass.pColorAttachments = &hdr_reference;
        scene_subpass.pDepthStencilAttachment = &scene_depth_reference;
        std::array<VkSubpassDependency, 2> scene_dependencies{};
        scene_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        scene_dependencies[0].dstSubpass = 0U;
        scene_dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                             VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        scene_dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_SHADER_WRITE_BIT;
        scene_dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        scene_dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                              VK_ACCESS_SHADER_READ_BIT;
        scene_dependencies[1].srcSubpass = 0U;
        scene_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        scene_dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        scene_dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        scene_dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        scene_dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        const std::array scene_attachments{hdr_attachment, scene_depth};
        VkRenderPassCreateInfo scene_info{};
        scene_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        scene_info.attachmentCount = static_cast<std::uint32_t>(scene_attachments.size());
        scene_info.pAttachments = scene_attachments.data();
        scene_info.subpassCount = 1U;
        scene_info.pSubpasses = &scene_subpass;
        scene_info.dependencyCount = static_cast<std::uint32_t>(scene_dependencies.size());
        scene_info.pDependencies = scene_dependencies.data();
        result = vkCreateRenderPass(device, &scene_info, nullptr, &scene_render_pass);
        if (result != VK_SUCCESS) { last_error = vk_error("HDR render pass creation", result); return false; }

        VkAttachmentDescription color_attachment{};
        color_attachment.format = swapchain_format;
        color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color_attachment.finalLayout = transfer_source_supported
                                           ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                           : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentDescription depth_attachment{};
        depth_attachment.format = depth_format;
        depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Depth is consumed entirely within the pass; nothing reads it afterwards.
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color_reference{};
        color_reference.attachment = 0;
        color_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference depth_reference{};
        depth_reference.attachment = 1;
        depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_reference;
        subpass.pDepthStencilAttachment = &depth_reference;
        std::array<VkSubpassDependency, 2> dependencies{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        // The depth image is shared by every frame: the previous frame's depth store (a write,
        // even as DONT_CARE) must finish before this pass clears it.
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = transfer_source_supported ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                                                  : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = transfer_source_supported ? static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_READ_BIT) : 0U;
        const std::array attachments{color_attachment, depth_attachment};
        VkRenderPassCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        create_info.pAttachments = attachments.data();
        create_info.subpassCount = 1;
        create_info.pSubpasses = &subpass;
        create_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        create_info.pDependencies = dependencies.data();
        result = vkCreateRenderPass(device, &create_info, nullptr, &render_pass);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateRenderPass", result);
            return false;
        }
        return true;
    }

    bool create_pipeline() {
        auto vertex_code = read_shader(RELAY_VERTEX_SHADER_PATH, last_error);
        if (vertex_code.empty()) return false;
        auto fragment_code = read_shader(RELAY_FRAGMENT_SHADER_PATH, last_error);
        if (fragment_code.empty()) return false;
        vertex_interface = reflect_spirv(vertex_code);
        fragment_interface = reflect_spirv(fragment_code);
        if (!vertex_interface.valid || !fragment_interface.valid) {
            last_error = "shader reflection failed: " +
                         (!vertex_interface.valid ? vertex_interface.error : fragment_interface.error);
            return false;
        }
        if (vertex_interface.push_constant_bytes != sizeof(DrawPushConstants) ||
            fragment_interface.push_constant_bytes != sizeof(DrawPushConstants)) {
            last_error = "shader push-constant layout does not match the native draw layout";
            return false;
        }
        VkShaderModuleCreateInfo vertex_info{};
        vertex_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vertex_info.codeSize = vertex_code.size() * sizeof(std::uint32_t);
        vertex_info.pCode = vertex_code.data();
        VkShaderModuleCreateInfo fragment_info = vertex_info;
        fragment_info.codeSize = fragment_code.size() * sizeof(std::uint32_t);
        fragment_info.pCode = fragment_code.data();
        VkShaderModule vertex_module{};
        VkShaderModule fragment_module{};
        auto result = vkCreateShaderModule(device, &vertex_info, nullptr, &vertex_module);
        if (result == VK_SUCCESS) {
            result = vkCreateShaderModule(device, &fragment_info, nullptr, &fragment_module);
        }
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device, vertex_module, nullptr);
            last_error = vk_error("vkCreateShaderModule", result);
            return false;
        }

        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_module,
                                            "main", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                            nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module,
                                            "main", nullptr},
        };
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkVertexInputBindingDescription vertex_binding{};
        vertex_binding.binding = 0U;
        vertex_binding.stride = sizeof(MeshVertex);
        vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 4> attributes{};
        attributes[0].location = 0U;
        attributes[0].binding = 0U;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = 0U;
        attributes[1].location = 1U;
        attributes[1].binding = 0U;
        attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[1].offset = static_cast<std::uint32_t>(offsetof(MeshVertex, u));
        attributes[2].location = 2U;
        attributes[2].binding = 0U;
        attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[2].offset = static_cast<std::uint32_t>(offsetof(MeshVertex, nx));
        attributes[3].location = 3U;
        attributes[3].binding = 0U;
        attributes[3].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[3].offset = static_cast<std::uint32_t>(offsetof(MeshVertex, tx));
        vertex_input.vertexBindingDescriptionCount = 1U;
        vertex_input.pVertexBindingDescriptions = &vertex_binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo input_assembly{};
        input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport_state{};
        viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport_state.viewportCount = 1;
        viewport_state.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkPushConstantRange push_constant{};
        push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push_constant.size = sizeof(DrawPushConstants);
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push_constant;
        layout_info.setLayoutCount = 1U;
        layout_info.pSetLayouts = &texture_layout;
        result = vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout);

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &input_assembly;
        pipeline_info.pViewportState = &viewport_state;
        pipeline_info.pRasterizationState = &rasterization;
        VkPipelineDepthStencilStateCreateInfo depth_stencil{};
        depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_stencil.depthTestEnable = VK_TRUE;
        depth_stencil.depthWriteEnable = VK_TRUE;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depth_stencil.depthBoundsTestEnable = VK_FALSE;
        depth_stencil.stencilTestEnable = VK_FALSE;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth_stencil;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = pipeline_layout;
        // Opaque geometry writes the HDR color and the whole G-buffer in the geometry pass.
        const std::array<VkPipelineColorBlendAttachmentState, geometry_color_attachments>
            geometry_blend{blend_attachment, blend_attachment, blend_attachment, blend_attachment,
                           blend_attachment, blend_attachment};
        blend.attachmentCount = geometry_color_attachments;
        blend.pAttachments = geometry_blend.data();
        const VkBool32 geometry_pass = VK_TRUE;
        const VkSpecializationMapEntry geometry_entry{0U, 0U, sizeof(VkBool32)};
        VkSpecializationInfo geometry_specialization{1U, &geometry_entry, sizeof(geometry_pass),
                                                     &geometry_pass};
        auto geometry_stages = stages;
        geometry_stages[1].pSpecializationInfo = &geometry_specialization;
        pipeline_info.pStages = geometry_stages.data();
        pipeline_info.renderPass = geometry_render_pass;
        if (result == VK_SUCCESS) {
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        }
        blend.attachmentCount = 1U;
        blend.pAttachments = &blend_attachment;
        pipeline_info.pStages = stages.data();
        pipeline_info.renderPass = scene_render_pass;
        if (result == VK_SUCCESS) {
            // Transparent geometry keeps depth testing but cannot write depth, and uses straight
            // alpha source-over compositing. RenderScene orders these draws back to front.
            depth_stencil.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                               &transparent_pipeline);
            blend_attachment.blendEnable = VK_FALSE;
            depth_stencil.depthWriteEnable = VK_TRUE;
        }
        if (result == VK_SUCCESS) {
            const auto selection_vertex = read_shader(RELAY_SELECTION_VERTEX_PATH, last_error);
            const auto selection_fragment = read_shader(RELAY_SELECTION_FRAGMENT_PATH, last_error);
            const auto vertex_layout = reflect_spirv(selection_vertex);
            const auto fragment_layout = reflect_spirv(selection_fragment);
            if (!vertex_layout.valid || !fragment_layout.valid ||
                vertex_layout.push_constant_bytes != sizeof(DrawPushConstants) ||
                fragment_layout.push_constant_bytes != sizeof(DrawPushConstants)) {
                last_error = "selection shader layout does not match native push constants";
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
            VkShaderModule selection_vertex_module{}, selection_fragment_module{};
            vertex_info.codeSize = selection_vertex.size() * sizeof(std::uint32_t);
            vertex_info.pCode = selection_vertex.data();
            fragment_info.codeSize = selection_fragment.size() * sizeof(std::uint32_t);
            fragment_info.pCode = selection_fragment.data();
            if (result == VK_SUCCESS)
                result = vkCreateShaderModule(device, &vertex_info, nullptr, &selection_vertex_module);
            if (result == VK_SUCCESS)
                result = vkCreateShaderModule(device, &fragment_info, nullptr, &selection_fragment_module);
            auto selection_stages = stages;
            selection_stages[0].module = selection_vertex_module;
            selection_stages[1].module = selection_fragment_module;
            pipeline_info.pStages = selection_stages.data();
            vertex_input.vertexAttributeDescriptionCount = 2;
            depth_stencil.depthWriteEnable = VK_FALSE;
            depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            depth_stencil.stencilTestEnable = VK_TRUE;
            depth_stencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                                   VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 1};
            depth_stencil.back = depth_stencil.front;
            const auto color_mask = blend_attachment.colorWriteMask;
            blend_attachment.colorWriteMask = 0;
            if (result == VK_SUCCESS)
                result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                   nullptr, &selection_mask_pipeline);
            blend_attachment.colorWriteMask = color_mask;
            depth_stencil.front.passOp = VK_STENCIL_OP_KEEP;
            depth_stencil.front.compareOp = VK_COMPARE_OP_NOT_EQUAL;
            depth_stencil.front.writeMask = 0;
            depth_stencil.back = depth_stencil.front;
            if (result == VK_SUCCESS)
                result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                   nullptr, &selection_outline_pipeline);
            vkDestroyShaderModule(device, selection_fragment_module, nullptr);
            vkDestroyShaderModule(device, selection_vertex_module, nullptr);
            depth_stencil.stencilTestEnable = VK_FALSE;
            depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
        }
        if (result == VK_SUCCESS) {
            const auto grid_vertex = read_shader(RELAY_GRID_VERTEX_PATH, last_error);
            const auto grid_fragment = read_shader(RELAY_GRID_FRAGMENT_PATH, last_error);
            const auto grid_vertex_interface = reflect_spirv(grid_vertex);
            const auto grid_fragment_interface = reflect_spirv(grid_fragment);
            if (!grid_vertex_interface.valid || !grid_fragment_interface.valid ||
                grid_vertex_interface.push_constant_bytes != sizeof(DrawPushConstants) ||
                grid_fragment_interface.push_constant_bytes != sizeof(DrawPushConstants)) {
                last_error = "editor grid shader layout does not match native push constants";
                result = VK_ERROR_INITIALIZATION_FAILED;
            }
            VkShaderModule grid_vertex_module{}, grid_fragment_module{};
            if (grid_vertex.empty() || grid_fragment.empty())
                result = VK_ERROR_INITIALIZATION_FAILED;
            if (result == VK_SUCCESS) {
                vertex_info.codeSize = grid_vertex.size() * sizeof(std::uint32_t);
                vertex_info.pCode = grid_vertex.data();
                fragment_info.codeSize = grid_fragment.size() * sizeof(std::uint32_t);
                fragment_info.pCode = grid_fragment.data();
                result = vkCreateShaderModule(device, &vertex_info, nullptr, &grid_vertex_module);
                if (result == VK_SUCCESS)
                    result = vkCreateShaderModule(device, &fragment_info, nullptr,
                                                  &grid_fragment_module);
            }
            auto grid_stages = stages;
            grid_stages[0].module = grid_vertex_module;
            grid_stages[1].module = grid_fragment_module;
            pipeline_info.pStages = grid_stages.data();
            vertex_input.vertexBindingDescriptionCount = 0;
            vertex_input.vertexAttributeDescriptionCount = 0;
            depth_stencil.depthWriteEnable = VK_FALSE;
            blend_attachment.blendEnable = VK_TRUE;
            blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
            blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            if (result == VK_SUCCESS)
                result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                                   nullptr, &grid_pipeline);
            vkDestroyShaderModule(device, grid_fragment_module, nullptr);
            vkDestroyShaderModule(device, grid_vertex_module, nullptr);
        }
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error("graphics pipeline creation", result);
            return false;
        }
        return true;
    }

    bool create_tone_resources() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0U;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1U;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1U;
        layout_info.pBindings = &binding;
        auto result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &tone_layout);
        if (result != VK_SUCCESS) { last_error = vk_error("tone descriptor layout", result); return false; }
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       static_cast<std::uint32_t>(frames_in_flight)};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = static_cast<std::uint32_t>(frames_in_flight);
        pool_info.poolSizeCount = 1U;
        pool_info.pPoolSizes = &pool_size;
        result = vkCreateDescriptorPool(device, &pool_info, nullptr, &tone_pool);
        if (result != VK_SUCCESS) { last_error = vk_error("tone descriptor pool", result); return false; }
        std::array<VkDescriptorSetLayout, frames_in_flight> layouts{};
        layouts.fill(tone_layout);
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = tone_pool;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocation.pSetLayouts = layouts.data();
        result = vkAllocateDescriptorSets(device, &allocation, tone_sets.data());
        if (result != VK_SUCCESS) { last_error = vk_error("tone descriptor allocation", result); return false; }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        result = vkCreateSampler(device, &sampler_info, nullptr, &tone_sampler);
        if (result != VK_SUCCESS) { last_error = vk_error("tone sampler", result); return false; }
        // ensure_scene_targets points each set at its frame's HDR image.
        const auto vertex_code = read_shader(RELAY_TONE_VERTEX_PATH, last_error);
        const auto fragment_code = read_shader(RELAY_TONE_FRAGMENT_PATH, last_error);
        if (vertex_code.empty() || fragment_code.empty()) return false;
        const auto tone_vertex_interface = reflect_spirv(vertex_code);
        const auto tone_fragment_interface = reflect_spirv(fragment_code);
        if (!tone_vertex_interface.valid || !tone_fragment_interface.valid ||
            tone_fragment_interface.push_constant_bytes != sizeof(ToneSettings)) {
            last_error = "tone shader interface is incompatible with display settings";
            return false;
        }
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = vertex_code.size() * sizeof(std::uint32_t);
        shader_info.pCode = vertex_code.data();
        VkShaderModule vertex_module{}, fragment_module{};
        result = vkCreateShaderModule(device, &shader_info, nullptr, &vertex_module);
        shader_info.codeSize = fragment_code.size() * sizeof(std::uint32_t);
        shader_info.pCode = fragment_code.data();
        if (result == VK_SUCCESS)
            result = vkCreateShaderModule(device, &shader_info, nullptr, &fragment_module);
        if (result != VK_SUCCESS) {
            vkDestroyShaderModule(device, vertex_module, nullptr);
            last_error = vk_error("tone shader module", result);
            return false;
        }
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size = sizeof(ToneSettings);
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &tone_layout;
        pipeline_layout_info.pushConstantRangeCount = 1U;
        pipeline_layout_info.pPushConstantRanges = &push;
        result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                        &tone_pipeline_layout);
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0U, VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module, "main", nullptr}};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1U;
        viewport.scissorCount = 1U;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color_blend{};
        color_blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1U;
        blend.pAttachments = &color_blend;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = tone_pipeline_layout;
        pipeline_info.renderPass = render_pass;
        if (result == VK_SUCCESS)
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &pipeline_info,
                                               nullptr, &tone_pipeline);
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) { last_error = vk_error("tone pipeline", result); return false; }
        return true;
    }

    // The global illumination composite: a fullscreen pass in the forward render pass that adds
    // indirect light, reading the G-buffer and the GI outputs.
    bool create_composite_resources() {
        std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
        for (std::uint32_t index = 0; index < bindings.size(); ++index) {
            bindings[index].binding = index;
            bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[index].descriptorCount = 1U;
            bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
        auto result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &composite_layout);
        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                             static_cast<std::uint32_t>(bindings.size() *
                                                                        frames_in_flight)};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = frames_in_flight;
        pool_info.poolSizeCount = 1U;
        pool_info.pPoolSizes = &pool_size;
        if (result == VK_SUCCESS) result = vkCreateDescriptorPool(device, &pool_info, nullptr, &composite_pool);
        std::array<VkDescriptorSetLayout, frames_in_flight> layouts{};
        layouts.fill(composite_layout);
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = composite_pool;
        allocation.descriptorSetCount = frames_in_flight;
        allocation.pSetLayouts = layouts.data();
        if (result == VK_SUCCESS) result = vkAllocateDescriptorSets(device, &allocation, composite_sets.data());
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size = sizeof(CompositeConstants);
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &composite_layout;
        pipeline_layout_info.pushConstantRangeCount = 1U;
        pipeline_layout_info.pPushConstantRanges = &push;
        if (result == VK_SUCCESS)
            result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                            &composite_pipeline_layout);
        if (result != VK_SUCCESS) { last_error = vk_error("GI composite layout", result); return false; }
        const auto vertex_code = read_shader(RELAY_TONE_VERTEX_PATH, last_error);
        const auto fragment_code = read_shader(RELAY_GI_COMPOSITE_FRAGMENT_PATH, last_error);
        if (vertex_code.empty() || fragment_code.empty()) return false;
        const auto fragment_interface_check = reflect_spirv(fragment_code);
        if (!fragment_interface_check.valid ||
            fragment_interface_check.push_constant_bytes != sizeof(CompositeConstants)) {
            last_error = "GI composite shader interface does not match its push constants";
            return false;
        }
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = vertex_code.size() * sizeof(std::uint32_t);
        shader_info.pCode = vertex_code.data();
        VkShaderModule vertex_module{}, fragment_module{};
        result = vkCreateShaderModule(device, &shader_info, nullptr, &vertex_module);
        shader_info.codeSize = fragment_code.size() * sizeof(std::uint32_t);
        shader_info.pCode = fragment_code.data();
        if (result == VK_SUCCESS)
            result = vkCreateShaderModule(device, &shader_info, nullptr, &fragment_module);
        const std::array stages{
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0U, VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main", nullptr},
            VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                nullptr, 0U, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module, "main", nullptr}};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1U;
        viewport.scissorCount = 1U;
        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState additive{};
        additive.blendEnable = VK_TRUE;
        additive.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additive.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additive.colorBlendOp = VK_BLEND_OP_ADD;
        additive.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        additive.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        additive.alphaBlendOp = VK_BLEND_OP_ADD;
        additive.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1U;
        blend.pAttachments = &additive;
        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = static_cast<std::uint32_t>(stages.size());
        pipeline_info.pStages = stages.data();
        pipeline_info.pVertexInputState = &vertex_input;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = composite_pipeline_layout;
        pipeline_info.renderPass = scene_render_pass;
        if (result == VK_SUCCESS)
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr,
                                               &composite_pipeline);
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) { last_error = vk_error("GI composite pipeline", result); return false; }
        return true;
    }

    bool create_framebuffers() {
        framebuffers.resize(image_views.size());
        for (std::size_t index = 0; index < image_views.size(); ++index) {
            const std::array attachments{image_views[index], depth_attachments[index].view};
            VkFramebufferCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            create_info.renderPass = render_pass;
            create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            create_info.pAttachments = attachments.data();
            create_info.width = swapchain_extent.width;
            create_info.height = swapchain_extent.height;
            create_info.layers = 1;
            const auto result =
                vkCreateFramebuffer(device, &create_info, nullptr, &framebuffers[index]);
            if (result != VK_SUCCESS) {
                last_error = vk_error("vkCreateFramebuffer", result);
                return false;
            }
        }
        return true;
    }

    bool create_present_semaphores() {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        render_finished.resize(swapchain_images.size());
        for (auto& semaphore : render_finished)
            if (vkCreateSemaphore(device, &semaphore_info, nullptr, &semaphore) != VK_SUCCESS) {
                last_error = "could not create Vulkan presentation semaphores";
                return false;
            }
        return true;
    }

    bool create_swapchain_resources() {
        return create_swapchain() && swapchain != VK_NULL_HANDLE && create_present_semaphores() &&
               create_image_views() &&
               create_depth_resources() && create_hdr_resources() &&
               create_directional_shadow_resources() &&
               create_point_shadow_resources() &&
               create_render_pass() && create_pipeline() && create_tone_resources() &&
               create_composite_resources() && create_framebuffers();
    }

    bool create_sync_objects() {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (std::size_t index = 0; index < frames_in_flight; ++index) {
            if (vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available[index]) != VK_SUCCESS ||
                vkCreateFence(device, &fence_info, nullptr, &frame_fences[index]) != VK_SUCCESS) {
                last_error = "could not create Vulkan frame synchronization objects";
                return false;
            }
        }
        return true;
    }

    void cleanup_swapchain() {
        if (device == VK_NULL_HANDLE) return;
        // The overlay builds pipelines against render_pass, which is about to be destroyed.
        if (overlay != nullptr && overlay_ready) {
            overlay->invalidate();
            overlay_ready = false;
        }
        for (const auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
        destroy_scene_targets();
        for (auto& shadow : shadow_attachments) {
            vkDestroyFramebuffer(device, shadow.framebuffer, nullptr);
            vkDestroyImageView(device, shadow.view, nullptr);
            vkDestroyImage(device, shadow.image, nullptr);
            vkFreeMemory(device, shadow.memory, nullptr);
            shadow = {};
        }
        for (auto& shadow : point_shadow_attachments) {
            for (const auto framebuffer : shadow.framebuffers)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            for (const auto view : shadow.face_views)
                vkDestroyImageView(device, view, nullptr);
            vkDestroyImageView(device, shadow.cube_view, nullptr);
            vkDestroyImage(device, shadow.image, nullptr);
            vkFreeMemory(device, shadow.memory, nullptr);
            shadow = {};
        }
        vkDestroyPipeline(device, shadow_pipeline, nullptr);
        shadow_pipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, shadow_pipeline_layout, nullptr);
        shadow_pipeline_layout = VK_NULL_HANDLE;
        vkDestroyRenderPass(device, shadow_render_pass, nullptr);
        shadow_render_pass = VK_NULL_HANDLE;
        vkDestroySampler(device, shadow_sampler, nullptr);
        shadow_sampler = VK_NULL_HANDLE;
        vkDestroySampler(device, point_shadow_sampler, nullptr);
        point_shadow_sampler = VK_NULL_HANDLE;
        vkDestroyPipeline(device, selection_mask_pipeline, nullptr);
        vkDestroyPipeline(device, selection_outline_pipeline, nullptr);
        selection_mask_pipeline = selection_outline_pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, grid_pipeline, nullptr);
        grid_pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, transparent_pipeline, nullptr);
        transparent_pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, tone_pipeline, nullptr);
        tone_pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, composite_pipeline, nullptr);
        composite_pipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, composite_pipeline_layout, nullptr);
        composite_pipeline_layout = VK_NULL_HANDLE;
        vkDestroyDescriptorPool(device, composite_pool, nullptr);
        composite_pool = VK_NULL_HANDLE;
        composite_sets.fill(VK_NULL_HANDLE);
        vkDestroyDescriptorSetLayout(device, composite_layout, nullptr);
        composite_layout = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, tone_pipeline_layout, nullptr);
        tone_pipeline_layout = VK_NULL_HANDLE;
        vkDestroyDescriptorPool(device, tone_pool, nullptr);
        tone_pool = VK_NULL_HANDLE;
        tone_sets.fill(VK_NULL_HANDLE);
        vkDestroyDescriptorSetLayout(device, tone_layout, nullptr);
        tone_layout = VK_NULL_HANDLE;
        vkDestroySampler(device, tone_sampler, nullptr);
        tone_sampler = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
        vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
        vkDestroyRenderPass(device, scene_render_pass, nullptr);
        scene_render_pass = VK_NULL_HANDLE;
        vkDestroyRenderPass(device, geometry_render_pass, nullptr);
        geometry_render_pass = VK_NULL_HANDLE;
        for (auto& depth : depth_attachments) {
            vkDestroyImageView(device, depth.view, nullptr);
            vkDestroyImage(device, depth.image, nullptr);
            vkFreeMemory(device, depth.memory, nullptr);
        }
        depth_attachments.clear();
        for (const auto view : image_views) vkDestroyImageView(device, view, nullptr);
        image_views.clear();
        for (const auto semaphore : render_finished) vkDestroySemaphore(device, semaphore, nullptr);
        render_finished.clear();
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
        swapchain_images.clear();
    }

    bool recreate_swapchain() {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        if (width == 0 || height == 0) return true;
        vkDeviceWaitIdle(device);
        cleanup_swapchain();
        resized = false;
        return create_swapchain_resources();
    }

    // Creates the global illumination effect the first time it is wanted. A failure is kept in
    // global_illumination_error and turns the effect off rather than failing the frame.
    bool prepare_global_illumination() {
#ifdef RELAY_HAS_FIDELITYFX
        if (!global_illumination_requested || global_illumination_failed) return false;
        if (!lighting_features) {
            global_illumination_failed = true;
            global_illumination_error =
                "this Vulkan device lacks the features global illumination needs";
            return false;
        }
        if (!global_illumination) {
            vkDeviceWaitIdle(device);
            global_illumination = std::make_unique<GlobalIllumination>();
            if (!global_illumination->initialize({physical_device, device, graphics_queue,
                                                  graphics_family},
                                                 global_illumination_error)) {
                global_illumination.reset();
                global_illumination_failed = true;
                return false;
            }
        }
        const auto extent = global_illumination->extent();
        if (extent.width != 0U &&
            (extent.width != scene_extent.width || extent.height != scene_extent.height))
            vkDeviceWaitIdle(device);
        return true;
#else
        if (global_illumination_requested && global_illumination_error.empty())
            global_illumination_error = "Relay was built without FidelityFX";
        return false;
#endif
    }

    void destroy_reflection_pipelines() {
        if (device == VK_NULL_HANDLE) return;
        for (auto* pipeline_handle : {&trace_pipeline, &arguments_pipeline})
            vkDestroyPipeline(device, *pipeline_handle, nullptr), *pipeline_handle = VK_NULL_HANDLE;
        for (auto* layout : {&trace_pipeline_layout, &arguments_pipeline_layout})
            vkDestroyPipelineLayout(device, *layout, nullptr), *layout = VK_NULL_HANDLE;
        vkDestroyDescriptorPool(device, trace_pool, nullptr);
        trace_pool = VK_NULL_HANDLE;
        for (auto* layout : {&trace_layout, &arguments_layout})
            vkDestroyDescriptorSetLayout(device, *layout, nullptr), *layout = VK_NULL_HANDLE;
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            vkDestroyBuffer(device, trace_instance_buffers[frame], nullptr);
            vkFreeMemory(device, trace_instance_memories[frame], nullptr);
            trace_instance_buffers[frame] = VK_NULL_HANDLE;
            trace_instance_memories[frame] = VK_NULL_HANDLE;
            trace_instance_capacities[frame] = 0U;
        }
    }

    bool create_compute_pipeline(const char* path, const VkPipelineLayout layout,
                                 VkPipeline& compute_pipeline) {
        const auto code = read_shader(path, last_error);
        if (code.empty()) return false;
        VkShaderModuleCreateInfo shader_info{};
        shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shader_info.codeSize = code.size() * sizeof(std::uint32_t);
        shader_info.pCode = code.data();
        VkShaderModule module{};
        auto result = vkCreateShaderModule(device, &shader_info, nullptr, &module);
        VkComputePipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_info.stage.module = module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = layout;
        if (result == VK_SUCCESS)
            result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1U, &pipeline_info, nullptr,
                                              &compute_pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error(std::string("compute pipeline ") + path, result);
            return false;
        }
        return true;
    }

    // The reflection trace pipeline reads the scene's set 0 (textures, materials, lights) and its
    // own set 1; the arguments pipeline only the ray counter and indirect arguments.
    bool create_reflection_pipelines() {
        const auto binding = [](const std::uint32_t index, const VkDescriptorType type) {
            VkDescriptorSetLayoutBinding entry{};
            entry.binding = index;
            entry.descriptorType = type;
            entry.descriptorCount = 1U;
            entry.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            return entry;
        };
        const std::array trace_bindings{
            binding(0U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
            binding(1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), binding(2U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            binding(3U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), binding(4U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(5U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            binding(6U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            binding(7U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            binding(8U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), binding(9U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            binding(10U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER), binding(11U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
        const std::array arguments_bindings{binding(0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
                                            binding(1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<std::uint32_t>(trace_bindings.size());
        layout_info.pBindings = trace_bindings.data();
        auto result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &trace_layout);
        layout_info.bindingCount = static_cast<std::uint32_t>(arguments_bindings.size());
        layout_info.pBindings = arguments_bindings.data();
        if (result == VK_SUCCESS)
            result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &arguments_layout);
        const std::array pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, frames_in_flight},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8U * frames_in_flight},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2U * frames_in_flight},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3U * frames_in_flight}};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 2U * frames_in_flight;
        pool_info.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        if (result == VK_SUCCESS) result = vkCreateDescriptorPool(device, &pool_info, nullptr, &trace_pool);
        std::array<VkDescriptorSetLayout, 2U * frames_in_flight> layouts{};
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            layouts[frame] = trace_layout;
            layouts[frames_in_flight + frame] = arguments_layout;
        }
        std::array<VkDescriptorSet, 2U * frames_in_flight> sets{};
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = trace_pool;
        allocation.descriptorSetCount = static_cast<std::uint32_t>(layouts.size());
        allocation.pSetLayouts = layouts.data();
        if (result == VK_SUCCESS) result = vkAllocateDescriptorSets(device, &allocation, sets.data());
        for (std::size_t frame = 0; frame < frames_in_flight; ++frame) {
            trace_sets[frame] = sets[frame];
            arguments_sets[frame] = sets[frames_in_flight + frame];
        }
        const std::array trace_set_layouts{texture_layout, trace_layout};
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size = sizeof(TraceConstants);
        VkPipelineLayoutCreateInfo pipeline_layout_info{};
        pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_layout_info.setLayoutCount = static_cast<std::uint32_t>(trace_set_layouts.size());
        pipeline_layout_info.pSetLayouts = trace_set_layouts.data();
        pipeline_layout_info.pushConstantRangeCount = 1U;
        pipeline_layout_info.pPushConstantRanges = &push;
        if (result == VK_SUCCESS)
            result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &trace_pipeline_layout);
        pipeline_layout_info.setLayoutCount = 1U;
        pipeline_layout_info.pSetLayouts = &arguments_layout;
        pipeline_layout_info.pushConstantRangeCount = 0U;
        if (result == VK_SUCCESS)
            result = vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr,
                                            &arguments_pipeline_layout);
        if (result != VK_SUCCESS) {
            last_error = vk_error("reflection pipeline layouts", result);
            return false;
        }
        return create_compute_pipeline(RELAY_REFLECTION_TRACE_PATH, trace_pipeline_layout,
                                       trace_pipeline) &&
               create_compute_pipeline(RELAY_REFLECTION_ARGUMENTS_PATH, arguments_pipeline_layout,
                                       arguments_pipeline);
    }

    // Creates the reflection effect the first time it is wanted; failures turn it off, as for GI.
    bool prepare_reflections() {
#ifdef RELAY_HAS_FIDELITYFX
        if (!reflections_requested || reflections_failed) return false;
        if (!ray_query_features) {
            reflections_failed = true;
            reflections_error = "this Vulkan device does not support ray queries";
            return false;
        }
        if (!reflections) {
            vkDeviceWaitIdle(device);
            reflections = std::make_unique<Reflections>();
            acceleration_structures = std::make_unique<SceneAccelerationStructures>();
            std::string error;
            if (!reflections->initialize({physical_device, device, graphics_queue, graphics_family},
                                         error) ||
                !acceleration_structures->initialize(physical_device, device, frames_in_flight,
                                                     error) ||
                !create_reflection_pipelines()) {
                reflections_error = error.empty() ? last_error : error;
                last_error.clear();
                reflections.reset();
                acceleration_structures.reset();
                destroy_reflection_pipelines();
                reflections_failed = true;
                return false;
            }
        }
        const auto extent = reflections->extent();
        if (extent.width != 0U &&
            (extent.width != scene_extent.width || extent.height != scene_extent.height))
            vkDeviceWaitIdle(device);
        return true;
#else
        if (reflections_requested && reflections_error.empty())
            reflections_error = "Relay was built without FidelityFX";
        return false;
#endif
    }

    static void compute_barrier(const VkCommandBuffer commands, const VkAccessFlags destination_access,
                                const VkPipelineStageFlags destination_stage) {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = destination_access;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, destination_stage, 0U, 1U,
                             &barrier, 0U, nullptr, 0U, nullptr);
    }

    // Builds the acceleration structures, classifies, traces and denoises this frame's
    // reflections between the geometry and forward passes.
    bool record_reflections(const VkCommandBuffer commands, const RenderScene& render_scene) {
#ifdef RELAY_HAS_FIDELITYFX
        std::vector<RayTracingInstance> ray_instances;
        std::vector<GpuTraceInstance> table;
        for (const auto& draw_instance : render_scene.instances) {
            if (draw_instance.alpha_blended) continue;
            const auto* mesh = assets->find_mesh(draw_instance.mesh);
            if (mesh == nullptr || mesh->index_count < 3U) continue;
            const bool deformed = draw_instance.deformed_vertex_offset >= 0;
            RayTracingInstance ray_instance;
            ray_instance.mesh = draw_instance.mesh;
            ray_instance.deformed = deformed;
            ray_instance.geometry.vertex_buffer = deformed ? deformed_buffers[current_frame] : mesh_vertex_buffer;
            ray_instance.geometry.vertex_stride = sizeof(MeshVertex);
            ray_instance.geometry.first_vertex = static_cast<std::uint32_t>(
                deformed ? draw_instance.deformed_vertex_offset : mesh->vertex_offset);
            ray_instance.geometry.vertex_count = mesh->vertex_count;
            ray_instance.geometry.index_buffer = mesh_index_buffer;
            ray_instance.geometry.first_index = mesh->first_index;
            ray_instance.geometry.index_count = mesh->index_count;
            ray_instance.model = draw_instance.model.values;
            ray_instance.custom_index = static_cast<std::uint32_t>(table.size());
            ray_instances.push_back(std::move(ray_instance));
            table.push_back({draw_instance.material_index, mesh->first_index,
                             deformed ? draw_instance.deformed_vertex_offset : mesh->vertex_offset,
                             deformed ? 1U : 0U});
        }
        if (table.empty()) table.emplace_back();
        std::string error;
        if (!acceleration_structures->record(commands, static_cast<std::uint32_t>(current_frame),
                                             ray_instances, error)) {
            reflections_error = error;
            return false;
        }
        const VkDeviceSize table_bytes = table.size() * sizeof(GpuTraceInstance);
        if (table_bytes > trace_instance_capacities[current_frame]) {
            vkDestroyBuffer(device, trace_instance_buffers[current_frame], nullptr);
            vkFreeMemory(device, trace_instance_memories[current_frame], nullptr);
            trace_instance_buffers[current_frame] = VK_NULL_HANDLE;
            trace_instance_memories[current_frame] = VK_NULL_HANDLE;
            trace_instance_capacities[current_frame] = 0U;
            const auto capacity = growing_capacity(table_bytes);
            if (!create_buffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               trace_instance_buffers[current_frame],
                               trace_instance_memories[current_frame])) {
                reflections_error = last_error;
                return false;
            }
            trace_instance_capacities[current_frame] = capacity;
        }
        void* mapped = nullptr;
        if (vkMapMemory(device, trace_instance_memories[current_frame], 0U, table_bytes, 0U, &mapped) !=
            VK_SUCCESS) {
            reflections_error = "could not map the reflection instance table";
            return false;
        }
        std::memcpy(mapped, table.data(), static_cast<std::size_t>(table_bytes));
        vkUnmapMemory(device, trace_instance_memories[current_frame]);

        const auto& current = scene_targets[current_frame];
        ReflectionFrame frame;
        frame.commands = commands;
        frame.frame_index = ++reflection_frame_index;
        frame.extent = scene_extent;
        frame.depth = {current.depth_value.image, current.depth_value.info};
        frame.normal_roughness = {current.normal_roughness.image, current.normal_roughness.info};
        frame.motion = {current.motion_occlusion.image, current.motion_occlusion.info};
        frame.view = render_scene.camera.view.values;
        frame.projection = render_scene.camera.projection.values;
        frame.previous_view = previous_view;
        frame.previous_projection = previous_projection;
        frame.sky_color = sky_radiance;
        frame.ground_color = ground_radiance;
        frame.reset = reflections->extent().width != scene_extent.width ||
                      reflections->extent().height != scene_extent.height;
        if (!reflections->record_classification(frame, reflections_error)) return false;
        compute_barrier(commands, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        const auto targets = reflections->targets(frame.frame_index);
        const VkDescriptorBufferInfo counter_info{targets.ray_counter, 0U, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo arguments_info{targets.indirect_arguments, 0U, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo rays_info{targets.ray_list, 0U, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo static_info{mesh_vertex_buffer, 0U, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo deformed_info{
            deformed_buffers[current_frame] ? deformed_buffers[current_frame] : mesh_vertex_buffer, 0U,
            VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo index_info{mesh_index_buffer, 0U, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo table_info{trace_instance_buffers[current_frame], 0U, VK_WHOLE_SIZE};
        const VkDescriptorImageInfo radiance_info{VK_NULL_HANDLE, targets.radiance, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo variance_info{VK_NULL_HANDLE, targets.variance, VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo depth_info{tone_sampler, current.depth_value.view,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo normal_info{tone_sampler, current.normal_roughness.view,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo noise_info{tone_sampler, reflections->noise(0U),
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const auto scene_structure = acceleration_structures->scene(static_cast<std::uint32_t>(current_frame));
        VkWriteDescriptorSetAccelerationStructureKHR structure_info{};
        structure_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        structure_info.accelerationStructureCount = 1U;
        structure_info.pAccelerationStructures = &scene_structure;
        const auto write = [](const VkDescriptorSet set, const std::uint32_t binding,
                              const VkDescriptorType type) {
            VkWriteDescriptorSet entry{};
            entry.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entry.dstSet = set;
            entry.dstBinding = binding;
            entry.descriptorCount = 1U;
            entry.descriptorType = type;
            return entry;
        };
        const auto trace_set = trace_sets[current_frame];
        std::array<VkWriteDescriptorSet, 14> writes{
            write(trace_set, 0U, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR),
            write(trace_set, 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(trace_set, 2U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(trace_set, 3U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            write(trace_set, 4U, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            write(trace_set, 5U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            write(trace_set, 6U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            write(trace_set, 7U, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            write(trace_set, 8U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(trace_set, 9U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(trace_set, 10U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(trace_set, 11U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(arguments_sets[current_frame], 0U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            write(arguments_sets[current_frame], 1U, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)};
        writes[0].pNext = &structure_info;
        writes[1].pBufferInfo = &rays_info;
        writes[2].pBufferInfo = &counter_info;
        writes[3].pImageInfo = &radiance_info;
        writes[4].pImageInfo = &variance_info;
        writes[5].pImageInfo = &depth_info;
        writes[6].pImageInfo = &normal_info;
        writes[7].pImageInfo = &noise_info;
        writes[8].pBufferInfo = &static_info;
        writes[9].pBufferInfo = &deformed_info;
        writes[10].pBufferInfo = &index_info;
        writes[11].pBufferInfo = &table_info;
        writes[12].pBufferInfo = &counter_info;
        writes[13].pBufferInfo = &arguments_info;
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U,
                               nullptr);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, arguments_pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, arguments_pipeline_layout, 0U,
                                1U, &arguments_sets[current_frame], 0U, nullptr);
        vkCmdDispatch(commands, 1U, 1U, 1U);
        compute_barrier(commands, VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline);
        const std::array trace_bind{texture_sets[current_frame], trace_set};
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_layout, 0U,
                                static_cast<std::uint32_t>(trace_bind.size()), trace_bind.data(), 0U,
                                nullptr);
        TraceConstants constants{};
        constants.inverse_view_projection = invert_matrix(render_scene.camera.view_projection.values);
        constants.camera_position = {static_cast<float>(render_scene.camera_position.x),
                                     static_cast<float>(render_scene.camera_position.y),
                                     static_cast<float>(render_scene.camera_position.z), 1.0F};
        constants.extent = {scene_extent.width, scene_extent.height,
                            static_cast<std::uint32_t>(frame.frame_index), 0U};
        vkCmdPushConstants(commands, trace_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0U,
                           sizeof(constants), &constants);
        vkCmdDispatchIndirect(commands, targets.indirect_arguments, reflection_arguments_hardware);
        compute_barrier(commands, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        return reflections->record_denoising(frame, reflections_error);
#else
        (void)commands;
        (void)render_scene;
        return false;
#endif
    }

    // Points this frame's composite descriptors at the G-buffer and whichever lighting results
    // run this frame; an effect that is off gets a placeholder its flag keeps unread.
    void update_composite_descriptors() {
        const auto& current = scene_targets[current_frame];
        const auto placeholder = current.normal_roughness.view;
        VkImageView diffuse = placeholder, specular = placeholder, reflected = placeholder;
#ifdef RELAY_HAS_FIDELITYFX
        if (global_illumination_active) {
            diffuse = global_illumination->diffuse_view();
            specular = global_illumination->specular_view();
        }
        if (reflections_active) reflected = reflections->output();
#endif
        const std::array<VkImageView, 7> views{current.normal_roughness.view, current.albedo_metallic.view,
                                               current.motion_occlusion.view, current.depth_value.view,
                                               diffuse, specular, reflected};
        std::array<VkDescriptorImageInfo, 7> images{};
        std::array<VkWriteDescriptorSet, 7> writes{};
        for (std::uint32_t binding = 0; binding < views.size(); ++binding) {
            images[binding] = {tone_sampler, views[binding], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = composite_sets[current_frame];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1U;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = &images[binding];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0U,
                               nullptr);
    }

    // Records the distance field update and the GI dispatch between the geometry and forward
    // passes, and points this frame's composite descriptors at the results.
    bool record_global_illumination(const VkCommandBuffer commands, const RenderScene& render_scene,
                                    const std::vector<std::uint64_t>& instance_keys) {
#ifdef RELAY_HAS_FIDELITYFX
        const auto& current = scene_targets[current_frame];
        const auto& history = scene_targets[(current_frame + 1U) % frames_in_flight];
        LightingFrame frame;
        frame.commands = commands;
        frame.frame_index = ++lighting_frame_index;
        frame.extent = scene_extent;
        frame.depth = {current.depth_value.image, current.depth_value.info};
        frame.normal_roughness = {current.normal_roughness.image, current.normal_roughness.info};
        frame.motion = {current.motion_occlusion.image, current.motion_occlusion.info};
        frame.history_depth = {history.depth_value.image, history.depth_value.info};
        frame.history_normal_roughness = {history.normal_roughness.image,
                                          history.normal_roughness.info};
        frame.previous_lit = {history.diffuse_light.image, history.diffuse_light.info};
        frame.view = render_scene.camera.view.values;
        frame.projection = render_scene.camera.projection.values;
        frame.previous_view = previous_view;
        frame.previous_projection = previous_projection;
        frame.camera_position = {static_cast<float>(render_scene.camera_position.x),
                                 static_cast<float>(render_scene.camera_position.y),
                                 static_cast<float>(render_scene.camera_position.z)};
        frame.sky_color = sky_radiance;
        frame.ground_color = ground_radiance;
        frame.index_buffer = mesh_index_buffer;
        frame.index_buffer_bytes = mesh_index_capacity;
        for (std::size_t index = 0; index < render_scene.instances.size(); ++index) {
            const auto& draw_instance = render_scene.instances[index];
            if (draw_instance.alpha_blended) continue;
            const auto* mesh = assets->find_mesh(draw_instance.mesh);
            if (mesh == nullptr || mesh->index_count < 3U) continue;
            LightingInstance lighting_instance;
            lighting_instance.key = instance_keys[index];
            lighting_instance.model = draw_instance.model.values;
            lighting_instance.deformed = draw_instance.deformed_vertex_offset >= 0;
            lighting_instance.vertex_buffer = lighting_instance.deformed ? deformed_buffers[current_frame]
                                                       : mesh_vertex_buffer;
            lighting_instance.vertex_buffer_bytes = lighting_instance.deformed ? deformed_capacities[current_frame]
                                                             : mesh_vertex_capacity;
            lighting_instance.vertex_stride = sizeof(MeshVertex);
            lighting_instance.first_vertex = static_cast<std::uint32_t>(
                lighting_instance.deformed ? draw_instance.deformed_vertex_offset : mesh->vertex_offset);
            lighting_instance.vertex_count = mesh->vertex_count;
            lighting_instance.first_index = mesh->first_index;
            lighting_instance.index_count = mesh->index_count;
            // World bounds of the local box's corners. Deformation can exceed the bind-pose box,
            // so deformed meshes get a margin.
            const float margin = lighting_instance.deformed ? 0.25F : 0.0F;
            lighting_instance.bounds_min = {std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max()};
            lighting_instance.bounds_max = {-std::numeric_limits<float>::max(),
                                   -std::numeric_limits<float>::max(),
                                   -std::numeric_limits<float>::max()};
            const auto& model = lighting_instance.model;
            for (std::uint32_t corner = 0; corner < 8U; ++corner) {
                const std::array<float, 3> local{
                    (corner & 1U) ? mesh->bounds_max[0] : mesh->bounds_min[0],
                    (corner & 2U) ? mesh->bounds_max[1] : mesh->bounds_min[1],
                    (corner & 4U) ? mesh->bounds_max[2] : mesh->bounds_min[2]};
                for (std::size_t axis = 0; axis < 3U; ++axis) {
                    const float value = model[axis] * local[0] + model[4 + axis] * local[1] +
                                        model[8 + axis] * local[2] + model[12 + axis];
                    lighting_instance.bounds_min[axis] = std::min(lighting_instance.bounds_min[axis], value - margin);
                    lighting_instance.bounds_max[axis] = std::max(lighting_instance.bounds_max[axis], value + margin);
                }
            }
            frame.instances.push_back(lighting_instance);
        }
        if (!global_illumination->record(frame, global_illumination_error)) return false;
        return true;
#else
        (void)commands;
        (void)render_scene;
        (void)instance_keys;
        return false;
#endif
    }

    // Ends the GPU pass that began at the previous timestamp and names it for the profiler.
    void mark_gpu_pass(const VkCommandBuffer commands, const std::uint32_t name) {
        auto& count = gpu_marker_counts[current_frame];
        if (timestamp_queries == VK_NULL_HANDLE || count == 0U || count >= gpu_marker_capacity) return;
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_queries,
                            static_cast<std::uint32_t>(current_frame) * gpu_marker_capacity + count);
        gpu_marker_names[current_frame][count++] = name;
    }

    // Reads the timestamps of the frame whose fence just completed and reports its passes.
    void read_gpu_timings() {
        const auto count = gpu_marker_counts[current_frame];
        if (timestamp_queries == VK_NULL_HANDLE || !timestamp_submitted[current_frame] || count < 2U)
            return;
        std::array<std::uint64_t, gpu_marker_capacity> timestamps{};
        const auto query_result = vkGetQueryPoolResults(
            device, timestamp_queries, static_cast<std::uint32_t>(current_frame) * gpu_marker_capacity,
            count, sizeof(std::uint64_t) * count, timestamps.data(), sizeof(std::uint64_t),
            VK_QUERY_RESULT_64_BIT);
        if (query_result != VK_SUCCESS || timestamps[count - 1U] < timestamps[0]) return;
        const auto milliseconds = [&](const std::uint64_t from, const std::uint64_t to) {
            return to >= from ? static_cast<double>(to - from) * timestamp_period_nanoseconds / 1'000'000.0
                              : 0.0;
        };
        latest_gpu_milliseconds = milliseconds(timestamps[0], timestamps[count - 1U]);
        std::vector<ProfileGpuPass> passes;
        passes.reserve(count - 1U);
        for (std::uint32_t marker = 1; marker < count; ++marker)
            passes.push_back({gpu_marker_names[current_frame][marker],
                              milliseconds(timestamps[marker - 1U], timestamps[marker])});
        profiler().record_gpu(gpu_profile_frames[current_frame], passes, latest_gpu_milliseconds);
    }

    bool record_commands(const VkCommandBuffer commands, const std::uint32_t image_index,
                         const float elapsed_seconds, const Scene* scene,
                         const VkBuffer capture_buffer) {
        static const auto shadow_pass = profiler().intern("Shadow maps");
        static const auto point_shadow_pass = profiler().intern("Point light shadows");
        static const auto geometry_pass = profiler().intern("Geometry (G-buffer)");
        static const auto global_illumination_pass = profiler().intern("Global illumination");
        static const auto reflections_pass = profiler().intern("Ray traced reflections");
        static const auto forward_pass = profiler().intern("Composite, transparency and overlays");
        static const auto display_pass = profiler().intern("Tone mapping and editor UI");
        static const auto capture_pass = profiler().intern("Capture copy");
        static const auto build_stage = profiler().intern("Build render scene");
        static const auto upload_stage = profiler().intern("Upload frame data");
        static const auto record_stage = profiler().intern("Record draw commands");
        const auto region = (overlay ? overlay->scene_viewport() : EditorViewport{}).pixels(
            swapchain_extent.width, swapchain_extent.height);
        if (!ensure_scene_targets({region.width, region.height})) return false;
        const bool lighting_wanted = scene != nullptr && prepare_global_illumination();
        const bool reflections_wanted = scene != nullptr && prepare_reflections();
        // Both effects start once the other frame's targets can serve as history.
        const bool history_ready = previous_frame_valid &&
                                   scene_targets_rendered[(current_frame + 1U) % frames_in_flight];
        global_illumination_active = lighting_wanted && history_ready;
        reflections_active = reflections_wanted && history_ready;
        RenderScene render_scene;
        std::optional<ProfileScope> cpu_stage;
        cpu_stage.emplace(build_stage);
        if (scene)
            render_scene =
                build_render_scene(*scene, *assets,
                                   static_cast<float>(region.width) / static_cast<float>(region.height),
                                   overlay != nullptr ? overlay->view_override() : nullptr, false,
                                   lighting_wanted || reflections_wanted,
                                   capture_buffer == VK_NULL_HANDLE ? render_interpolation : nullptr);
        if (render_scene.deformation_overflow) {
            last_error = "scene exceeds four million deformed vertices per frame";
            return false;
        }
        cpu_stage.reset();
        cpu_stage.emplace(upload_stage);
        // This frame's fence has completed. Host writes never race another frame's reads.
        const VkDeviceSize bytes = render_scene.deformed_vertices.size() * sizeof(MeshVertex);
        if (bytes > deformed_capacities[current_frame]) {
            vkDestroyBuffer(device, deformed_buffers[current_frame], nullptr);
            vkFreeMemory(device, deformed_memories[current_frame], nullptr);
            deformed_buffers[current_frame] = VK_NULL_HANDLE;
            deformed_memories[current_frame] = VK_NULL_HANDLE;
            if (!create_buffer(bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometry_buffer_usage(),
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               deformed_buffers[current_frame], deformed_memories[current_frame]))
                return false;
            deformed_capacities[current_frame] = bytes;
        }
        const auto write_memory = [&](VkDeviceMemory memory, const void *data, VkDeviceSize size) {
            void *mapped = nullptr;
            const auto result = vkMapMemory(device, memory, 0, size, 0, &mapped);
            if (result != VK_SUCCESS) {
                last_error = vk_error("animation/light frame upload", result);
                return false;
            }
            std::memcpy(mapped, data, static_cast<std::size_t>(size));
            vkUnmapMemory(device, memory);
            return true;
        };
        if (bytes && !write_memory(deformed_memories[current_frame],
                                   render_scene.deformed_vertices.data(), bytes))
            return false;
        GpuLighting lighting;
        lighting.camera_count = {
            static_cast<float>(render_scene.camera_position.x),
            static_cast<float>(render_scene.camera_position.y),
            static_cast<float>(render_scene.camera_position.z),
            static_cast<float>(std::min<std::size_t>(16, render_scene.lights.size()))};
        for (std::size_t i = 0; i < std::min<std::size_t>(16, render_scene.lights.size()); ++i) {
            const auto &source = render_scene.lights[i];
            auto &target = lighting.lights[i];
            const auto &l = source.light;
            target.position_type = {
                static_cast<float>(source.position.x), static_cast<float>(source.position.y),
                static_cast<float>(source.position.z), static_cast<float>(l.type)};
            target.direction_inner = {
                static_cast<float>(source.direction.x), static_cast<float>(source.direction.y),
                static_cast<float>(source.direction.z), static_cast<float>(std::cos(l.inner_cone))};
            target.color_intensity = {static_cast<float>(l.color.x), static_cast<float>(l.color.y),
                                      static_cast<float>(l.color.z),
                                      static_cast<float>(l.intensity)};
            target.attenuation_outer = {
                static_cast<float>(l.attenuation.x), static_cast<float>(l.attenuation.y),
                static_cast<float>(l.attenuation.z), static_cast<float>(std::cos(l.outer_cone))};
            target.range[0] = static_cast<float>(l.range);
        }
        for (std::size_t cascade = 0; cascade < directional_shadow_cascade_count; ++cascade) {
            lighting.shadow_view_projections[cascade] =
                render_scene.directional_shadow.view_projections[cascade].values;
            lighting.shadow_splits[cascade] =
                render_scene.directional_shadow.split_depths[cascade];
        }
        lighting.spot_shadow_view_projection = render_scene.spot_shadow.view_projection.values;
        for (std::size_t face = 0; face < point_shadow_face_count; ++face)
            lighting.point_shadow_view_projections[face] =
                render_scene.point_shadow.view_projections[face].values;
        lighting.camera_forward = {static_cast<float>(render_scene.camera_forward.x),
                                   static_cast<float>(render_scene.camera_forward.y),
                                   static_cast<float>(render_scene.camera_forward.z),
                                   static_cast<float>((global_illumination_active ? 1U : 0U) |
                                                      (reflections_active ? 2U : 0U))};
        lighting.point_shadow_position_far = {
            static_cast<float>(render_scene.point_shadow.position.x),
            static_cast<float>(render_scene.point_shadow.position.y),
            static_cast<float>(render_scene.point_shadow.position.z),
            render_scene.point_shadow.far_plane};
        lighting.shadow_parameters[0] = render_scene.directional_shadow.enabled ? 1U : 0U;
        lighting.shadow_parameters[1] =
            static_cast<std::uint32_t>(render_scene.directional_shadow.light_index);
        lighting.shadow_parameters[2] = render_scene.spot_shadow.enabled ? 1U : 0U;
        lighting.shadow_parameters[3] =
            static_cast<std::uint32_t>(render_scene.spot_shadow.light_index);
        lighting.point_shadow_parameters[0] = render_scene.point_shadow.enabled ? 1U : 0U;
        lighting.point_shadow_parameters[1] =
            static_cast<std::uint32_t>(render_scene.point_shadow.light_index);
        lighting.point_shadow_parameters[2] =
            std::bit_cast<std::uint32_t>(render_scene.point_shadow.near_plane);
        if (!write_memory(lighting_memories[current_frame], &lighting, sizeof(lighting)))
            return false;

        // Per-draw data. Each drawn instance's previous model-view-projection comes from last
        // frame's camera and the same draw's last model matrix, keyed by entity, mesh and the
        // instance's position among that entity's draws.
        std::vector<GpuDraw> draws;
        std::vector<std::uint32_t> draw_slots(render_scene.instances.size(), no_draw_slot);
        std::unordered_map<std::uint64_t, std::array<float, 16>> current_models;
        std::unordered_map<std::uint64_t, std::uint32_t> occurrences;
        std::vector<std::uint64_t> instance_keys(render_scene.instances.size());
        for (std::size_t index = 0; index < render_scene.instances.size(); ++index) {
            const auto& draw_instance = render_scene.instances[index];
            auto key = draw_instance.entity.packed() * 0x9E3779B97F4A7C15ULL ^
                       std::hash<std::string>{}(draw_instance.mesh);
            key += occurrences[key]++;
            instance_keys[index] = key;
        }
        const auto& view_projection = render_scene.camera.view_projection.values;
        for (std::size_t index = 0; index < render_scene.instances.size(); ++index) {
            const auto& draw_instance = render_scene.instances[index];
            if (!draw_instance.camera_visible) continue;
            const auto key = instance_keys[index];
            current_models[key] = draw_instance.model.values;
            GpuDraw draw{};
            const auto previous = previous_models.find(key);
            draw.previous_model_view_projection =
                previous_frame_valid && previous != previous_models.end()
                    ? multiply_matrices(previous_view_projection, previous->second)
                    : draw_instance.model_view_projection.values;
            draw.material[0] = draw_instance.material_index;
            draw_slots[index] = static_cast<std::uint32_t>(draws.size());
            draws.push_back(draw);
        }
        if (scene == nullptr) {
            // The placeholder triangle has no scene instance; it draws with slot 0.
            GpuDraw draw{};
            draw.previous_model_view_projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            draw.material[0] = assets->material_index("builtin.orange");
            draws.push_back(draw);
        }
        if (draws.empty()) draws.emplace_back();
        previous_models = std::move(current_models);
        previous_view_projection = view_projection;
        previous_frame_valid = true;
        const VkDeviceSize draw_bytes = draws.size() * sizeof(GpuDraw);
        if (draw_bytes > draw_capacities[current_frame]) {
            vkDestroyBuffer(device, draw_buffers[current_frame], nullptr);
            vkFreeMemory(device, draw_memories[current_frame], nullptr);
            draw_buffers[current_frame] = VK_NULL_HANDLE;
            draw_memories[current_frame] = VK_NULL_HANDLE;
            draw_capacities[current_frame] = 0U;
            const auto capacity = growing_capacity(draw_bytes);
            if (!create_buffer(capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               draw_buffers[current_frame], draw_memories[current_frame]))
                return false;
            draw_capacities[current_frame] = capacity;
        }
        if (!write_memory(draw_memories[current_frame], draws.data(), draw_bytes)) return false;
        // Written every frame: asset uploads can replace the descriptor sets.
        const VkDescriptorBufferInfo draw_info{draw_buffers[current_frame], 0U, draw_bytes};
        VkWriteDescriptorSet draw_write{};
        draw_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        draw_write.dstSet = texture_sets[current_frame];
        draw_write.dstBinding = 5U;
        draw_write.descriptorCount = 1U;
        draw_write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        draw_write.pBufferInfo = &draw_info;
        vkUpdateDescriptorSets(device, 1U, &draw_write, 0U, nullptr);
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        auto result = vkBeginCommandBuffer(commands, &begin_info);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkBeginCommandBuffer", result);
            return false;
        }
        cpu_stage.reset();
        cpu_stage.emplace(record_stage);
        gpu_marker_counts[current_frame] = 0U;
        gpu_profile_frames[current_frame] = profiler().current_frame();
        if (timestamp_queries != VK_NULL_HANDLE) {
            const auto first_query = static_cast<std::uint32_t>(current_frame) * gpu_marker_capacity;
            vkCmdResetQueryPool(commands, timestamp_queries, first_query, gpu_marker_capacity);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_queries, first_query);
            gpu_marker_counts[current_frame] = 1U;
        }
        latest_draw_calls = 0U;
        VkClearValue shadow_clear{};
        shadow_clear.depthStencil = {1.0F, 0U};
        for (std::size_t map = 0; map < shadow_map_count; ++map) {
            const auto& shadow_attachment =
                shadow_attachments[current_frame * shadow_map_count + map];
            VkViewport shadow_viewport{0.0F, 0.0F,
                                       static_cast<float>(shadow_attachment.extent),
                                       static_cast<float>(shadow_attachment.extent), 0.0F, 1.0F};
            VkRect2D shadow_scissor{{0, 0},
                                    {shadow_attachment.extent, shadow_attachment.extent}};
            VkRenderPassBeginInfo shadow_info{};
            shadow_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            shadow_info.renderPass = shadow_render_pass;
            shadow_info.framebuffer = shadow_attachment.framebuffer;
            shadow_info.renderArea.extent = {shadow_attachment.extent,
                                             shadow_attachment.extent};
            shadow_info.clearValueCount = 1U;
            shadow_info.pClearValues = &shadow_clear;
            vkCmdBeginRenderPass(commands, &shadow_info, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    shadow_pipeline_layout, 0U, 1U,
                                    &texture_sets[current_frame], 0U, nullptr);
            vkCmdSetViewport(commands, 0U, 1U, &shadow_viewport);
            vkCmdSetScissor(commands, 0U, 1U, &shadow_scissor);
            const bool map_enabled = map < directional_shadow_cascade_count
                                         ? render_scene.directional_shadow.enabled
                                         : render_scene.spot_shadow.enabled;
            if (!map_enabled) {
                vkCmdEndRenderPass(commands);
                continue;
            }
            const VkDeviceSize vertex_offset = 0U;
            vkCmdBindIndexBuffer(commands, mesh_index_buffer, 0U, VK_INDEX_TYPE_UINT32);
            for (const auto& draw_instance : render_scene.instances) {
                if (draw_instance.alpha_blended ||
                    (draw_instance.shadow_cascade_mask & (1U << map)) == 0U)
                    continue;
                const auto* mesh = assets->find_mesh(draw_instance.mesh);
                if (!mesh) continue;
                const auto buffer = draw_instance.deformed_vertex_offset >= 0
                                        ? deformed_buffers[current_frame] : mesh_vertex_buffer;
                vkCmdBindVertexBuffers(commands, 0U, 1U, &buffer, &vertex_offset);
                DrawPushConstants constants{};
                const auto& shadow_view_projection = map < directional_shadow_cascade_count
                                                         ? render_scene.directional_shadow
                                                               .view_projections[map]
                                                         : render_scene.spot_shadow.view_projection;
                constants.model_view_projection = multiply_matrices(
                    shadow_view_projection.values, draw_instance.model.values);
                constants.model = draw_instance.model.values;
                vkCmdPushConstants(commands, shadow_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                                   0U, sizeof(constants), &constants);
                vkCmdDrawIndexed(commands, mesh->index_count, 1U, mesh->first_index,
                                 draw_instance.deformed_vertex_offset >= 0
                                     ? draw_instance.deformed_vertex_offset : mesh->vertex_offset,
                                 draw_instance.material_index);
                ++latest_draw_calls;
            }
            vkCmdEndRenderPass(commands);
        }
        mark_gpu_pass(commands, shadow_pass);
        for (std::size_t face = 0; face < point_shadow_face_count; ++face) {
            VkRenderPassBeginInfo shadow_info{};
            shadow_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            shadow_info.renderPass = shadow_render_pass;
            shadow_info.framebuffer = point_shadow_attachments[current_frame].framebuffers[face];
            shadow_info.renderArea.extent = {point_shadow_resolution, point_shadow_resolution};
            shadow_info.clearValueCount = 1U;
            shadow_info.pClearValues = &shadow_clear;
            vkCmdBeginRenderPass(commands, &shadow_info, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    shadow_pipeline_layout, 0U, 1U,
                                    &texture_sets[current_frame], 0U, nullptr);
            VkViewport viewport{0.0F, 0.0F, static_cast<float>(point_shadow_resolution),
                                static_cast<float>(point_shadow_resolution), 0.0F, 1.0F};
            VkRect2D scissor{{0, 0}, {point_shadow_resolution, point_shadow_resolution}};
            vkCmdSetViewport(commands, 0U, 1U, &viewport);
            vkCmdSetScissor(commands, 0U, 1U, &scissor);
            if (render_scene.point_shadow.enabled) {
                const VkDeviceSize vertex_offset = 0U;
                vkCmdBindIndexBuffer(commands, mesh_index_buffer, 0U, VK_INDEX_TYPE_UINT32);
                for (const auto& draw_instance : render_scene.instances) {
                    if (draw_instance.alpha_blended ||
                        (draw_instance.shadow_cascade_mask &
                         (1U << (point_shadow_mask_offset + face))) == 0U)
                        continue;
                    const auto* mesh = assets->find_mesh(draw_instance.mesh);
                    if (!mesh) continue;
                    const auto buffer = draw_instance.deformed_vertex_offset >= 0
                                            ? deformed_buffers[current_frame] : mesh_vertex_buffer;
                    vkCmdBindVertexBuffers(commands, 0U, 1U, &buffer, &vertex_offset);
                    DrawPushConstants constants{};
                    constants.model_view_projection = multiply_matrices(
                        render_scene.point_shadow.view_projections[face].values,
                        draw_instance.model.values);
                    constants.model = draw_instance.model.values;
                    vkCmdPushConstants(commands, shadow_pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(constants),
                                       &constants);
                    vkCmdDrawIndexed(commands, mesh->index_count, 1U, mesh->first_index,
                                     draw_instance.deformed_vertex_offset >= 0
                                         ? draw_instance.deformed_vertex_offset
                                         : mesh->vertex_offset,
                                     draw_instance.material_index);
                    ++latest_draw_calls;
                }
            }
            vkCmdEndRenderPass(commands);
        }
        mark_gpu_pass(commands, point_shadow_pass);
        std::array<VkClearValue, geometry_color_attachments + 1U> clear{};
        clear[0].color = overlay ? VkClearColorValue{{0.014444F, 0.014444F, 0.014444F, 1.0F}}
                                 : VkClearColorValue{{0.012F, 0.018F, 0.045F, 1.0F}};
        // Empty pixels have no normal, which lighting effects read as background.
        clear[1].color = VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}};
        clear[4].color = VkClearColorValue{{1.0F, 0.0F, 0.0F, 0.0F}};
        // Reversed-Z is not in use, so the far plane clears to 1.0 and LESS keeps the nearest write.
        clear[geometry_color_attachments].depthStencil = {1.0F, 0U};
        const auto& targets = scene_targets[current_frame];
        VkRenderPassBeginInfo scene_pass_info{};
        scene_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        scene_pass_info.renderPass = geometry_render_pass;
        scene_pass_info.framebuffer = targets.geometry_framebuffer;
        scene_pass_info.renderArea.extent = scene_extent;
        scene_pass_info.clearValueCount = static_cast<std::uint32_t>(clear.size());
        scene_pass_info.pClearValues = clear.data();
        vkCmdBeginRenderPass(commands, &scene_pass_info, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport scene_viewport{};
        scene_viewport.width = static_cast<float>(scene_extent.width);
        scene_viewport.height = static_cast<float>(scene_extent.height);
        scene_viewport.maxDepth = 1.0F;
        const VkRect2D scene_scissor{{0, 0}, scene_extent};
        const VkDeviceSize vertex_offset = 0U;
        const auto bind_scene_state = [&](const VkPipeline scene_pipeline) {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline);
            vkCmdSetViewport(commands, 0, 1, &scene_viewport);
            vkCmdSetScissor(commands, 0, 1, &scene_scissor);
            vkCmdBindVertexBuffers(commands, 0U, 1U, &mesh_vertex_buffer, &vertex_offset);
            vkCmdBindIndexBuffer(commands, mesh_index_buffer, 0U, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0U,
                                    1U, &texture_sets[current_frame], 0U, nullptr);
        };
        const auto draw_instances = [&](const bool alpha_blended) {
            for (std::size_t index = 0; index < render_scene.instances.size(); ++index) {
                const auto& draw_instance = render_scene.instances[index];
                if (!draw_instance.camera_visible || draw_instance.alpha_blended != alpha_blended)
                    continue;
                const auto* mesh = assets->find_mesh(draw_instance.mesh);
                if (mesh == nullptr || draw_slots[index] == no_draw_slot) continue;
                const auto buffer = draw_instance.deformed_vertex_offset >= 0
                                        ? deformed_buffers[current_frame] : mesh_vertex_buffer;
                vkCmdBindVertexBuffers(commands, 0, 1, &buffer, &vertex_offset);
                const DrawPushConstants constants{draw_instance.model_view_projection.values,
                                                  draw_instance.model.values};
                vkCmdPushConstants(commands, pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(constants), &constants);
                vkCmdDrawIndexed(commands, mesh->index_count, 1U, mesh->first_index,
                                 draw_instance.deformed_vertex_offset >= 0
                                     ? draw_instance.deformed_vertex_offset : mesh->vertex_offset,
                                 draw_slots[index]);
                ++latest_draw_calls;
            }
        };
        bind_scene_state(pipeline);
        if (scene != nullptr && !scene->entities().empty()) {
            draw_instances(false);
        } else if (scene == nullptr) {
            const float angle = elapsed_seconds * 0.35F;
            const float cosine = std::cos(angle);
            const float sine = std::sin(angle);
            DrawPushConstants constants{};
            constants.model_view_projection = {cosine, sine, 0.0F, 0.0F,
                                                -sine, cosine, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F,
                                                0.0F, 0.0F, 0.0F, 1.0F};
            constants.model = {1.0F, 0.0F, 0.0F, 0.0F,
                               0.0F, 1.0F, 0.0F, 0.0F,
                               0.0F, 0.0F, 1.0F, 0.0F,
                               0.0F, 0.0F, 0.0F, 1.0F};
            vkCmdPushConstants(commands, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(constants), &constants);
            if (const auto* mesh = assets->find_mesh("builtin.triangle")) {
                vkCmdDrawIndexed(commands, mesh->index_count, 1U, mesh->first_index,
                                 mesh->vertex_offset, 0U);
            }
            latest_draw_calls = 1U;
        }
        vkCmdEndRenderPass(commands);
        mark_gpu_pass(commands, geometry_pass);
        if (global_illumination_active) {
            RELAY_PROFILE_SCOPE("Record global illumination");
            if (!record_global_illumination(commands, render_scene, instance_keys)) {
                // The analytic sky light was left out of this frame; later frames fall back to it.
                global_illumination_active = false;
                global_illumination_failed = true;
            }
            mark_gpu_pass(commands, global_illumination_pass);
        }
        if (reflections_active) {
            RELAY_PROFILE_SCOPE("Record reflections");
            if (!record_reflections(commands, render_scene)) {
                reflections_active = false;
                reflections_failed = true;
            }
            mark_gpu_pass(commands, reflections_pass);
        }

        // Forward pass: transparent geometry, then editor-only overlays.
        scene_pass_info.renderPass = scene_render_pass;
        scene_pass_info.framebuffer = targets.forward_framebuffer;
        scene_pass_info.clearValueCount = 0U;
        scene_pass_info.pClearValues = nullptr;
        vkCmdBeginRenderPass(commands, &scene_pass_info, VK_SUBPASS_CONTENTS_INLINE);
        if (global_illumination_active || reflections_active) {
            update_composite_descriptors();
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline);
            vkCmdSetViewport(commands, 0, 1, &scene_viewport);
            vkCmdSetScissor(commands, 0, 1, &scene_scissor);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    composite_pipeline_layout, 0U, 1U,
                                    &composite_sets[current_frame], 0U, nullptr);
            CompositeConstants constants{};
            constants.inverse_view_projection = invert_matrix(view_projection);
            constants.camera_position = {static_cast<float>(render_scene.camera_position.x),
                                         static_cast<float>(render_scene.camera_position.y),
                                         static_cast<float>(render_scene.camera_position.z), 1.0F};
            constants.extent = {static_cast<float>(scene_extent.width),
                                static_cast<float>(scene_extent.height),
                                static_cast<float>((global_illumination_active ? 1U : 0U) |
                                                   (reflections_active ? 2U : 0U)),
                                reflection_roughness_limit};
            vkCmdPushConstants(commands, composite_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0U, sizeof(constants), &constants);
            vkCmdDraw(commands, 3U, 1U, 0U, 0U);
            ++latest_draw_calls;
        }
        bind_scene_state(transparent_pipeline);
        if (scene != nullptr) draw_instances(true);
        {
            if (overlay && overlay->ground_grid_visible() && capture_buffer == VK_NULL_HANDLE) {
                DrawPushConstants grid_constants{};
                grid_constants.model_view_projection = render_scene.camera.view_projection.values;
                grid_constants.model[0] = static_cast<float>(render_scene.camera_position.x);
                grid_constants.model[1] = static_cast<float>(render_scene.camera_position.y);
                grid_constants.model[2] = static_cast<float>(render_scene.camera_position.z);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, grid_pipeline);
                vkCmdPushConstants(commands, pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(grid_constants), &grid_constants);
                vkCmdDraw(commands, 6, 2, 0, 0);
            }
            const auto selected = overlay && capture_buffer == VK_NULL_HANDLE
                                      ? overlay->selected_entities() : std::vector<Entity>{};
            if (!selected.empty()) {
                const auto draw_selection = [&](bool outline) {
                    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      outline ? selection_outline_pipeline : selection_mask_pipeline);
                    for (const auto& draw_instance : render_scene.instances) {
                        if (!draw_instance.camera_visible) continue;
                        Entity ancestor = draw_instance.entity;
                        while (ancestor.valid() && std::find(selected.begin(), selected.end(), ancestor) == selected.end()) {
                            const auto* record = scene ? scene->get(ancestor) : nullptr;
                            ancestor = record ? record->parent : Entity{};
                        }
                        if (!ancestor.valid()) continue;
                        const auto* mesh = assets->find_mesh(draw_instance.mesh);
                        if (!mesh) continue;
                        const auto buffer = draw_instance.deformed_vertex_offset >= 0
                                                ? deformed_buffers[current_frame] : mesh_vertex_buffer;
                        vkCmdBindVertexBuffers(commands, 0, 1, &buffer, &vertex_offset);
                        DrawPushConstants constants{};
                        constants.model_view_projection = draw_instance.model_view_projection.values;
                        if (outline) {
                            const float scale = SDL_GetWindowDisplayScale(window);
                            const float pixels = 2.0F * (scale > 0.0F ? scale : 1.0F);
                            constants.model[0] = 2.0F * pixels / static_cast<float>(region.width);
                            constants.model[1] = 2.0F * pixels / static_cast<float>(region.height);
                        }
                        // Selection is drawn into the HDR target before exposure and the SDR
                        // post pass. Pre-map the UI amber and cancel scene camera exposure.
                        const float inverse_exposure = std::exp2(-render_scene.camera.exposure_ev);
                        constants.model[4] = std::min(60000.0F, 7.25F * inverse_exposure);
                        constants.model[5] = std::min(60000.0F, 0.342F * inverse_exposure);
                        constants.model[6] = std::min(60000.0F, 0.0387F * inverse_exposure);
                        vkCmdPushConstants(commands, pipeline_layout,
                                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(constants), &constants);
                        vkCmdDrawIndexed(commands, mesh->index_count, outline ? 8U : 1U,
                                         mesh->first_index, draw_instance.deformed_vertex_offset >= 0
                                             ? draw_instance.deformed_vertex_offset : mesh->vertex_offset,
                                         draw_instance.material_index * 8U);
                        ++latest_draw_calls;
                    }
                };
                draw_selection(false);
                draw_selection(true);
            }
        }
        vkCmdEndRenderPass(commands);
        mark_gpu_pass(commands, forward_pass);
        scene_targets_rendered[current_frame] = true;
        previous_view = render_scene.camera.view.values;
        previous_projection = render_scene.camera.projection.values;
        std::array<VkClearValue, 2> swapchain_clear{};
        swapchain_clear[0].color = clear[0].color;
        swapchain_clear[1].depthStencil = {1.0F, 0U};
        VkRenderPassBeginInfo render_info{};
        render_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_info.renderArea.extent = swapchain_extent;
        render_info.clearValueCount = static_cast<std::uint32_t>(swapchain_clear.size());
        render_info.pClearValues = swapchain_clear.data();
        // The display pass converts linear HDR into the swapchain's SDR color space. The editor
        // callback places this image at the viewport's position in ImGui's draw order.
        render_info.renderPass = render_pass;
        render_info.framebuffer = framebuffers[image_index];
        vkCmdBeginRenderPass(commands, &render_info, VK_SUBPASS_CONTENTS_INLINE);
        const auto draw_tone = [&] {
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, tone_pipeline);
            VkViewport viewport{};
            viewport.width = static_cast<float>(swapchain_extent.width);
            viewport.height = static_cast<float>(swapchain_extent.height);
            viewport.maxDepth = 1.0F;
            VkRect2D scissor{};
            scissor.offset = {static_cast<std::int32_t>(region.x),
                              static_cast<std::int32_t>(region.y)};
            scissor.extent = {region.width, region.height};
            vkCmdSetViewport(commands, 0U, 1U, &viewport);
            vkCmdSetScissor(commands, 0U, 1U, &scissor);
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    tone_pipeline_layout, 0U, 1U, &tone_sets[current_frame],
                                    0U, nullptr);
            const ToneSettings settings{
                std::exp2(render_scene.camera.exposure_ev),
                swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                        swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ? 0U : 1U,
                {static_cast<float>(region.x), static_cast<float>(region.y)},
                {static_cast<float>(region.width), static_cast<float>(region.height)}};
            vkCmdPushConstants(commands, tone_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                               0U, sizeof(settings), &settings);
            vkCmdDraw(commands, 3U, 1U, 0U, 0U);
            ++latest_draw_calls;
        };
        if (overlay != nullptr && overlay_ready && capture_buffer == VK_NULL_HANDLE) {
            RELAY_PROFILE_SCOPE("Record editor UI");
            overlay->record(commands, draw_tone);
        } else {
            draw_tone();
        }
        vkCmdEndRenderPass(commands);
        mark_gpu_pass(commands, display_pass);
        if (transfer_source_supported) {
            if (capture_buffer != VK_NULL_HANDLE) {
                VkBufferImageCopy copy{};
                copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copy.imageSubresource.layerCount = 1;
                copy.imageExtent = {swapchain_extent.width, swapchain_extent.height, 1};
                vkCmdCopyImageToBuffer(commands, swapchain_images[image_index],
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_buffer, 1, &copy);
            }
            VkImageMemoryBarrier present_barrier{};
            present_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            present_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                            VK_ACCESS_TRANSFER_READ_BIT;
            present_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            present_barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            present_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            present_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            present_barrier.image = swapchain_images[image_index];
            present_barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            present_barrier.subresourceRange.levelCount = 1;
            present_barrier.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &present_barrier);
        }
        if (capture_buffer != VK_NULL_HANDLE) mark_gpu_pass(commands, capture_pass);
        result = vkEndCommandBuffer(commands);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkEndCommandBuffer", result);
            return false;
        }
        return true;
    }

    bool draw(const double elapsed, const Scene* scene = nullptr,
              const VkBuffer capture_buffer = VK_NULL_HANDLE) {
        last_draw_presented = false;
        if (submission_failed) return false;
        {
            RELAY_PROFILE_SCOPE("Asset uploads and readbacks");
            collect_readbacks(false);
            if (!collect_upload_batch()) return false;
            if (!refresh_mesh_assets()) return false;
        }
        if (swapchain == VK_NULL_HANDLE) {
            RELAY_PROFILE_SCOPE("Recreate swapchain");
            return recreate_swapchain();
        }
        auto result = VK_SUCCESS;
        {
            // Long waits here mean the GPU is still busy with the frame submitted two frames ago.
            RELAY_PROFILE_WAIT("Wait for GPU");
            result = vkWaitForFences(device, 1, &frame_fences[current_frame], VK_TRUE,
                                     std::numeric_limits<std::uint64_t>::max());
        }
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkWaitForFences", result);
            return false;
        }
        for (const auto semaphore : retired_upload_semaphores[current_frame])
            vkDestroySemaphore(device, semaphore, nullptr);
        retired_upload_semaphores[current_frame].clear();
        old_asset_frame_pending[current_frame] = false;
        if (!collect_upload_batch()) return false;
        collect_readbacks(false);
        read_gpu_timings();
        std::uint32_t image_index = 0;
        {
            RELAY_PROFILE_WAIT("Acquire swapchain image");
            result = vkAcquireNextImageKHR(device, swapchain,
                                           std::numeric_limits<std::uint64_t>::max(),
                                           image_available[current_frame], VK_NULL_HANDLE,
                                           &image_index);
        }
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            last_error = vk_error("vkAcquireNextImageKHR", result);
            return false;
        }
        // Build the UI for this frame only when it will actually be presented. A failed overlay is
        // latched so a broken UI degrades to a plain viewport instead of retrying every frame.
        if (overlay != nullptr && !overlay_failed && capture_buffer == VK_NULL_HANDLE) {
            if (!overlay_ready) {
                OverlayContext context{};
                context.sdl_window = window;
                context.api_version = device_api_version;
                context.instance = instance;
                context.physical_device = physical_device;
                context.device = device;
                context.graphics_family = graphics_family;
                context.graphics_queue = graphics_queue;
                context.render_pass = render_pass;
                context.image_count = static_cast<std::uint32_t>(swapchain_images.size());
                context.frames_in_flight = static_cast<std::uint32_t>(frames_in_flight);
                std::string overlay_error;
                if (overlay->initialize(context, overlay_error)) {
                    overlay_ready = true;
                } else {
                    overlay_failed = true;
                    last_error = "editor overlay unavailable: " + overlay_error;
                }
            }
            if (overlay_ready) {
                RELAY_PROFILE_SCOPE("Editor UI");
                overlay->build(swapchain_extent.width, swapchain_extent.height);
            }
        }
        vkResetCommandBuffer(command_buffers[current_frame], 0);
        {
            RELAY_PROFILE_SCOPE("Prepare frame");
            if (!record_commands(command_buffers[current_frame], image_index,
                                 static_cast<float>(elapsed), scene, capture_buffer)) {
                submission_failed = true;
                return false;
            }
        }
        const std::array wait_semaphores{image_available[current_frame], upload_complete};
        const std::array<VkPipelineStageFlags, 2> wait_stages{
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = upload_wait_pending ? 2U : 1U;
        submit_info.pWaitSemaphores = wait_semaphores.data();
        submit_info.pWaitDstStageMask = wait_stages.data();
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffers[current_frame];
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished[image_index];
        vkResetFences(device, 1, &frame_fences[current_frame]);
        {
            RELAY_PROFILE_SCOPE("Queue submit");
            result = vkQueueSubmit(graphics_queue, 1, &submit_info, frame_fences[current_frame]);
        }
        if (result != VK_SUCCESS) {
            submission_failed = true;
            last_error = vk_error("vkQueueSubmit", result);
            return false;
        }
        if (upload_wait_pending) {
            retired_upload_semaphores[current_frame].push_back(upload_complete);
            upload_complete = VK_NULL_HANDLE;
            upload_wait_pending = false;
        }
        timestamp_submitted[current_frame] = timestamp_queries != VK_NULL_HANDLE;
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished[image_index];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        {
            // With vsync, presentation blocks here until the display takes a new image.
            RELAY_PROFILE_WAIT("Present");
            result = vkQueuePresentKHR(present_queue, &present_info);
        }
        last_draw_presented = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || resized) {
            if (!recreate_swapchain()) return false;
        } else if (result != VK_SUCCESS) {
            last_error = vk_error("vkQueuePresentKHR", result);
            return false;
        }
        current_frame = (current_frame + 1U) % frames_in_flight;
        return true;
    }

    std::optional<std::uint32_t> find_memory_type(const std::uint32_t allowed_types,
                                                   const VkMemoryPropertyFlags required) const {
        VkPhysicalDeviceMemoryProperties properties{};
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
            if ((allowed_types & (1U << index)) != 0U &&
                (properties.memoryTypes[index].propertyFlags & required) == required) {
                return index;
            }
        }
        return std::nullopt;
    }

    bool create_capture_buffer(VkBuffer& buffer, VkDeviceMemory& memory) {
        const auto byte_count = static_cast<VkDeviceSize>(swapchain_extent.width) *
                                swapchain_extent.height * 4U;
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = byte_count;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        auto result = vkCreateBuffer(device, &buffer_info, nullptr, &buffer);
        if (result != VK_SUCCESS) {
            last_error = vk_error("capture buffer creation", result);
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        const auto memory_type = find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!memory_type.has_value()) {
            last_error = "no host-visible coherent Vulkan memory is available for frame capture";
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = *memory_type;
        result = vkAllocateMemory(device, &allocate_info, nullptr, &memory);
        if (result != VK_SUCCESS) {
            last_error = vk_error("capture memory allocation", result);
            vkDestroyBuffer(device, buffer, nullptr);
            buffer = VK_NULL_HANDLE;
            return false;
        }
        result = vkBindBufferMemory(device, buffer, memory, 0);
        if (result != VK_SUCCESS) {
            last_error = vk_error("capture buffer memory binding", result);
            vkFreeMemory(device, memory, nullptr);
            vkDestroyBuffer(device, buffer, nullptr);
            memory = VK_NULL_HANDLE;
            buffer = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void collect_readbacks(bool wait) {
        std::array<std::size_t, frames_in_flight> order{0, 1};
        std::sort(order.begin(), order.end(), [&](auto a, auto b) { return readbacks[a].serial < readbacks[b].serial; });
        for (const auto index : order) {
            auto& slot = readbacks[index];
            if (!slot.receiver) continue;
            auto result = wait ? vkWaitForFences(device, 1, &frame_fences[index], VK_TRUE,
                std::numeric_limits<std::uint64_t>::max()) : vkGetFenceStatus(device, frame_fences[index]);
            if (result == VK_NOT_READY) break;
            auto receiver = std::move(slot.receiver);
            slot.receiver = {};
            if (result != VK_SUCCESS) { receiver({}, vk_error("readback fence", result)); continue; }
            const auto size = static_cast<std::size_t>(slot.extent.width) * slot.extent.height * 4U;
            void* mapped = nullptr;
            result = vkMapMemory(device, slot.memory, 0, size, 0, &mapped);
            if (result != VK_SUCCESS) { receiver({}, vk_error("readback mapping", result)); continue; }
            OwnedFrame frame{slot.extent.width, slot.extent.height, std::vector<std::uint8_t>(size)};
            std::memcpy(frame.rgba.data(), mapped, size);
            vkUnmapMemory(device, slot.memory);
            if (slot.format == VK_FORMAT_B8G8R8A8_SRGB || slot.format == VK_FORMAT_B8G8R8A8_UNORM)
                for (std::size_t offset = 0; offset < size; offset += 4U) std::swap(frame.rgba[offset], frame.rgba[offset+2U]);
            receiver(std::move(frame), {});
        }
    }

    void flush_readbacks() { if (device) collect_readbacks(true); }

    bool readback_async(const Scene* scene, double elapsed, FrameReceiver receiver, std::string& error) {
        collect_readbacks(false);
        if (!transfer_source_supported || swapchain == VK_NULL_HANDLE) {
            error = "Vulkan swapchain readback is unavailable"; return false;
        }
        if (swapchain_format != VK_FORMAT_B8G8R8A8_SRGB && swapchain_format != VK_FORMAT_B8G8R8A8_UNORM &&
            swapchain_format != VK_FORMAT_R8G8B8A8_SRGB && swapchain_format != VK_FORMAT_R8G8B8A8_UNORM) {
            error = "readback requires an 8-bit RGBA or BGRA swapchain"; return false;
        }
        auto& slot = readbacks[current_frame];
        if (slot.receiver) { error = "Vulkan readback ring is full"; return false; }
        // Bound staging memory, even on very large desktop surfaces.
        if (static_cast<std::uint64_t>(swapchain_extent.width) * swapchain_extent.height * 4U > 64U * 1024U * 1024U) {
            error = "Vulkan readback exceeds the 64 MiB slot budget"; return false;
        }
        if (!slot.buffer || slot.extent.width != swapchain_extent.width || slot.extent.height != swapchain_extent.height) {
            vkDestroyBuffer(device, slot.buffer, nullptr); vkFreeMemory(device, slot.memory, nullptr);
            slot.buffer = {}; slot.memory = {};
            if (!create_capture_buffer(slot.buffer, slot.memory)) { error = last_error; return false; }
        }
        slot.extent = swapchain_extent;
        slot.format = swapchain_format;
        const auto index = current_frame;
        if (!draw(elapsed, scene, slot.buffer)) { error = last_error; return false; }
        // An acquire-time resize can return without submitting a copy.
        if (current_frame == index) { error = "swapchain changed before readback submission; retry capture"; return false; }
        slot.serial = ++readback_serial;
        slot.receiver = std::move(receiver);
        error.clear();
        return true;
    }

    bool capture_image(const std::filesystem::path& path, const double elapsed, const Scene* scene) {
        bool captured = false;
        if (!readback_async(scene, elapsed, [&](OwnedFrame frame, std::string failure) {
            if (!failure.empty()) last_error = std::move(failure);
            else captured = write_frame_image(frame.view(), path, last_error);
        }, last_error)) return false;
        flush_readbacks();
        return captured;
    }

};

VulkanWindow::VulkanWindow(std::string title, const std::uint32_t width,
                           const std::uint32_t height, const AssetRegistry& assets,
                           const bool editor_window)
    : impl_(std::make_unique<Impl>()) {
    impl_->assets = &assets;
    impl_->initialize(title, width, height, editor_window);
}

VulkanWindow::~VulkanWindow() = default;
VulkanWindow::VulkanWindow(VulkanWindow&&) noexcept = default;
VulkanWindow& VulkanWindow::operator=(VulkanWindow&&) noexcept = default;

bool VulkanWindow::valid() const {
    return impl_ && impl_->window != nullptr && impl_->device != VK_NULL_HANDLE &&
           impl_->swapchain != VK_NULL_HANDLE && impl_->pipeline != VK_NULL_HANDLE;
}

std::string VulkanWindow::error() const {
    return impl_ ? impl_->last_error : "window has no implementation";
}

std::string VulkanWindow::device_name() const {
    return impl_ ? impl_->selected_device_name : std::string{};
}

bool VulkanWindow::poll_quit() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // The overlay sees every event first. When it claims one, the event is UI interaction
        // rather than player input, so it must not reach the deterministic input trace and must not
        // be interpreted as a quit request.
        sdl_track_gamepads(event);
        const bool consumed_by_overlay =
            impl_->overlay != nullptr && impl_->overlay_ready && impl_->overlay->handle_event(&event);
        if (consumed_by_overlay) {
            if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) impl_->resized = true;
            continue;
        }
        if (auto input = sdl_input_event(event)) impl_->pending_input_events.push_back(std::move(*input));
        if (event.type == SDL_EVENT_QUIT) return true;
        // Escape closes the bare demo window, but in the editor it would throw away unsaved work
        // on a keypress people use to dismiss menus. There, only closing the window quits.
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE &&
            impl_->overlay == nullptr) {
            return true;
        }
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) impl_->resized = true;
    }
    return false;
}

std::vector<std::string> VulkanWindow::drain_input_events() {
    if (!impl_) return {};
    auto events = std::move(impl_->pending_input_events);
    impl_->pending_input_events.clear();
    return events;
}

bool VulkanWindow::draw(const double elapsed_seconds) {
    return impl_ && impl_->draw(elapsed_seconds);
}

bool VulkanWindow::draw(const Scene& scene, const double elapsed_seconds) {
    return impl_ && impl_->draw(elapsed_seconds, &scene);
}

bool VulkanWindow::readback_async(const Scene& scene, double elapsed, FrameReceiver receiver, std::string& error) {
    if (!impl_) { error = "Vulkan window is unavailable"; return false; }
    return impl_->readback_async(&scene, elapsed, std::move(receiver), error);
}
void VulkanWindow::flush_readbacks() { if (impl_) impl_->flush_readbacks(); }

bool VulkanWindow::capture_image(const std::filesystem::path& path, const double elapsed_seconds) {
    return impl_ && impl_->capture_image(path, elapsed_seconds, nullptr);
}

bool VulkanWindow::capture_image(const std::filesystem::path& path, const Scene& scene,
                                 const double elapsed_seconds) {
    return impl_ && impl_->capture_image(path, elapsed_seconds, &scene);
}

double VulkanWindow::gpu_frame_milliseconds() const {
    return impl_ ? impl_->latest_gpu_milliseconds : 0.0;
}

bool VulkanWindow::presented() const { return impl_ && impl_->last_draw_presented; }

void VulkanWindow::set_render_interpolation(const RenderInterpolation* interpolation) {
    if (impl_) impl_->render_interpolation = interpolation;
}

std::uint32_t VulkanWindow::draw_call_count() const {
    return impl_ ? impl_->latest_draw_calls : 0U;
}

std::uint32_t VulkanWindow::render_resource_count() const {
    if (!impl_) return 0U;
    // Every swapchain depth attachment contributes an image, its memory and its view.
    const std::size_t depth_resources = impl_->depth_attachments.size() * 3U;
    // Each frame's scene targets: seven images with memory and views, and two framebuffers.
    const std::size_t scene_resources =
        impl_->scene_targets[0].geometry_framebuffer != VK_NULL_HANDLE
            ? impl_->scene_targets.size() * (7U * 3U + 2U) : 0U;
    // Every cascade owns an image, allocation, view and framebuffer.
    const std::size_t shadow_resources = impl_->shadow_attachments.size() * 4U;
    const std::size_t point_shadow_resources =
        impl_->point_shadow_attachments.size() * (3U + point_shadow_face_count * 2U);
    return static_cast<std::uint32_t>(impl_->swapchain_images.size() + impl_->image_views.size() +
                                      impl_->framebuffers.size() + impl_->textures.size() * 4U +
                                      depth_resources + scene_resources + shadow_resources +
                                      point_shadow_resources + impl_->tone_sets.size() +
                                      impl_->draw_buffers.size() * 2U + 18U);
}

std::string VulkanWindow::render_graph_json() const {
    return impl_ ? impl_->render_graph.json() : "{\"valid\":false}";
}

std::string VulkanWindow::shader_interfaces_json() const {
    if (!impl_) return "{\"vertex\":null,\"fragment\":null}";
    return "{\"vertex\":" + impl_->vertex_interface.json() +
           ",\"fragment\":" + impl_->fragment_interface.json() + '}';
}

std::string VulkanWindow::upload_status_json() const {
    if (!impl_) return "{\"available\":false}";
    return "{\"available\":true,\"staging_budget_bytes\":" +
           std::to_string(impl_->upload_budget.staging_bytes) +
           ",\"device_budget_bytes\":" + std::to_string(impl_->upload_budget.device_bytes) +
           ",\"single_texture_budget_bytes\":" +
           std::to_string(impl_->upload_budget.single_texture_bytes) +
           ",\"staging_bytes\":" + std::to_string(impl_->upload_staging_bytes) +
           ",\"peak_staging_bytes\":" +
           std::to_string(impl_->peak_upload_staging_bytes) +
           ",\"last_batch_bytes\":" + std::to_string(impl_->last_upload_bytes) +
           ",\"estimated_device_bytes\":" +
           std::to_string(impl_->active_device_estimate) +
           ",\"submitted_batches\":" + std::to_string(impl_->upload_batches) +
           ",\"rejected_batches\":" + std::to_string(impl_->rejected_uploads) +
           ",\"pending\":" +
           (impl_->upload_fence != VK_NULL_HANDLE || impl_->upload_wait_pending
                ? "true" : "false") +
           ",\"dedicated_transfer_queue\":" +
           (impl_->transfer_family != impl_->graphics_family ? "true" : "false") +
           '}';
}

void VulkanWindow::set_global_illumination(const bool enabled) {
    if (!impl_ || impl_->global_illumination_requested == enabled) return;
    impl_->global_illumination_requested = enabled;
    // A new request gets a new attempt; failures stay reported until then.
    impl_->global_illumination_failed = false;
    impl_->global_illumination_error.clear();
}

void VulkanWindow::set_vsync(const bool enabled) {
    if (!impl_ || impl_->vsync_requested == enabled) return;
    impl_->vsync_requested = enabled;
    // The swapchain is rebuilt with the new presentation mode after the next present.
    impl_->resized = true;
}

void VulkanWindow::set_reflections(const bool enabled) {
    if (!impl_ || impl_->reflections_requested == enabled) return;
    impl_->reflections_requested = enabled;
    impl_->reflections_failed = false;
    impl_->reflections_error.clear();
}

std::string VulkanWindow::lighting_status_json() const {
    if (!impl_) return "{\"global_illumination\":{\"requested\":false}}";
    std::string json = "{\"global_illumination\":{\"requested\":";
    json += impl_->global_illumination_requested ? "true" : "false";
    json += ",\"device_supported\":";
    json += impl_->lighting_features ? "true" : "false";
    json += ",\"active\":";
    json += impl_->global_illumination_active ? "true" : "false";
#ifdef RELAY_HAS_FIDELITYFX
    json += ",\"built\":true";
    if (impl_->global_illumination) {
        const auto status = impl_->global_illumination->status();
        json += ",\"static_instances\":" + std::to_string(status.static_instances) +
                ",\"dynamic_instances\":" + std::to_string(status.dynamic_instances) +
                ",\"scratch_bytes\":" + std::to_string(status.scratch_bytes);
    }
#else
    json += ",\"built\":false";
#endif
    const auto escape = [](const std::string& text) {
        std::string escaped;
        for (const char character : text) {
            if (character == '"' || character == '\\') escaped += '\\';
            escaped += character;
        }
        return escaped;
    };
    json += ",\"error\":\"" + escape(impl_->global_illumination_error) + "\"}";
    json += ",\"reflections\":{\"requested\":";
    json += impl_->reflections_requested ? "true" : "false";
    json += ",\"device_supported\":";
    json += impl_->ray_query_features ? "true" : "false";
    json += ",\"active\":";
    json += impl_->reflections_active ? "true" : "false";
#ifdef RELAY_HAS_FIDELITYFX
    if (impl_->acceleration_structures)
        json += ",\"mesh_structures\":" +
                std::to_string(impl_->acceleration_structures->static_structures());
#endif
    json += ",\"error\":\"" + escape(impl_->reflections_error) + "\"}";
    const auto mode = impl_->present_mode_in_use;
    json += ",\"presentation\":{\"vsync_requested\":";
    json += impl_->vsync_requested ? "true" : "false";
    json += ",\"vsync\":";
    json += mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "true" : "false";
    json += ",\"mode\":\"";
    json += mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate"
            : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                  : "fifo";
    json += "\"}";
    return json + '}';
}

void VulkanWindow::resize(const std::uint32_t width, const std::uint32_t height) {
    if (!impl_ || impl_->window == nullptr) return;
    SDL_SetWindowSize(impl_->window, static_cast<int>(width), static_cast<int>(height));
    impl_->resized = true;
}

void VulkanWindow::set_overlay(EditorOverlay* const overlay) {
    if (!impl_) return;
    if (impl_->overlay != nullptr && impl_->overlay_ready) {
        if (impl_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(impl_->device);
        impl_->overlay->invalidate();
    }
    impl_->overlay = overlay;
    impl_->overlay_ready = false;
    impl_->overlay_failed = false;
}

} // namespace relay
