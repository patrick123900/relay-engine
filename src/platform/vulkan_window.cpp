#include "relay/platform/vulkan_window.hpp"
#include "relay/observe/capture.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/shader_reflection.hpp"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace relay {
namespace {

constexpr std::size_t frames_in_flight = 2;

std::string vk_error(const std::string& operation, const VkResult result) {
    return operation + " failed with Vulkan result " + std::to_string(result);
}

bool extension_available(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::string_view(extension.extensionName) == name;
    });
}

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
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t graphics_family{};
    std::uint32_t present_family{};
    VkQueue graphics_queue{};
    VkQueue present_queue{};
    VkCommandPool command_pool{};
    VkBuffer mesh_vertex_buffer{};
    VkDeviceMemory mesh_vertex_memory{};
    VkBuffer mesh_index_buffer{};
    VkDeviceMemory mesh_index_memory{};
    struct GpuTexture {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        std::uint32_t mip_levels{};
    };
    std::vector<GpuTexture> textures;
    VkSampler texture_sampler{};
    VkDescriptorSetLayout texture_layout{};
    VkDescriptorPool texture_pool{};
    VkDescriptorSet texture_set{};
    VkSwapchainKHR swapchain{};
    VkFormat swapchain_format{VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent{};
    bool transfer_source_supported{false};
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> image_views;
    VkRenderPass render_pass{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{};
    // One depth attachment shared by every swapchain image. Only one frame records into the render
    // pass at a time, so the depth buffer does not need to be per-image the way colour does.
    VkFormat depth_format{VK_FORMAT_UNDEFINED};
    VkImage depth_image{};
    VkDeviceMemory depth_memory{};
    VkImageView depth_view{};
    std::vector<VkFramebuffer> framebuffers;
    std::array<VkCommandBuffer, frames_in_flight> command_buffers{};
    std::array<VkSemaphore, frames_in_flight> image_available{};
    std::array<VkSemaphore, frames_in_flight> render_finished{};
    std::array<VkFence, frames_in_flight> frame_fences{};
    VkQueryPool timestamp_queries{};
    std::array<bool, frames_in_flight> timestamp_submitted{};
    float timestamp_period_nanoseconds{};
    double latest_gpu_milliseconds{};
    std::uint32_t latest_draw_calls{};
    std::size_t current_frame{};
    bool resized{false};
    std::string selected_device_name;
    std::string last_error;
    std::vector<std::string> pending_input_events;
    CompiledRenderGraph render_graph;
    ShaderInterface vertex_interface;
    ShaderInterface fragment_interface;
    std::uint64_t uploaded_asset_revision{};

    ~Impl() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        cleanup_swapchain();
        if (device != VK_NULL_HANDLE) {
            for (std::size_t index = 0; index < frames_in_flight; ++index) {
                vkDestroyFence(device, frame_fences[index], nullptr);
                vkDestroySemaphore(device, render_finished[index], nullptr);
                vkDestroySemaphore(device, image_available[index], nullptr);
            }
            vkDestroyBuffer(device, mesh_index_buffer, nullptr);
            vkFreeMemory(device, mesh_index_memory, nullptr);
            vkDestroyBuffer(device, mesh_vertex_buffer, nullptr);
            vkFreeMemory(device, mesh_vertex_memory, nullptr);
            vkDestroyDescriptorPool(device, texture_pool, nullptr);
            vkDestroySampler(device, texture_sampler, nullptr);
            for (const auto& texture : textures) {
                vkDestroyImageView(device, texture.view, nullptr);
                vkDestroyImage(device, texture.image, nullptr);
                vkFreeMemory(device, texture.memory, nullptr);
            }
            vkDestroyDescriptorSetLayout(device, texture_layout, nullptr);
            vkDestroyCommandPool(device, command_pool, nullptr);
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

    bool initialize(const std::string& title, const std::uint32_t width, const std::uint32_t height) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            last_error = SDL_GetError();
            return false;
        }
        sdl_initialized = true;
        window = SDL_CreateWindow(title.c_str(), static_cast<int>(width), static_cast<int>(height),
                                  SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        if (window == nullptr) {
            last_error = SDL_GetError();
            return false;
        }
        render_graph = make_scene_render_graph();
        if (!render_graph.valid) {
            last_error = "render graph compilation failed: " + render_graph.error;
            return false;
        }
        return create_instance() && create_surface() && pick_physical_device() &&
               create_logical_device() && create_timestamp_pool() && create_command_pool() &&
               create_mesh_buffers() && create_texture_resources() && create_swapchain_resources() &&
               create_sync_objects();
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
        application.apiVersion = VK_API_VERSION_1_0;

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
                selected_device_name = properties.deviceName;
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
        const std::set<std::uint32_t> unique_families{graphics_family, present_family};
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
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
        create_info.pEnabledFeatures = &enabled_features;
        const auto result = vkCreateDevice(physical_device, &create_info, nullptr, &device);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkCreateDevice", result);
            return false;
        }
        vkGetDeviceQueue(device, graphics_family, 0, &graphics_queue);
        vkGetDeviceQueue(device, present_family, 0, &present_queue);
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

    bool upload_buffer(const void* data, const VkDeviceSize size,
                       const VkBufferUsageFlags final_usage, VkBuffer& destination,
                       VkDeviceMemory& destination_memory) {
        VkBuffer staging{};
        VkDeviceMemory staging_memory{};
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
            !create_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | final_usage,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, destination, destination_memory)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        }
        VkCommandBuffer commands{};
        if (result == VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocation.commandPool = command_pool;
            allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocation.commandBufferCount = 1U;
            result = vkAllocateCommandBuffers(device, &allocation, &commands);
        }
        if (result == VK_SUCCESS) {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(commands, &begin);
            if (result == VK_SUCCESS) {
                VkBufferCopy copy{};
                copy.size = size;
                vkCmdCopyBuffer(commands, staging, destination, 1U, &copy);
                result = vkEndCommandBuffer(commands);
            }
        }
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &commands;
            result = vkQueueSubmit(graphics_queue, 1U, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) result = vkQueueWaitIdle(graphics_queue);
        }
        if (commands != VK_NULL_HANDLE) vkFreeCommandBuffers(device, command_pool, 1U, &commands);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, staging_memory, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error("mesh asset upload", result);
            return false;
        }
        return true;
    }

    bool create_mesh_buffers() {
        const auto vertices = assets->mesh_vertices();
        const auto indices = assets->mesh_indices();
        const bool uploaded = upload_buffer(vertices.data(), vertices.size_bytes(),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                            mesh_vertex_buffer, mesh_vertex_memory) &&
                              upload_buffer(indices.data(), indices.size_bytes(),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                            mesh_index_buffer, mesh_index_memory);
        if (uploaded) uploaded_asset_revision = assets->revision();
        return uploaded;
    }

    bool refresh_mesh_assets() {
        if (uploaded_asset_revision == assets->revision()) return true;
        auto result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS) {
            last_error = vk_error("waiting to refresh imported meshes", result);
            return false;
        }
        vkDestroyBuffer(device, mesh_index_buffer, nullptr);
        vkFreeMemory(device, mesh_index_memory, nullptr);
        vkDestroyBuffer(device, mesh_vertex_buffer, nullptr);
        vkFreeMemory(device, mesh_vertex_memory, nullptr);
        mesh_index_buffer = VK_NULL_HANDLE;
        mesh_index_memory = VK_NULL_HANDLE;
        mesh_vertex_buffer = VK_NULL_HANDLE;
        mesh_vertex_memory = VK_NULL_HANDLE;
        return create_mesh_buffers();
    }

    bool create_texture_image(const TextureAsset& asset, GpuTexture& texture) {
        texture.mip_levels = static_cast<std::uint32_t>(
                                 std::floor(std::log2(static_cast<double>(
                                     std::max(asset.width, asset.height))))) + 1U;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {asset.width, asset.height, 1U};
        image_info.mipLevels = texture.mip_levels;
        image_info.arrayLayers = 1U;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
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
        if (!create_buffer(byte_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging, staging_memory)) return false;
        void* mapped = nullptr;
        result = vkMapMemory(device, staging_memory, 0U, byte_count, 0U, &mapped);
        if (result == VK_SUCCESS) {
            std::memcpy(mapped, asset.rgba.data(), asset.rgba.size());
            vkUnmapMemory(device, staging_memory);
        }
        VkCommandBuffer commands{};
        if (result == VK_SUCCESS) {
            VkCommandBufferAllocateInfo command_allocation{};
            command_allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            command_allocation.commandPool = command_pool;
            command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            command_allocation.commandBufferCount = 1U;
            result = vkAllocateCommandBuffers(device, &command_allocation, &commands);
        }
        if (result == VK_SUCCESS) {
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            result = vkBeginCommandBuffer(commands, &begin);
        }
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
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, nullptr, 0U, nullptr,
                                 1U, &initial);
            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1U;
            copy.imageExtent = {asset.width, asset.height, 1U};
            vkCmdCopyBufferToImage(commands, staging, texture.image,
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
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
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
                vkCmdBlitImage(commands, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &blit,
                               VK_FILTER_LINEAR);
                source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                source.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                source.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr,
                                     0U, nullptr, 1U, &source);
                mip_width = std::max(mip_width / 2, 1);
                mip_height = std::max(mip_height / 2, 1);
            }
            VkImageMemoryBarrier last{};
            last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            last.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            last.image = texture.image;
            last.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            last.subresourceRange.baseMipLevel = texture.mip_levels - 1U;
            last.subresourceRange.levelCount = 1U;
            last.subresourceRange.layerCount = 1U;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, nullptr, 0U,
                                 nullptr, 1U, &last);
            result = vkEndCommandBuffer(commands);
        }
        if (result == VK_SUCCESS) {
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1U;
            submit.pCommandBuffers = &commands;
            result = vkQueueSubmit(graphics_queue, 1U, &submit, VK_NULL_HANDLE);
            if (result == VK_SUCCESS) result = vkQueueWaitIdle(graphics_queue);
        }
        if (commands != VK_NULL_HANDLE) vkFreeCommandBuffers(device, command_pool, 1U, &commands);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, staging_memory, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture upload and mip generation", result);
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = texture.mip_levels;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &texture.view);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture image view creation", result);
            return false;
        }
        return true;
    }

    bool create_texture_resources() {
        VkFormatProperties format_properties{};
        vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R8G8B8A8_UNORM,
                                            &format_properties);
        constexpr VkFormatFeatureFlags required_format_features =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
            VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((format_properties.optimalTilingFeatures & required_format_features) !=
            required_format_features) {
            last_error = "RGBA8 textures do not support linear blit mip generation";
            return false;
        }
        const auto texture_assets = assets->textures();
        if (texture_assets.empty() || texture_assets.size() > bindless_texture_capacity) {
            last_error = "built-in textures exceed the bindless table capacity";
            return false;
        }
        textures.resize(texture_assets.size());
        for (std::size_t index = 0; index < texture_assets.size(); ++index) {
            if (!create_texture_image(texture_assets[index], textures[index])) return false;
        }
        VkSamplerCreateInfo sampler_info{};
        sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_info.magFilter = VK_FILTER_LINEAR;
        sampler_info.minFilter = VK_FILTER_LINEAR;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_info.maxLod = static_cast<float>(textures.front().mip_levels);
        auto result = vkCreateSampler(device, &sampler_info, nullptr, &texture_sampler);
        if (result != VK_SUCCESS) {
            last_error = vk_error("texture sampler creation", result);
            return false;
        }
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0U;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = bindless_texture_capacity;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = 1U;
        layout_info.pBindings = &binding;
        result = vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &texture_layout);
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       bindless_texture_capacity};
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.maxSets = 1U;
        pool_info.poolSizeCount = 1U;
        pool_info.pPoolSizes = &pool_size;
        if (result == VK_SUCCESS) result = vkCreateDescriptorPool(device, &pool_info, nullptr, &texture_pool);
        VkDescriptorSetAllocateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        set_info.descriptorPool = texture_pool;
        set_info.descriptorSetCount = 1U;
        set_info.pSetLayouts = &texture_layout;
        if (result == VK_SUCCESS) result = vkAllocateDescriptorSets(device, &set_info, &texture_set);
        if (result != VK_SUCCESS) {
            last_error = vk_error("bindless texture descriptor allocation", result);
            return false;
        }
        std::array<VkDescriptorImageInfo, bindless_texture_capacity> image_infos{};
        for (std::size_t index = 0; index < image_infos.size(); ++index) {
            const auto& texture = textures[index < textures.size() ? index : 0U];
            image_infos[index].sampler = texture_sampler;
            image_infos[index].imageView = texture.view;
            image_infos[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = texture_set;
        write.dstBinding = 0U;
        write.descriptorCount = bindless_texture_capacity;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = image_infos.data();
        vkUpdateDescriptorSets(device, 1U, &write, 0U, nullptr);
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
        create_info.queryCount = static_cast<std::uint32_t>(frames_in_flight * 2U);
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
        std::uint32_t mode_count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &mode_count, modes.data());

        const auto format = choose_surface_format(formats);
        const auto mailbox = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_MAILBOX_KHR);
        const auto present_mode = mailbox != modes.end() ? VK_PRESENT_MODE_MAILBOX_KHR
                                                         : VK_PRESENT_MODE_FIFO_KHR;
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
                                 (transfer_source_supported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0U);
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
        static constexpr std::array candidates{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                               VK_FORMAT_D24_UNORM_S8_UINT};
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
        auto result = vkCreateImage(device, &image_info, nullptr, &depth_image);
        if (result != VK_SUCCESS) {
            last_error = vk_error("depth image creation", result);
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, depth_image, &requirements);
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
        result = vkAllocateMemory(device, &allocation, nullptr, &depth_memory);
        if (result == VK_SUCCESS) result = vkBindImageMemory(device, depth_image, depth_memory, 0U);
        if (result != VK_SUCCESS) {
            last_error = vk_error("depth image allocation", result);
            return false;
        }
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = depth_image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = depth_format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_info.subresourceRange.levelCount = 1U;
        view_info.subresourceRange.layerCount = 1U;
        result = vkCreateImageView(device, &view_info, nullptr, &depth_view);
        if (result != VK_SUCCESS) {
            last_error = vk_error("depth image view creation", result);
            return false;
        }
        return true;
    }

    bool create_render_pass() {
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
        depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
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
        dependencies[1].dstAccessMask = transfer_source_supported ? VK_ACCESS_TRANSFER_READ_BIT : 0U;
        const std::array attachments{color_attachment, depth_attachment};
        VkRenderPassCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        create_info.pAttachments = attachments.data();
        create_info.subpassCount = 1;
        create_info.pSubpasses = &subpass;
        create_info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        create_info.pDependencies = dependencies.data();
        const auto result = vkCreateRenderPass(device, &create_info, nullptr, &render_pass);
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
        std::array<VkVertexInputAttributeDescription, 2> attributes{};
        attributes[0].location = 0U;
        attributes[0].binding = 0U;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = 0U;
        attributes[1].location = 1U;
        attributes[1].binding = 0U;
        attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[1].offset = static_cast<std::uint32_t>(offsetof(MeshVertex, u));
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
        pipeline_info.renderPass = render_pass;
        if (result == VK_SUCCESS) {
            result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        }
        vkDestroyShaderModule(device, fragment_module, nullptr);
        vkDestroyShaderModule(device, vertex_module, nullptr);
        if (result != VK_SUCCESS) {
            last_error = vk_error("graphics pipeline creation", result);
            return false;
        }
        return true;
    }

    bool create_framebuffers() {
        framebuffers.resize(image_views.size());
        for (std::size_t index = 0; index < image_views.size(); ++index) {
            const std::array attachments{image_views[index], depth_view};
            VkFramebufferCreateInfo create_info{};
            create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            create_info.renderPass = render_pass;
            create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
            create_info.pAttachments = attachments.data();
            create_info.width = swapchain_extent.width;
            create_info.height = swapchain_extent.height;
            create_info.layers = 1;
            const auto result = vkCreateFramebuffer(device, &create_info, nullptr, &framebuffers[index]);
            if (result != VK_SUCCESS) {
                last_error = vk_error("vkCreateFramebuffer", result);
                return false;
            }
        }
        return true;
    }

    bool create_swapchain_resources() {
        return create_swapchain() && swapchain != VK_NULL_HANDLE && create_image_views() &&
               create_depth_resources() && create_render_pass() && create_pipeline() &&
               create_framebuffers();
    }

    bool create_sync_objects() {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (std::size_t index = 0; index < frames_in_flight; ++index) {
            if (vkCreateSemaphore(device, &semaphore_info, nullptr, &image_available[index]) != VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphore_info, nullptr, &render_finished[index]) != VK_SUCCESS ||
                vkCreateFence(device, &fence_info, nullptr, &frame_fences[index]) != VK_SUCCESS) {
                last_error = "could not create Vulkan frame synchronization objects";
                return false;
            }
        }
        return true;
    }

    void cleanup_swapchain() {
        if (device == VK_NULL_HANDLE) return;
        for (const auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        framebuffers.clear();
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
        vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
        vkDestroyImageView(device, depth_view, nullptr);
        depth_view = VK_NULL_HANDLE;
        vkDestroyImage(device, depth_image, nullptr);
        depth_image = VK_NULL_HANDLE;
        vkFreeMemory(device, depth_memory, nullptr);
        depth_memory = VK_NULL_HANDLE;
        for (const auto view : image_views) vkDestroyImageView(device, view, nullptr);
        image_views.clear();
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

    struct DrawPushConstants {
        std::array<float, 16> model_view_projection;
        std::array<float, 4> color;
        std::uint32_t texture_index{};
    };

    bool record_commands(const VkCommandBuffer commands, const std::uint32_t image_index,
                         const float elapsed_seconds, const Scene* scene,
                         const VkBuffer capture_buffer) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        auto result = vkBeginCommandBuffer(commands, &begin_info);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkBeginCommandBuffer", result);
            return false;
        }
        if (timestamp_queries != VK_NULL_HANDLE) {
            const auto first_query = static_cast<std::uint32_t>(current_frame * 2U);
            vkCmdResetQueryPool(commands, timestamp_queries, first_query, 2U);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_queries, first_query);
        }
        std::array<VkClearValue, 2> clear{};
        clear[0].color = {{0.012F, 0.018F, 0.045F, 1.0F}};
        // Reversed-Z is not in use, so the far plane clears to 1.0 and LESS keeps the nearest write.
        clear[1].depthStencil = {1.0F, 0U};
        VkRenderPassBeginInfo render_info{};
        render_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_info.renderPass = render_pass;
        render_info.framebuffer = framebuffers[image_index];
        render_info.renderArea.extent = swapchain_extent;
        render_info.clearValueCount = static_cast<std::uint32_t>(clear.size());
        render_info.pClearValues = clear.data();
        vkCmdBeginRenderPass(commands, &render_info, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport viewport{};
        viewport.width = static_cast<float>(swapchain_extent.width);
        viewport.height = static_cast<float>(swapchain_extent.height);
        viewport.maxDepth = 1.0F;
        VkRect2D scissor{};
        scissor.extent = swapchain_extent;
        vkCmdSetViewport(commands, 0, 1, &viewport);
        vkCmdSetScissor(commands, 0, 1, &scissor);
        const VkDeviceSize vertex_offset = 0U;
        vkCmdBindVertexBuffers(commands, 0U, 1U, &mesh_vertex_buffer, &vertex_offset);
        vkCmdBindIndexBuffer(commands, mesh_index_buffer, 0U, VK_INDEX_TYPE_UINT32);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0U,
                                1U, &texture_set, 0U, nullptr);
        latest_draw_calls = 0U;
        if (scene != nullptr && !scene->entities().empty()) {
            const float aspect = static_cast<float>(swapchain_extent.width) /
                                 static_cast<float>(std::max(swapchain_extent.height, 1U));
            const auto render_scene = build_render_scene(*scene, *assets, aspect);
            for (const auto& instance : render_scene.instances) {
                const auto* mesh = assets->find_mesh(instance.mesh);
                if (mesh == nullptr) continue;
                const DrawPushConstants constants{instance.model_view_projection.values,
                                                  instance.color, instance.texture_index};
                vkCmdPushConstants(commands, pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(constants), &constants);
                vkCmdDrawIndexed(commands, mesh->index_count, 1U, mesh->first_index,
                                 mesh->vertex_offset, 0U);
                ++latest_draw_calls;
            }
        } else {
            const float angle = elapsed_seconds * 0.35F;
            const float cosine = std::cos(angle);
            const float sine = std::sin(angle);
            DrawPushConstants constants{};
            constants.model_view_projection = {cosine, sine, 0.0F, 0.0F,
                                                -sine, cosine, 0.0F, 0.0F,
                                                0.0F, 0.0F, 1.0F, 0.0F,
                                                0.0F, 0.0F, 0.0F, 1.0F};
            constants.color = {0.98F, 0.45F, 0.16F, 1.0F};
            constants.texture_index = 0U;
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
        if (timestamp_queries != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_queries,
                                static_cast<std::uint32_t>(current_frame * 2U + 1U));
        }
        result = vkEndCommandBuffer(commands);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkEndCommandBuffer", result);
            return false;
        }
        return true;
    }

    bool draw(const double elapsed, const Scene* scene = nullptr,
              const VkBuffer capture_buffer = VK_NULL_HANDLE) {
        if (!refresh_mesh_assets()) return false;
        if (swapchain == VK_NULL_HANDLE) return recreate_swapchain();
        auto result = vkWaitForFences(device, 1, &frame_fences[current_frame], VK_TRUE,
                                      std::numeric_limits<std::uint64_t>::max());
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkWaitForFences", result);
            return false;
        }
        if (timestamp_queries != VK_NULL_HANDLE && timestamp_submitted[current_frame]) {
            std::array<std::uint64_t, 2> timestamps{};
            const auto query_result = vkGetQueryPoolResults(
                device, timestamp_queries, static_cast<std::uint32_t>(current_frame * 2U), 2U,
                sizeof(timestamps), timestamps.data(), sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
            if (query_result == VK_SUCCESS && timestamps[1] >= timestamps[0]) {
                latest_gpu_milliseconds = static_cast<double>(timestamps[1] - timestamps[0]) *
                                          timestamp_period_nanoseconds / 1'000'000.0;
            }
        }
        std::uint32_t image_index = 0;
        result = vkAcquireNextImageKHR(device, swapchain,
                                       std::numeric_limits<std::uint64_t>::max(),
                                       image_available[current_frame], VK_NULL_HANDLE, &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return recreate_swapchain();
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            last_error = vk_error("vkAcquireNextImageKHR", result);
            return false;
        }
        vkResetFences(device, 1, &frame_fences[current_frame]);
        vkResetCommandBuffer(command_buffers[current_frame], 0);
        if (!record_commands(command_buffers[current_frame], image_index, static_cast<float>(elapsed),
                             scene, capture_buffer)) {
            return false;
        }
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &image_available[current_frame];
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &command_buffers[current_frame];
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &render_finished[current_frame];
        result = vkQueueSubmit(graphics_queue, 1, &submit_info, frame_fences[current_frame]);
        if (result != VK_SUCCESS) {
            last_error = vk_error("vkQueueSubmit", result);
            return false;
        }
        timestamp_submitted[current_frame] = timestamp_queries != VK_NULL_HANDLE;
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &render_finished[current_frame];
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        result = vkQueuePresentKHR(present_queue, &present_info);
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

    bool write_capture(const std::filesystem::path& path, const void* mapped) {
        const bool bgra = swapchain_format == VK_FORMAT_B8G8R8A8_SRGB ||
                          swapchain_format == VK_FORMAT_B8G8R8A8_UNORM;
        const bool rgba = swapchain_format == VK_FORMAT_R8G8B8A8_SRGB ||
                          swapchain_format == VK_FORMAT_R8G8B8A8_UNORM;
        if (!bgra && !rgba) {
            last_error = "frame capture currently requires an 8-bit BGRA or RGBA swapchain";
            return false;
        }
        OwnedFrame frame{swapchain_extent.width, swapchain_extent.height,
                         std::vector<std::uint8_t>(static_cast<std::size_t>(swapchain_extent.width) *
                                                  swapchain_extent.height * 4U)};
        const auto* pixels = static_cast<const std::uint8_t*>(mapped);
        for (std::uint32_t y = 0; y < swapchain_extent.height; ++y) {
            for (std::uint32_t x = 0; x < swapchain_extent.width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * swapchain_extent.width + x) * 4U;
                frame.rgba[offset] = bgra ? pixels[offset + 2U] : pixels[offset];
                frame.rgba[offset + 1U] = pixels[offset + 1U];
                frame.rgba[offset + 2U] = bgra ? pixels[offset] : pixels[offset + 2U];
                frame.rgba[offset + 3U] = pixels[offset + 3U];
            }
        }
        return write_frame_image(frame.view(), path, last_error);
    }

    bool capture_image(const std::filesystem::path& path, const double elapsed, const Scene* scene) {
        if (!transfer_source_supported) {
            last_error = "the window surface does not support swapchain transfer-source images";
            return false;
        }
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        if (!create_capture_buffer(buffer, memory)) return false;
        bool captured = draw(elapsed, scene, buffer);
        if (captured) {
            const auto wait_result = vkDeviceWaitIdle(device);
            if (wait_result != VK_SUCCESS) {
                last_error = vk_error("capture queue wait", wait_result);
                captured = false;
            }
        }
        void* mapped = nullptr;
        if (captured) {
            const auto byte_count = static_cast<VkDeviceSize>(swapchain_extent.width) *
                                    swapchain_extent.height * 4U;
            const auto map_result = vkMapMemory(device, memory, 0, byte_count, 0, &mapped);
            if (map_result != VK_SUCCESS) {
                last_error = vk_error("capture memory mapping", map_result);
                captured = false;
            }
        }
        if (captured) captured = write_capture(path, mapped);
        if (mapped != nullptr) vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, buffer, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return captured;
    }
};

VulkanWindow::VulkanWindow(std::string title, const std::uint32_t width,
                           const std::uint32_t height, const AssetRegistry& assets)
    : impl_(std::make_unique<Impl>()) {
    impl_->assets = &assets;
    impl_->initialize(title, width, height);
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
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            impl_->pending_input_events.push_back(
                std::string{"key:"} + (event.type == SDL_EVENT_KEY_DOWN ? "down:" : "up:") +
                std::to_string(static_cast<std::int64_t>(event.key.key)));
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            impl_->pending_input_events.push_back("mouse_motion:" + std::to_string(event.motion.x) + ':' +
                                                  std::to_string(event.motion.y));
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            impl_->pending_input_events.push_back(
                std::string{"mouse_button:"} +
                (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down:" : "up:") +
                std::to_string(event.button.button));
        } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
            impl_->pending_input_events.push_back("mouse_wheel:" + std::to_string(event.wheel.x) + ':' +
                                                  std::to_string(event.wheel.y));
        } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            impl_->pending_input_events.push_back("gamepad_axis:" + std::to_string(event.gaxis.which) + ':' +
                                                  std::to_string(event.gaxis.axis) + ':' +
                                                  std::to_string(event.gaxis.value));
        } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                   event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            impl_->pending_input_events.push_back(
                std::string{"gamepad_button:"} +
                (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down:" : "up:") +
                std::to_string(event.gbutton.which) + ':' + std::to_string(event.gbutton.button));
        }
        if (event.type == SDL_EVENT_QUIT) return true;
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) return true;
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

std::uint32_t VulkanWindow::draw_call_count() const {
    return impl_ ? impl_->latest_draw_calls : 0U;
}

std::uint32_t VulkanWindow::render_resource_count() const {
    if (!impl_) return 0U;
    // Depth contributes an image, its memory and its view.
    const std::size_t depth_resources = impl_->depth_image == VK_NULL_HANDLE ? 0U : 3U;
    return static_cast<std::uint32_t>(impl_->swapchain_images.size() + impl_->image_views.size() +
                                      impl_->framebuffers.size() + impl_->textures.size() * 2U +
                                      depth_resources + 8U);
}

std::string VulkanWindow::render_graph_json() const {
    return impl_ ? impl_->render_graph.json() : "{\"valid\":false}";
}

std::string VulkanWindow::shader_interfaces_json() const {
    if (!impl_) return "{\"vertex\":null,\"fragment\":null}";
    return "{\"vertex\":" + impl_->vertex_interface.json() +
           ",\"fragment\":" + impl_->fragment_interface.json() + '}';
}

void VulkanWindow::resize(const std::uint32_t width, const std::uint32_t height) {
    if (!impl_ || impl_->window == nullptr) return;
    SDL_SetWindowSize(impl_->window, static_cast<int>(width), static_cast<int>(height));
    impl_->resized = true;
}

} // namespace relay
