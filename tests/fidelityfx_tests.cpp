// Creates every FidelityFX effect Relay uses on a headless Vulkan device. Context creation builds
// all of an effect's compute pipelines from the precompiled SPIR-V permutations and checks their
// reflection data against the backend, so this covers the Linux shader toolchain end to end.
// No window or surface is created. Without a suitable GPU the test reports a skip and passes.

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_brixelizer.h>
#include <FidelityFX/host/ffx_brixelizergi.h>
#include <FidelityFX/host/ffx_classifier.h>
#include <FidelityFX/host/ffx_denoiser.h>
#include <FidelityFX/host/ffx_spd.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

struct HeadlessDevice {
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::string skip_reason;

    HeadlessDevice() {
        VkApplicationInfo application{};
        application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        application.pApplicationName = "relay_fidelityfx_tests";
        application.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &application;
        if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
            skip_reason = "no Vulkan 1.3 instance";
            return;
        }
        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (properties.apiVersion >= VK_API_VERSION_1_3 &&
                properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
                physical_device = candidate;
                if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
            }
        }
        if (!physical_device) {
            skip_reason = "no Vulkan 1.3 GPU";
            return;
        }

        VkPhysicalDeviceVulkan13Features features13{};
        features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan12Features features12{};
        features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan11Features features11{};
        features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceFeatures2 features{};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &features11;
        features11.pNext = &features12;
        features12.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(physical_device, &features);
        if (!features12.shaderFloat16 || !features11.storageBuffer16BitAccess ||
            !features13.subgroupSizeControl || !features13.computeFullSubgroups ||
            !features12.shaderSubgroupExtendedTypes || !features.features.shaderInt64 ||
            !features12.descriptorBindingPartiallyBound) {
            skip_reason = "the GPU lacks features the FidelityFX shaders use";
            return;
        }

        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &family_count, families.data());
        std::uint32_t family = family_count;
        for (std::uint32_t index = 0; index < family_count; ++index)
            if ((families[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) { family = index; break; }
        if (family == family_count) {
            skip_reason = "no compute queue";
            return;
        }
        const float priority = 1.0F;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;

        // Enable exactly what Relay's renderer enables for FidelityFX.
        VkPhysicalDeviceVulkan13Features enable13{};
        enable13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        enable13.subgroupSizeControl = VK_TRUE;
        enable13.computeFullSubgroups = VK_TRUE;
        enable13.synchronization2 = features13.synchronization2;
        VkPhysicalDeviceVulkan12Features enable12{};
        enable12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        enable12.pNext = &enable13;
        enable12.shaderFloat16 = VK_TRUE;
        enable12.shaderSubgroupExtendedTypes = VK_TRUE;
        enable12.descriptorBindingPartiallyBound = VK_TRUE;
        enable12.shaderBufferInt64Atomics = features12.shaderBufferInt64Atomics;
        enable12.shaderStorageBufferArrayNonUniformIndexing =
            features12.shaderStorageBufferArrayNonUniformIndexing;
        enable12.bufferDeviceAddress = features12.bufferDeviceAddress;
        VkPhysicalDeviceVulkan11Features enable11{};
        enable11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        enable11.pNext = &enable12;
        enable11.storageBuffer16BitAccess = VK_TRUE;
        VkPhysicalDeviceFeatures2 enable{};
        enable.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        enable.pNext = &enable11;
        enable.features.shaderInt64 = VK_TRUE;
        enable.features.shaderInt16 = features.features.shaderInt16;

        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.pNext = &enable;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        if (vkCreateDevice(physical_device, &device_info, nullptr, &device) != VK_SUCCESS) {
            skip_reason = "vkCreateDevice failed";
            device = VK_NULL_HANDLE;
        }
    }

    ~HeadlessDevice() {
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }
};

struct Backend {
    std::vector<std::byte> scratch;
    FfxInterface interface{};
    bool ready{};

    Backend(HeadlessDevice& gpu, const std::size_t contexts) {
        const auto size = ffxGetScratchMemorySizeVK(gpu.physical_device, contexts);
        scratch.resize(size);
        VkDeviceContext device_context{gpu.device, gpu.physical_device, vkGetDeviceProcAddr};
        ready = ffxGetInterfaceVK(&interface, ffxGetDeviceVK(&device_context), scratch.data(),
                                  scratch.size(), contexts) == FFX_OK;
    }
};

void test_brixelizer_and_gi(HeadlessDevice& gpu) {
    Backend backend(gpu, 2);
    expect(backend.ready, "the Vulkan backend for Brixelizer initialises");
    if (!backend.ready) return;

    auto brixelizer = std::make_unique<FfxBrixelizerContext>();
    FfxBrixelizerContextDescription description{};
    description.numCascades = 4;
    description.backendInterface = backend.interface;
    float voxel_size = 0.2F;
    for (std::uint32_t index = 0; index < description.numCascades; ++index) {
        description.cascadeDescs[index].flags =
            static_cast<FfxBrixelizerCascadeFlag>(FFX_BRIXELIZER_CASCADE_STATIC |
                                                  FFX_BRIXELIZER_CASCADE_DYNAMIC);
        description.cascadeDescs[index].voxelSize = voxel_size;
        voxel_size *= 2.0F;
    }
    const auto created = ffxBrixelizerContextCreate(&description, brixelizer.get());
    expect(created == FFX_OK, "ffxBrixelizerContextCreate returns FFX_OK (got " +
                                  std::to_string(created) + ")");
    if (created != FFX_OK) return;

    auto gi = std::make_unique<FfxBrixelizerGIContext>();
    FfxBrixelizerGIContextDescription gi_description{};
    gi_description.flags = FFX_BRIXELIZER_GI_FLAG_DEPTH_INVERTED;
    gi_description.internalResolution = FFX_BRIXELIZER_GI_INTERNAL_RESOLUTION_50_PERCENT;
    gi_description.displaySize = {1280U, 720U};
    gi_description.backendInterface = backend.interface;
    const auto gi_created = ffxBrixelizerGIContextCreate(gi.get(), &gi_description);
    expect(gi_created == FFX_OK, "ffxBrixelizerGIContextCreate returns FFX_OK (got " +
                                     std::to_string(gi_created) + ")");
    if (gi_created == FFX_OK)
        expect(ffxBrixelizerGIContextDestroy(gi.get()) == FFX_OK, "Brixelizer GI is destroyed");
    expect(ffxBrixelizerContextDestroy(brixelizer.get()) == FFX_OK, "Brixelizer is destroyed");
}

void test_reflection_effects(HeadlessDevice& gpu) {
    Backend backend(gpu, 3);
    expect(backend.ready, "the Vulkan backend for reflections initialises");
    if (!backend.ready) return;

    auto classifier = std::make_unique<FfxClassifierContext>();
    FfxClassifierContextDescription classifier_description{};
    classifier_description.flags = FFX_CLASSIFIER_REFLECTION | FFX_CLASSIFIER_ENABLE_DEPTH_INVERTED;
    classifier_description.resolution = {1280U, 720U};
    classifier_description.backendInterface = backend.interface;
    const auto classifier_created =
        ffxClassifierContextCreate(classifier.get(), &classifier_description);
    expect(classifier_created == FFX_OK, "ffxClassifierContextCreate returns FFX_OK (got " +
                                             std::to_string(classifier_created) + ")");

    auto denoiser = std::make_unique<FfxDenoiserContext>();
    FfxDenoiserContextDescription denoiser_description{};
    denoiser_description.flags = FFX_DENOISER_REFLECTIONS | FFX_DENOISER_ENABLE_DEPTH_INVERTED;
    denoiser_description.windowSize = {1280U, 720U};
    denoiser_description.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    denoiser_description.backendInterface = backend.interface;
    const auto denoiser_created = ffxDenoiserContextCreate(denoiser.get(), &denoiser_description);
    expect(denoiser_created == FFX_OK, "ffxDenoiserContextCreate returns FFX_OK (got " +
                                           std::to_string(denoiser_created) + ")");

    auto spd = std::make_unique<FfxSpdContext>();
    FfxSpdContextDescription spd_description{};
    spd_description.flags = FFX_SPD_SAMPLER_LOAD | FFX_SPD_WAVE_INTEROP_WAVE_OPS |
                            FFX_SPD_MATH_NONPACKED;
    spd_description.downsampleFilter = FFX_SPD_DOWNSAMPLE_FILTER_MIN;
    spd_description.backendInterface = backend.interface;
    const auto spd_created = ffxSpdContextCreate(spd.get(), &spd_description);
    expect(spd_created == FFX_OK, "ffxSpdContextCreate returns FFX_OK (got " +
                                      std::to_string(spd_created) + ")");

    if (spd_created == FFX_OK) expect(ffxSpdContextDestroy(spd.get()) == FFX_OK, "SPD is destroyed");
    if (denoiser_created == FFX_OK)
        expect(ffxDenoiserContextDestroy(denoiser.get()) == FFX_OK, "the denoiser is destroyed");
    if (classifier_created == FFX_OK)
        expect(ffxClassifierContextDestroy(classifier.get()) == FFX_OK,
               "the classifier is destroyed");
}

} // namespace

int main() {
    HeadlessDevice gpu;
    if (!gpu.device) {
        std::printf("SKIP: %s\n", gpu.skip_reason.c_str());
        return 0;
    }
    test_brixelizer_and_gi(gpu);
    test_reflection_effects(gpu);
    if (failures != 0) {
        std::fprintf(stderr, "%d FidelityFX check(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("FidelityFX effects created and destroyed on the GPU\n");
    return EXIT_SUCCESS;
}
