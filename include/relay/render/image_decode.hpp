#pragma once

#include "relay/render/assets.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace relay {

// Decodes a PNG or JPEG held in memory into tightly packed RGBA8 pixels. The caller assigns the
// stable asset name and semantic color space after decoding.
[[nodiscard]] bool decode_image_rgba(std::span<const std::uint8_t> bytes,
                                     std::string_view format_hint, TextureAsset& output,
                                     std::string& error);

} // namespace relay
