#include "vulkan_ray_tracing.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_map>

namespace relay {
namespace {

struct Allocation {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDeviceSize size{};
    VkDeviceAddress address{};
    void* mapped{};
};

struct Structure {
    Allocation storage;
    VkAccelerationStructureKHR handle{};
    VkDeviceAddress address{};
    VkDeviceSize size{};
};

VkDeviceSize align(const VkDeviceSize value, const VkDeviceSize alignment) {
    return alignment == 0U ? value : (value + alignment - 1U) / alignment * alignment;
}

} // namespace

struct SceneAccelerationStructures::Impl {
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    VkDeviceSize scratch_alignment{256U};
    PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes{};
    PFN_vkCreateAccelerationStructureKHR create_structure{};
    PFN_vkDestroyAccelerationStructureKHR destroy_structure{};
    PFN_vkCmdBuildAccelerationStructuresKHR build_structures{};
    PFN_vkGetAccelerationStructureDeviceAddressKHR structure_address{};

    std::unordered_map<std::string, Structure> meshes;
    struct Frame {
        std::vector<Structure> deformed;
        Structure scene;
        Allocation instances;
        Allocation scratch;
    };
    std::vector<Frame> frames;

    ~Impl() {
        if (device == VK_NULL_HANDLE) return;
        for (auto& [name, structure] : meshes) destroy(structure);
        for (auto& frame : frames) {
            for (auto& structure : frame.deformed) destroy(structure);
            destroy(frame.scene);
            release(frame.instances);
            release(frame.scratch);
        }
    }

    std::optional<std::uint32_t> memory_type(const std::uint32_t allowed,
                                             const VkMemoryPropertyFlags flags) const {
        for (std::uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
            if ((allowed & (1U << index)) != 0U &&
                (memory_properties.memoryTypes[index].propertyFlags & flags) == flags)
                return index;
        return std::nullopt;
    }

    bool allocate(const VkDeviceSize size, const VkBufferUsageFlags usage, const bool host_visible,
                  Allocation& allocation, std::string& error) {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &buffer_info, nullptr, &allocation.buffer) != VK_SUCCESS) {
            error = "could not create a ray tracing buffer";
            return false;
        }
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, allocation.buffer, &requirements);
        const auto flags = host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const auto type = memory_type(requirements.memoryTypeBits, flags);
        if (!type) {
            error = "no memory type for a ray tracing buffer";
            return false;
        }
        VkMemoryAllocateFlagsInfo flags_info{};
        flags_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.pNext = &flags_info;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = *type;
        if (vkAllocateMemory(device, &allocate_info, nullptr, &allocation.memory) != VK_SUCCESS) {
            error = "out of memory for ray tracing structures";
            return false;
        }
        vkBindBufferMemory(device, allocation.buffer, allocation.memory, 0U);
        if (host_visible)
            vkMapMemory(device, allocation.memory, 0U, VK_WHOLE_SIZE, 0U, &allocation.mapped);
        VkBufferDeviceAddressInfo address{};
        address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address.buffer = allocation.buffer;
        allocation.address = vkGetBufferDeviceAddress(device, &address);
        allocation.size = size;
        return true;
    }

    void release(Allocation& allocation) {
        if (allocation.mapped) vkUnmapMemory(device, allocation.memory);
        vkDestroyBuffer(device, allocation.buffer, nullptr);
        vkFreeMemory(device, allocation.memory, nullptr);
        allocation = {};
    }

    void destroy(Structure& structure) {
        if (structure.handle) destroy_structure(device, structure.handle, nullptr);
        release(structure.storage);
        structure = {};
    }

    bool ensure_scratch(Frame& frame, const VkDeviceSize size, std::string& error) {
        if (frame.scratch.size >= size) return true;
        release(frame.scratch);
        return allocate(std::max<VkDeviceSize>(size, frame.scratch.size * 2U),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, frame.scratch, error);
    }

    // Creates (or keeps, when big enough) the storage for a structure of `size` bytes.
    bool ensure_structure(Structure& structure, const VkAccelerationStructureTypeKHR type,
                          const VkDeviceSize size, std::string& error) {
        if (structure.handle && structure.size >= size) return true;
        destroy(structure);
        if (!allocate(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false,
                      structure.storage, error))
            return false;
        VkAccelerationStructureCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        info.buffer = structure.storage.buffer;
        info.size = size;
        info.type = type;
        if (create_structure(device, &info, nullptr, &structure.handle) != VK_SUCCESS) {
            error = "could not create an acceleration structure";
            return false;
        }
        VkAccelerationStructureDeviceAddressInfoKHR address{};
        address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address.accelerationStructure = structure.handle;
        structure.address = structure_address(device, &address);
        structure.size = size;
        return true;
    }

    VkDeviceAddress buffer_address(const VkBuffer buffer) const {
        VkBufferDeviceAddressInfo address{};
        address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        address.buffer = buffer;
        return vkGetBufferDeviceAddress(device, &address);
    }

    VkAccelerationStructureGeometryKHR triangles(const RayTracingGeometry& geometry) const {
        VkAccelerationStructureGeometryKHR description{};
        description.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        description.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        description.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& data = description.geometry.triangles;
        data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        data.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        data.vertexData.deviceAddress = buffer_address(geometry.vertex_buffer) +
                                        static_cast<VkDeviceAddress>(geometry.first_vertex) *
                                            geometry.vertex_stride;
        data.vertexStride = geometry.vertex_stride;
        data.maxVertex = geometry.vertex_count > 0U ? geometry.vertex_count - 1U : 0U;
        data.indexType = VK_INDEX_TYPE_UINT32;
        data.indexData.deviceAddress = buffer_address(geometry.index_buffer) +
                                       static_cast<VkDeviceAddress>(geometry.first_index) *
                                           sizeof(std::uint32_t);
        return description;
    }

    struct PendingBuild {
        Structure* structure{};
        VkAccelerationStructureGeometryKHR geometry{};
        VkAccelerationStructureBuildRangeInfoKHR range{};
        VkDeviceSize scratch_offset{};
    };

    // Sizes and prepares a bottom-level build; the scratch offset is assigned later.
    bool prepare_bottom(Structure& structure, const RayTracingGeometry& geometry,
                        const VkBuildAccelerationStructureFlagsKHR flags,
                        std::vector<PendingBuild>& builds, VkDeviceSize& scratch_total,
                        std::string& error) {
        PendingBuild build;
        build.structure = &structure;
        build.geometry = triangles(geometry);
        build.range.primitiveCount = geometry.index_count / 3U;
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = flags;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1U;
        info.pGeometries = &build.geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        get_build_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
                        &build.range.primitiveCount, &sizes);
        if (!ensure_structure(structure, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                              sizes.accelerationStructureSize, error))
            return false;
        build.scratch_offset = align(scratch_total, scratch_alignment);
        scratch_total = build.scratch_offset + sizes.buildScratchSize;
        builds.push_back(build);
        return true;
    }

    void record_bottom(const VkCommandBuffer commands, const std::vector<PendingBuild>& builds,
                       const VkDeviceAddress scratch, const VkBuildAccelerationStructureFlagsKHR flags) {
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
        infos.reserve(builds.size());
        for (const auto& build : builds) {
            VkAccelerationStructureBuildGeometryInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = flags;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.dstAccelerationStructure = build.structure->handle;
            info.geometryCount = 1U;
            info.pGeometries = &build.geometry;
            info.scratchData.deviceAddress = scratch + build.scratch_offset;
            infos.push_back(info);
            ranges.push_back(&build.range);
        }
        if (!infos.empty())
            build_structures(commands, static_cast<std::uint32_t>(infos.size()), infos.data(),
                             ranges.data());
    }

    static void structure_barrier(const VkCommandBuffer commands,
                                  const VkPipelineStageFlags destination) {
        VkMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             destination, 0U, 1U, &barrier, 0U, nullptr, 0U, nullptr);
    }

    bool record(const VkCommandBuffer commands, const std::uint32_t frame_index,
                const std::vector<RayTracingInstance>& instances, std::string& error) {
        auto& frame = frames[frame_index];
        // Vertex data written by the host or by transfers this frame must be visible to builds.
        VkMemoryBarrier inputs{};
        inputs.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        inputs.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        inputs.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                               VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
                             &inputs, 0U, nullptr, 0U, nullptr);

        // Size every build first so the scratch buffer is allocated once, before any command
        // that uses it is recorded. Static and deformed builds use different flags and are
        // recorded as two batches at their own scratch offsets.
        std::vector<PendingBuild> static_builds, deformed_builds;
        VkDeviceSize scratch_total = 0U;
        std::vector<const Structure*> bottoms(instances.size(), nullptr);
        std::size_t deformed_count = 0U;
        for (const auto& instance : instances)
            if (instance.deformed) ++deformed_count;
        if (frame.deformed.size() < deformed_count) frame.deformed.resize(deformed_count);
        std::size_t deformed_index = 0U;
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const auto& instance = instances[index];
            if (instance.geometry.index_count < 3U) continue;
            if (instance.deformed) {
                auto& structure = frame.deformed[deformed_index++];
                if (!prepare_bottom(structure, instance.geometry,
                                    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR,
                                    deformed_builds, scratch_total, error))
                    return false;
                bottoms[index] = &structure;
                continue;
            }
            auto [entry, inserted] = meshes.try_emplace(instance.mesh);
            if (inserted &&
                !prepare_bottom(entry->second, instance.geometry,
                                VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                                static_builds, scratch_total, error)) {
                meshes.erase(entry);
                return false;
            }
            bottoms[index] = &entry->second;
        }

        // Top level: one instance per drawable, with the world transform's top three rows.
        std::vector<VkAccelerationStructureInstanceKHR> records;
        records.reserve(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            if (!bottoms[index]) continue;
            const auto& instance = instances[index];
            VkAccelerationStructureInstanceKHR record{};
            for (std::size_t row = 0; row < 3U; ++row)
                for (std::size_t column = 0; column < 4U; ++column)
                    record.transform.matrix[row][column] = instance.model[column * 4U + row];
            record.instanceCustomIndex = instance.custom_index & 0xFFFFFFU;
            record.mask = 0xFFU;
            record.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            record.accelerationStructureReference = bottoms[index]->address;
            records.push_back(record);
        }
        const auto instance_bytes = std::max<VkDeviceSize>(
            records.size() * sizeof(VkAccelerationStructureInstanceKHR), 64U);
        if (frame.instances.size < instance_bytes) {
            release(frame.instances);
            if (!allocate(std::max<VkDeviceSize>(instance_bytes, 64U * 1024U),
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                          true, frame.instances, error))
                return false;
        }
        if (!records.empty())
            std::memcpy(frame.instances.mapped, records.data(),
                        records.size() * sizeof(VkAccelerationStructureInstanceKHR));
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.data.deviceAddress = frame.instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1U;
        info.pGeometries = &geometry;
        const auto count = static_cast<std::uint32_t>(records.size());
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        get_build_sizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count,
                        &sizes);
        if (!ensure_structure(frame.scene, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                              sizes.accelerationStructureSize, error) ||
            !ensure_scratch(frame, std::max<VkDeviceSize>({scratch_total, sizes.buildScratchSize, 1U}),
                            error))
            return false;

        record_bottom(commands, static_builds, frame.scratch.address,
                      VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR);
        record_bottom(commands, deformed_builds, frame.scratch.address,
                      VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR);
        // The bottom levels must be complete, and done with the scratch, before the top level.
        VkMemoryBarrier bottoms_done{};
        bottoms_done.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        bottoms_done.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        bottoms_done.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                     VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0U, 1U,
                             &bottoms_done, 0U, nullptr, 0U, nullptr);
        info.dstAccelerationStructure = frame.scene.handle;
        info.scratchData.deviceAddress = frame.scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{};
        range.primitiveCount = count;
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        build_structures(commands, 1U, &info, &ranges);
        structure_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        return true;
    }
};

SceneAccelerationStructures::SceneAccelerationStructures() : impl_(std::make_unique<Impl>()) {}
SceneAccelerationStructures::~SceneAccelerationStructures() = default;

bool SceneAccelerationStructures::initialize(const VkPhysicalDevice physical_device,
                                             const VkDevice device,
                                             const std::uint32_t frames_in_flight,
                                             std::string& error) {
    auto& impl = *impl_;
    impl.physical_device = physical_device;
    impl.device = device;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &impl.memory_properties);
    VkPhysicalDeviceAccelerationStructurePropertiesKHR structure_properties{};
    structure_properties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &structure_properties;
    vkGetPhysicalDeviceProperties2(physical_device, &properties);
    impl.scratch_alignment =
        std::max<VkDeviceSize>(structure_properties.minAccelerationStructureScratchOffsetAlignment, 1U);
    impl.get_build_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureBuildSizesKHR"));
    impl.create_structure = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkCreateAccelerationStructureKHR"));
    impl.destroy_structure = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        vkGetDeviceProcAddr(device, "vkDestroyAccelerationStructureKHR"));
    impl.build_structures = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        vkGetDeviceProcAddr(device, "vkCmdBuildAccelerationStructuresKHR"));
    impl.structure_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        vkGetDeviceProcAddr(device, "vkGetAccelerationStructureDeviceAddressKHR"));
    if (!impl.get_build_sizes || !impl.create_structure || !impl.destroy_structure ||
        !impl.build_structures || !impl.structure_address) {
        error = "the device does not expose acceleration structure functions";
        return false;
    }
    impl.frames.resize(frames_in_flight);
    return true;
}

bool SceneAccelerationStructures::record(const VkCommandBuffer commands, const std::uint32_t frame,
                                         const std::vector<RayTracingInstance>& instances,
                                         std::string& error) {
    return impl_->record(commands, frame, instances, error);
}

VkAccelerationStructureKHR SceneAccelerationStructures::scene(const std::uint32_t frame) const {
    return impl_->frames[frame].scene.handle;
}

std::uint32_t SceneAccelerationStructures::static_structures() const {
    return static_cast<std::uint32_t>(impl_->meshes.size());
}

} // namespace relay
