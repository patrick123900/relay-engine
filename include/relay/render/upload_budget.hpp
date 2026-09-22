#pragma once

#include "relay/render/assets.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace relay {

struct UploadBudget {
    std::uint64_t staging_bytes{512ULL * 1024ULL * 1024ULL};
    std::uint64_t device_bytes{2ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t single_texture_bytes{256ULL * 1024ULL * 1024ULL};
};

struct UploadEstimate {
    std::uint64_t staging_bytes{};
    std::uint64_t device_bytes{};
    std::uint64_t largest_texture_bytes{};
    std::uint32_t new_textures{};
    bool overflow{};
};

// Conservative RGBA8 mip-chain size, independent of Vulkan allocation granularity.
[[nodiscard]] std::uint64_t texture_mip_bytes(std::uint32_t width, std::uint32_t height);

// Geometry/material allocation sizes include the renderer's geometric headroom. Existing textures
// are counted only when they must be uploaded; the caller adds any resources still resident.
[[nodiscard]] UploadEstimate estimate_asset_upload(const AssetRegistry& assets,
                                                   std::size_t first_texture,
                                                   std::uint64_t vertex_allocation,
                                                   std::uint64_t index_allocation,
                                                   std::uint64_t material_allocation,
                                                   std::uint64_t staged_geometry_bytes);
[[nodiscard]] std::string check_upload_budget(const UploadEstimate& estimate,
                                              const UploadBudget& budget,
                                              std::uint64_t resident_device_bytes = 0U);

} // namespace relay
