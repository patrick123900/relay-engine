#include "relay/render/texture_compression.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace relay {
namespace {

struct Color {
    float r{}, g{}, b{};
};

std::uint16_t pack565(const Color& c) {
    const auto q = [](const float value, const int max) {
        return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround(value / 255.0F * static_cast<float>(max))), 0, max));
    };
    return static_cast<std::uint16_t>((q(c.r, 31) << 11U) | (q(c.g, 63) << 5U) | q(c.b, 31));
}

Color unpack565(const std::uint16_t value) {
    const auto r = static_cast<float>((value >> 11U) & 31U);
    const auto g = static_cast<float>((value >> 5U) & 63U);
    const auto b = static_cast<float>(value & 31U);
    // How a GPU widens the channels: the top bits repeat into the bottom.
    return {std::floor(r * 255.0F / 31.0F + 0.5F), std::floor(g * 255.0F / 63.0F + 0.5F), std::floor(b * 255.0F / 31.0F + 0.5F)};
}

std::array<Color, 4> bc1_palette(const std::uint16_t c0, const std::uint16_t c1) {
    const auto a = unpack565(c0);
    const auto b = unpack565(c1);
    std::array<Color, 4> palette{a, b, {}, {}};
    if (c0 > c1) {
        palette[2] = {std::floor((2.0F * a.r + b.r) / 3.0F), std::floor((2.0F * a.g + b.g) / 3.0F), std::floor((2.0F * a.b + b.b) / 3.0F)};
        palette[3] = {std::floor((a.r + 2.0F * b.r) / 3.0F), std::floor((a.g + 2.0F * b.g) / 3.0F), std::floor((a.b + 2.0F * b.b) / 3.0F)};
    } else {
        palette[2] = {std::floor((a.r + b.r) / 2.0F), std::floor((a.g + b.g) / 2.0F), std::floor((a.b + b.b) / 2.0F)};
        palette[3] = {0.0F, 0.0F, 0.0F};
    }
    return palette;
}

float distance2(const Color& a, const Color& b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// Picks the closest palette entry for each of the 16 pixels, returning the total squared error.
float assign_indices(const std::array<Color, 16>& pixels, const std::array<Color, 4>& palette, const int entries,
                     std::array<std::uint8_t, 16>& indices) {
    float error = 0.0F;
    for (std::size_t i = 0; i < 16; ++i) {
        int best = 0;
        float best_distance = distance2(pixels[i], palette[0]);
        for (int k = 1; k < entries; ++k) {
            const float d = distance2(pixels[i], palette[static_cast<std::size_t>(k)]);
            if (d < best_distance) { best_distance = d; best = k; }
        }
        indices[i] = static_cast<std::uint8_t>(best);
        error += best_distance;
    }
    return error;
}

// The 4-colour BC1 mapping of palette order to the index bits: 0 -> c0, 1 -> c1, 2 -> 2/3 c0, 3 -> 1/3 c0.
// `assign_indices` numbers entries 0..3 as stored, so no remapping is needed.

void write_color_block(std::uint8_t* out, const std::uint16_t c0, const std::uint16_t c1,
                       const std::array<std::uint8_t, 16>& indices) {
    out[0] = static_cast<std::uint8_t>(c0 & 0xFFU);
    out[1] = static_cast<std::uint8_t>(c0 >> 8U);
    out[2] = static_cast<std::uint8_t>(c1 & 0xFFU);
    out[3] = static_cast<std::uint8_t>(c1 >> 8U);
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 16; ++i) bits |= static_cast<std::uint32_t>(indices[i]) << (2U * i);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>((bits >> (8U * static_cast<unsigned>(i))) & 0xFFU);
}

// Compresses 16 RGB pixels into a BC1 colour block (always the four-colour mode, so it also serves BC3).
void encode_color_block(const std::array<Color, 16>& pixels, std::uint8_t* out) {
    Color mean{};
    for (const auto& p : pixels) { mean.r += p.r; mean.g += p.g; mean.b += p.b; }
    mean = {mean.r / 16.0F, mean.g / 16.0F, mean.b / 16.0F};
    // Principal axis of the block's colours: the line the endpoints sit on.
    float cov[6] = {};
    for (const auto& p : pixels) {
        const float r = p.r - mean.r, g = p.g - mean.g, b = p.b - mean.b;
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    Color axis{1.0F, 1.0F, 1.0F};
    for (int iteration = 0; iteration < 6; ++iteration) {
        const Color next{cov[0] * axis.r + cov[1] * axis.g + cov[2] * axis.b,
                         cov[1] * axis.r + cov[3] * axis.g + cov[4] * axis.b,
                         cov[2] * axis.r + cov[4] * axis.g + cov[5] * axis.b};
        const float length = std::sqrt(next.r * next.r + next.g * next.g + next.b * next.b);
        if (length < 1e-6F) break;
        axis = {next.r / length, next.g / length, next.b / length};
    }
    float low = 1e30F, high = -1e30F;
    for (const auto& p : pixels) {
        const float t = (p.r - mean.r) * axis.r + (p.g - mean.g) * axis.g + (p.b - mean.b) * axis.b;
        low = std::min(low, t);
        high = std::max(high, t);
    }
    // Pull the ends in a little: the extreme pixels cost more than they gain.
    const float inset = (high - low) / 16.0F;
    low += inset;
    high -= inset;
    const auto at = [&](const float t) {
        return Color{std::clamp(mean.r + axis.r * t, 0.0F, 255.0F), std::clamp(mean.g + axis.g * t, 0.0F, 255.0F),
                     std::clamp(mean.b + axis.b * t, 0.0F, 255.0F)};
    };
    const auto c0 = pack565(at(high));
    const auto c1 = pack565(at(low));
    // Two passes: the endpoints from the line, then the endpoints that fit the chosen indices best.
    std::array<std::uint8_t, 16> indices{}, best_indices{};
    auto first = std::max(c0, c1), second = std::min(c0, c1);
    float best_error = assign_indices(pixels, bc1_palette(first, second), first == second ? 1 : 4, best_indices);
    auto best_first = first, best_second = second;
    for (int pass = 0; pass < 3 && best_error > 0.0F; ++pass) {
        // Least squares: pixel i should be (w0*A + w1*B) with weights from its index.
        static constexpr float weights[4] = {1.0F, 0.0F, 2.0F / 3.0F, 1.0F / 3.0F};
        float aa = 0, bb = 0, ab = 0;
        Color ax{}, bx{};
        for (std::size_t i = 0; i < 16; ++i) {
            const float w = weights[best_indices[i]];
            const float v = 1.0F - w;
            aa += w * w; bb += v * v; ab += w * v;
            ax.r += w * pixels[i].r; ax.g += w * pixels[i].g; ax.b += w * pixels[i].b;
            bx.r += v * pixels[i].r; bx.g += v * pixels[i].g; bx.b += v * pixels[i].b;
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6F) break;
        const auto solve = [&](const float p, const float q) {
            return std::pair<float, float>{(p * bb - q * ab) / det, (q * aa - p * ab) / det};
        };
        const auto r = solve(ax.r, bx.r), g = solve(ax.g, bx.g), b = solve(ax.b, bx.b);
        const auto clamp = [](const float v) { return std::clamp(v, 0.0F, 255.0F); };
        const auto n0 = pack565({clamp(r.first), clamp(g.first), clamp(b.first)});
        const auto n1 = pack565({clamp(r.second), clamp(g.second), clamp(b.second)});
        first = std::max(n0, n1);
        second = std::min(n0, n1);
        const float error = assign_indices(pixels, bc1_palette(first, second), first == second ? 1 : 4, indices);
        if (error + 1e-3F >= best_error) break;
        best_error = error;
        best_first = first;
        best_second = second;
        best_indices = indices;
    }
    write_color_block(out, best_first, best_second, best_indices);
}

// An 8-byte BC4-style block holding one channel of 16 pixels (BC3's alpha).
void encode_alpha_block(const std::array<std::uint8_t, 16>& values, std::uint8_t* out) {
    const auto [low_it, high_it] = std::minmax_element(values.begin(), values.end());
    const int low = *low_it, high = *high_it;
    std::array<int, 8> palette{};
    int a0 = high, a1 = low;
    if (a0 == a1) {
        // A flat block: the six-value mode (a0 <= a1) with any index returns the value.
        out[0] = static_cast<std::uint8_t>(a0);
        out[1] = static_cast<std::uint8_t>(a1);
        std::memset(out + 2, 0, 6);
        return;
    }
    palette[0] = a0;
    palette[1] = a1;
    for (int k = 1; k <= 6; ++k) palette[static_cast<std::size_t>(k + 1)] = ((7 - k) * a0 + k * a1 + 3) / 7;
    out[0] = static_cast<std::uint8_t>(a0);
    out[1] = static_cast<std::uint8_t>(a1);
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < 16; ++i) {
        int best = 0, best_distance = 1 << 20;
        for (int k = 0; k < 8; ++k) {
            const int d = std::abs(palette[static_cast<std::size_t>(k)] - values[i]);
            if (d < best_distance) { best_distance = d; best = k; }
        }
        bits |= static_cast<std::uint64_t>(best) << (3U * i);
    }
    for (int i = 0; i < 6; ++i) out[2 + i] = static_cast<std::uint8_t>((bits >> (8U * static_cast<unsigned>(i))) & 0xFFU);
}

float srgb_to_linear(const std::uint8_t value) {
    static const auto table = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0F;
            t[i] = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
        }
        return t;
    }();
    return table[value];
}

std::uint8_t linear_to_srgb(const float value) {
    const float c = std::clamp(value, 0.0F, 1.0F);
    const float s = c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(s * 255.0F)), 0, 255));
}

// Half-size copy of an RGBA8 image: each pixel the average of up to four, in linear light for colour.
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& source, const std::uint32_t width,
                                const std::uint32_t height, const bool srgb, std::uint32_t& out_width,
                                std::uint32_t& out_height) {
    out_width = std::max(width / 2U, 1U);
    out_height = std::max(height / 2U, 1U);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(out_width) * out_height * 4U);
    for (std::uint32_t y = 0; y < out_height; ++y)
        for (std::uint32_t x = 0; x < out_width; ++x) {
            float sum[4] = {};
            int count = 0;
            for (std::uint32_t dy = 0; dy < 2U; ++dy)
                for (std::uint32_t dx = 0; dx < 2U; ++dx) {
                    const auto sx = std::min(x * 2U + dx, width - 1U);
                    const auto sy = std::min(y * 2U + dy, height - 1U);
                    const auto* pixel = &source[(static_cast<std::size_t>(sy) * width + sx) * 4U];
                    for (int c = 0; c < 3; ++c) sum[c] += srgb ? srgb_to_linear(pixel[c]) : static_cast<float>(pixel[c]) / 255.0F;
                    sum[3] += static_cast<float>(pixel[3]) / 255.0F;
                    ++count;
                }
            auto* out = &result[(static_cast<std::size_t>(y) * out_width + x) * 4U];
            for (int c = 0; c < 3; ++c) {
                const float average = sum[c] / static_cast<float>(count);
                out[c] = srgb ? linear_to_srgb(average)
                              : static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(average * 255.0F)), 0, 255));
            }
            out[3] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(sum[3] / static_cast<float>(count) * 255.0F)), 0, 255));
        }
    return result;
}

} // namespace

std::uint32_t texture_block_bytes(const TextureCodec codec) { return codec == TextureCodec::bc1 ? 8U : 16U; }

std::uint64_t compressed_level_bytes(const TextureCodec codec, const std::uint32_t width, const std::uint32_t height) {
    return static_cast<std::uint64_t>((width + 3U) / 4U) * ((height + 3U) / 4U) * texture_block_bytes(codec);
}

std::uint32_t full_mip_levels(const std::uint32_t width, const std::uint32_t height) {
    std::uint32_t levels = 1U;
    for (auto size = std::max(width, height); size > 1U; size /= 2U) ++levels;
    return levels;
}

TextureCodec choose_texture_codec(const std::span<const std::uint8_t> rgba) {
    for (std::size_t i = 3; i < rgba.size(); i += 4U)
        if (rgba[i] != 255U) return TextureCodec::bc3;
    return TextureCodec::bc1;
}

CompressedTexture compress_texture(const std::span<const std::uint8_t> rgba, const std::uint32_t width,
                                   const std::uint32_t height, const bool srgb, const TextureCodec codec) {
    CompressedTexture result;
    result.codec = codec;
    result.width = width;
    result.height = height;
    if (width == 0U || height == 0U || rgba.size() < static_cast<std::size_t>(width) * height * 4U) return result;
    std::vector<std::uint8_t> level(rgba.begin(), rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(width) * height * 4U));
    std::uint32_t level_width = width, level_height = height;
    const auto levels = full_mip_levels(width, height);
    for (std::uint32_t index = 0; index < levels; ++index) {
        result.level_offsets.push_back(static_cast<std::uint32_t>(result.data.size()));
        const auto blocks_x = (level_width + 3U) / 4U, blocks_y = (level_height + 3U) / 4U;
        const auto block = texture_block_bytes(codec);
        const auto base = result.data.size();
        result.data.resize(base + static_cast<std::size_t>(blocks_x) * blocks_y * block);
        for (std::uint32_t by = 0; by < blocks_y; ++by)
            for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
                std::array<Color, 16> pixels{};
                std::array<std::uint8_t, 16> alphas{};
                for (std::uint32_t py = 0; py < 4U; ++py)
                    for (std::uint32_t px = 0; px < 4U; ++px) {
                        // Pixels past the edge repeat the last row and column.
                        const auto sx = std::min(bx * 4U + px, level_width - 1U);
                        const auto sy = std::min(by * 4U + py, level_height - 1U);
                        const auto* pixel = &level[(static_cast<std::size_t>(sy) * level_width + sx) * 4U];
                        pixels[py * 4U + px] = {static_cast<float>(pixel[0]), static_cast<float>(pixel[1]), static_cast<float>(pixel[2])};
                        alphas[py * 4U + px] = pixel[3];
                    }
                auto* out = &result.data[base + (static_cast<std::size_t>(by) * blocks_x + bx) * block];
                if (codec == TextureCodec::bc3) {
                    encode_alpha_block(alphas, out);
                    encode_color_block(pixels, out + 8);
                } else {
                    encode_color_block(pixels, out);
                }
            }
        if (index + 1U < levels) level = halve(level, level_width, level_height, srgb, level_width, level_height);
    }
    result.level_offsets.push_back(static_cast<std::uint32_t>(result.data.size()));
    return result;
}

std::vector<std::uint8_t> decompress_texture_level(const CompressedTexture& texture, const std::uint32_t level) {
    if (level >= texture.levels()) return {};
    const auto level_width = std::max(texture.width >> level, 1U);
    const auto level_height = std::max(texture.height >> level, 1U);
    const auto blocks_x = (level_width + 3U) / 4U, blocks_y = (level_height + 3U) / 4U;
    const auto block = texture_block_bytes(texture.codec);
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(level_width) * level_height * 4U);
    const auto* base = &texture.data[texture.level_offsets[level]];
    for (std::uint32_t by = 0; by < blocks_y; ++by)
        for (std::uint32_t bx = 0; bx < blocks_x; ++bx) {
            const auto* in = base + (static_cast<std::size_t>(by) * blocks_x + bx) * block;
            std::array<std::uint8_t, 16> alpha;
            alpha.fill(255);
            const auto* color = in;
            if (texture.codec == TextureCodec::bc3) {
                std::array<int, 8> palette{};
                palette[0] = in[0];
                palette[1] = in[1];
                if (in[0] > in[1]) {
                    for (int k = 1; k <= 6; ++k) palette[static_cast<std::size_t>(k + 1)] = ((7 - k) * in[0] + k * in[1] + 3) / 7;
                } else {
                    for (int k = 1; k <= 4; ++k) palette[static_cast<std::size_t>(k + 1)] = ((5 - k) * in[0] + k * in[1] + 2) / 5;
                    palette[6] = 0;
                    palette[7] = 255;
                }
                std::uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(in[2 + i]) << (8U * static_cast<unsigned>(i));
                for (std::size_t i = 0; i < 16; ++i)
                    alpha[i] = static_cast<std::uint8_t>(palette[(bits >> (3U * i)) & 7U]);
                color = in + 8;
            }
            const auto c0 = static_cast<std::uint16_t>(color[0] | (color[1] << 8U));
            const auto c1 = static_cast<std::uint16_t>(color[2] | (color[3] << 8U));
            auto palette = bc1_palette(c0, c1);
            // BC3 always reads the colour block in four-colour mode.
            if (texture.codec == TextureCodec::bc3 && c0 <= c1) {
                const auto a = unpack565(c0), b = unpack565(c1);
                palette[2] = {std::floor((2.0F * a.r + b.r) / 3.0F), std::floor((2.0F * a.g + b.g) / 3.0F), std::floor((2.0F * a.b + b.b) / 3.0F)};
                palette[3] = {std::floor((a.r + 2.0F * b.r) / 3.0F), std::floor((a.g + 2.0F * b.g) / 3.0F), std::floor((a.b + 2.0F * b.b) / 3.0F)};
            }
            std::uint32_t bits = 0;
            for (int i = 0; i < 4; ++i) bits |= static_cast<std::uint32_t>(color[4 + i]) << (8U * static_cast<unsigned>(i));
            for (std::uint32_t py = 0; py < 4U; ++py)
                for (std::uint32_t px = 0; px < 4U; ++px) {
                    const auto x = bx * 4U + px, y = by * 4U + py;
                    if (x >= level_width || y >= level_height) continue;
                    const auto index = (bits >> (2U * (py * 4U + px))) & 3U;
                    const auto& c = palette[index];
                    auto* out = &rgba[(static_cast<std::size_t>(y) * level_width + x) * 4U];
                    out[0] = static_cast<std::uint8_t>(c.r);
                    out[1] = static_cast<std::uint8_t>(c.g);
                    out[2] = static_cast<std::uint8_t>(c.b);
                    out[3] = alpha[py * 4U + px];
                }
        }
    return rgba;
}

} // namespace relay
