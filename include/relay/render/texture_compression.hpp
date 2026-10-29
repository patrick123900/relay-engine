#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace relay {

// GPU texture compression for cooked games: BC1 (opaque colour, 4 bits a pixel) and BC3 (colour
// with alpha, 8 bits a pixel), which every desktop GPU samples directly. A build compresses the
// textures of imported models once, with a full mip chain, so the game uploads the bytes as they
// are: a fourth or an eighth of the memory and no mip generation. A device that cannot sample them
// gets the pixels decompressed on the CPU instead.
enum class TextureCodec : std::uint8_t { bc1 = 1, bc3 = 3 };

struct CompressedTexture {
    TextureCodec codec{TextureCodec::bc1};
    std::uint32_t width{};
    std::uint32_t height{};
    // Byte offset of each mip level in `data`, then the total; so levels() + 1 entries.
    std::vector<std::uint32_t> level_offsets;
    std::vector<std::uint8_t> data;
    [[nodiscard]] std::uint32_t levels() const {
        return level_offsets.empty() ? 0U : static_cast<std::uint32_t>(level_offsets.size() - 1U);
    }
};

[[nodiscard]] std::uint32_t texture_block_bytes(TextureCodec codec);
// Bytes one mip level of the given size takes (blocks of 4x4 pixels, partial blocks rounded up).
[[nodiscard]] std::uint64_t compressed_level_bytes(TextureCodec codec, std::uint32_t width, std::uint32_t height);
// Mip levels down to 1x1.
[[nodiscard]] std::uint32_t full_mip_levels(std::uint32_t width, std::uint32_t height);

// BC1 when every pixel is opaque, else BC3.
[[nodiscard]] TextureCodec choose_texture_codec(std::span<const std::uint8_t> rgba);

// Builds the mip chain of an RGBA8 image (each level a filtered half of the one before, averaged in
// linear light when `srgb`) and compresses every level.
[[nodiscard]] CompressedTexture compress_texture(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                                 std::uint32_t height, bool srgb, TextureCodec codec);
// One mip level back to RGBA8, width and height of that level.
[[nodiscard]] std::vector<std::uint8_t> decompress_texture_level(const CompressedTexture& texture,
                                                                  std::uint32_t level);

} // namespace relay
