#include "relay/render/upload_budget.hpp"

#include <algorithm>
#include <limits>

namespace relay {
namespace {

bool add(std::uint64_t& total, const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total) {
        total = std::numeric_limits<std::uint64_t>::max();
        return false;
    }
    total += value;
    return true;
}

} // namespace

std::uint64_t texture_mip_bytes(std::uint32_t width, std::uint32_t height) {
    if (width == 0U || height == 0U) return 0U;
    std::uint64_t total = 0U;
    while (true) {
        if (static_cast<std::uint64_t>(width) >
            std::numeric_limits<std::uint64_t>::max() / (4ULL * height))
            return std::numeric_limits<std::uint64_t>::max();
        if (!add(total, 4ULL * width * height)) return total;
        if (width == 1U && height == 1U) break;
        width = std::max(width / 2U, 1U);
        height = std::max(height / 2U, 1U);
    }
    return total;
}

UploadEstimate estimate_asset_upload(const AssetRegistry& assets,
                                     const std::size_t first_texture,
                                     const std::uint64_t vertex_allocation,
                                     const std::uint64_t index_allocation,
                                     const std::uint64_t material_allocation,
                                     const std::uint64_t staged_geometry_bytes) {
    UploadEstimate result;
    result.staging_bytes = staged_geometry_bytes;
    result.overflow = !add(result.device_bytes, vertex_allocation) ||
                      !add(result.device_bytes, index_allocation) ||
                      !add(result.device_bytes, material_allocation);
    const auto textures = assets.textures();
    for (std::size_t index = std::min(first_texture, textures.size()); index < textures.size(); ++index) {
        const auto& texture = textures[index];
        const auto bytes = texture_mip_bytes(texture.width, texture.height);
        result.largest_texture_bytes = std::max(result.largest_texture_bytes, bytes);
        result.overflow |= !add(result.device_bytes, bytes);
        result.overflow |= !add(result.staging_bytes,
                                static_cast<std::uint64_t>(texture.rgba.size()));
        ++result.new_textures;
    }
    return result;
}

std::string check_upload_budget(const UploadEstimate& estimate, const UploadBudget& budget,
                                const std::uint64_t resident_device_bytes) {
    if (estimate.overflow || estimate.device_bytes >
                                 std::numeric_limits<std::uint64_t>::max() - resident_device_bytes)
        return "asset upload size overflow";
    if (estimate.largest_texture_bytes > budget.single_texture_bytes)
        return "texture mip chain exceeds the single-texture device budget";
    if (estimate.staging_bytes > budget.staging_bytes)
        return "asset upload exceeds the staging memory budget";
    if (resident_device_bytes + estimate.device_bytes > budget.device_bytes)
        return "asset upload exceeds the estimated device memory budget";
    return {};
}

} // namespace relay
