#include "relay/render/vulkan_device.hpp"

#ifdef RELAY_HAS_VULKAN
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>
#endif

namespace relay {

#ifdef RELAY_HAS_VULKAN
namespace {

std::string device_type_name(const VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
    default: return "other";
    }
}

int device_score(const VkPhysicalDevice device) {
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    switch (properties.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 400;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 300;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 200;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 100;
    default: return 0;
    }
}

bool has_extension(const std::set<std::string>& extensions, const char* name) {
    return extensions.contains(name);
}

} // namespace
#endif

VulkanCapabilities probe_vulkan_capabilities() {
    VulkanCapabilities capabilities;
#ifndef RELAY_HAS_VULKAN
    capabilities.error = "Relay was built without a Vulkan SDK or loader";
    return capabilities;
#else
    std::uint32_t loader_version = VK_API_VERSION_1_0;
    const auto enumerate_instance_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerate_instance_version != nullptr) {
        if (enumerate_instance_version(&loader_version) != VK_SUCCESS) {
            capabilities.error = "Vulkan loader version query failed";
            return capabilities;
        }
    }
    capabilities.loader_available = true;
    capabilities.api_major = VK_API_VERSION_MAJOR(loader_version);
    capabilities.api_minor = VK_API_VERSION_MINOR(loader_version);
    capabilities.api_patch = VK_API_VERSION_PATCH(loader_version);

    VkApplicationInfo application_info{};
    application_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application_info.pApplicationName = "Relay Engine";
    application_info.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.pEngineName = "Relay";
    application_info.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application_info.apiVersion = std::min(loader_version, VK_API_VERSION_1_3);

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &application_info;
    VkInstance instance{};
    const auto instance_result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (instance_result != VK_SUCCESS) {
        capabilities.error = "vkCreateInstance failed with code " + std::to_string(instance_result);
        return capabilities;
    }

    std::uint32_t device_count = 0;
    auto result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    if (result != VK_SUCCESS || device_count == 0) {
        capabilities.error = "no Vulkan-capable physical device was found";
        vkDestroyInstance(instance, nullptr);
        return capabilities;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
    if (result != VK_SUCCESS) {
        capabilities.error = "physical device enumeration failed";
        vkDestroyInstance(instance, nullptr);
        return capabilities;
    }
    const auto physical_device = *std::max_element(devices.begin(), devices.end(),
        [](const VkPhysicalDevice left, const VkPhysicalDevice right) {
            return device_score(left) < device_score(right);
        });
    capabilities.device_available = true;

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical_device, &properties);
    capabilities.device_name = properties.deviceName;
    capabilities.device_type = device_type_name(properties.deviceType);

    std::uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extension_properties(extension_count);
    vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &extension_count, extension_properties.data());
    std::set<std::string> extensions;
    for (const auto& extension : extension_properties) extensions.emplace(extension.extensionName);
    capabilities.ray_tracing_pipeline =
        has_extension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) &&
        has_extension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    capabilities.ray_query = has_extension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    capabilities.mesh_shader = has_extension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    capabilities.descriptor_buffer = has_extension(extensions, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME);

    std::uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(
        physical_device, &queue_family_count, queue_families.data());
    std::uint32_t graphics_family = queue_family_count;
    for (std::uint32_t index = 0; index < queue_family_count; ++index) {
        if ((queue_families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
            graphics_family = index;
            break;
        }
    }
    if (graphics_family == queue_family_count) {
        capabilities.error = "selected Vulkan device has no graphics queue";
        vkDestroyInstance(instance, nullptr);
        return capabilities;
    }

    constexpr float queue_priority = 1.0F;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = graphics_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &queue_priority;
    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkDevice logical_device{};
    result = vkCreateDevice(physical_device, &device_info, nullptr, &logical_device);
    if (result == VK_SUCCESS) {
        capabilities.logical_device_created = true;
        vkDestroyDevice(logical_device, nullptr);
    } else {
        capabilities.error = "vkCreateDevice failed with code " + std::to_string(result);
    }
    vkDestroyInstance(instance, nullptr);
    return capabilities;
#endif
}

} // namespace relay
