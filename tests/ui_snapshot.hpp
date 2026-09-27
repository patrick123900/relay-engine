#pragma once

// Test-only: rasterizes the headless editor's last Dear ImGui frame into a PNG, so interface work
// can be looked at without a window or a desktop. Every textured draw samples the font atlas, which
// is the only texture the headless editor has. Colors are blended in sRGB like the Vulkan backend.

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace relay_test {

inline void write_png_rgb(const std::filesystem::path& path, const int width, const int height,
                          const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(height) * (static_cast<std::size_t>(width) * 3U + 1U));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0U);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * width * 3,
                   rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * width * 3);
    }
    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    for (std::size_t offset = 0; offset < raw.size(); offset += 65535U) {
        const auto length = static_cast<std::uint16_t>(std::min<std::size_t>(65535U, raw.size() - offset));
        zlib.push_back(offset + length >= raw.size() ? 1U : 0U);
        zlib.insert(zlib.end(), {static_cast<std::uint8_t>(length & 0xffU), static_cast<std::uint8_t>(length >> 8U),
                                 static_cast<std::uint8_t>(~length & 0xffU), static_cast<std::uint8_t>((~length >> 8U) & 0xffU)});
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                    raw.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    std::uint32_t a = 1U, b = 0U;
    for (const auto byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    const std::uint32_t adler = (b << 16U) | a;
    zlib.insert(zlib.end(), {static_cast<std::uint8_t>(adler >> 24U), static_cast<std::uint8_t>(adler >> 16U),
                             static_cast<std::uint8_t>(adler >> 8U), static_cast<std::uint8_t>(adler)});
    const auto crc = [](const std::vector<std::uint8_t>& bytes) {
        std::uint32_t value = 0xffffffffU;
        for (const auto byte : bytes) {
            value ^= byte;
            for (int bit = 0; bit < 8; ++bit) value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
        }
        return ~value;
    };
    std::ofstream output(path, std::ios::binary);
    const auto big_endian = [&](const std::uint32_t value) {
        const std::array<char, 4> bytes{static_cast<char>(value >> 24U), static_cast<char>(value >> 16U),
                                        static_cast<char>(value >> 8U), static_cast<char>(value)};
        output.write(bytes.data(), 4);
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        big_endian(static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> checked(type, type + 4);
        checked.insert(checked.end(), data.begin(), data.end());
        output.write(reinterpret_cast<const char*>(checked.data()), static_cast<std::streamsize>(checked.size()));
        big_endian(crc(checked));
    };
    output.write("\x89PNG\r\n\x1a\n", 8);
    const auto w = static_cast<std::uint32_t>(width), h = static_cast<std::uint32_t>(height);
    chunk("IHDR", {static_cast<std::uint8_t>(w >> 24U), static_cast<std::uint8_t>(w >> 16U),
                   static_cast<std::uint8_t>(w >> 8U), static_cast<std::uint8_t>(w),
                   static_cast<std::uint8_t>(h >> 24U), static_cast<std::uint8_t>(h >> 16U),
                   static_cast<std::uint8_t>(h >> 8U), static_cast<std::uint8_t>(h), 8U, 2U, 0U, 0U, 0U});
    chunk("IDAT", zlib);
    chunk("IEND", {});
}

// Writes the last rendered frame; returns false when there is none.
inline bool write_ui_snapshot(const std::filesystem::path& path) {
    const auto* data = ImGui::GetDrawData();
    if (!data || !data->Valid) return false;
    const int width = static_cast<int>(data->DisplaySize.x), height = static_cast<int>(data->DisplaySize.y);
    std::vector<float> frame(static_cast<std::size_t>(width) * height * 3U, 0.08F);
    auto* atlas = ImGui::GetIO().Fonts->TexData;
    const auto texel = [&](float u, float v) -> std::array<float, 4> {
        if (!atlas || !atlas->Pixels) return {1, 1, 1, 1};
        const int x = std::clamp(static_cast<int>(u * static_cast<float>(atlas->Width)), 0, atlas->Width - 1);
        const int y = std::clamp(static_cast<int>(v * static_cast<float>(atlas->Height)), 0, atlas->Height - 1);
        const auto* pixel = static_cast<const unsigned char*>(atlas->GetPixelsAt(x, y));
        if (atlas->Format == ImTextureFormat_Alpha8) return {1, 1, 1, pixel[0] / 255.0F};
        return {pixel[0] / 255.0F, pixel[1] / 255.0F, pixel[2] / 255.0F, pixel[3] / 255.0F};
    };
    for (int list = 0; list < data->CmdListsCount; ++list) {
        const auto* commands = data->CmdLists[list];
        for (const auto& command : commands->CmdBuffer) {
            if (command.UserCallback) continue;
            const int clip_x0 = std::max(0, static_cast<int>(command.ClipRect.x - data->DisplayPos.x));
            const int clip_y0 = std::max(0, static_cast<int>(command.ClipRect.y - data->DisplayPos.y));
            const int clip_x1 = std::min(width, static_cast<int>(command.ClipRect.z - data->DisplayPos.x));
            const int clip_y1 = std::min(height, static_cast<int>(command.ClipRect.w - data->DisplayPos.y));
            for (unsigned int index = 0; index + 2U < command.ElemCount; index += 3U) {
                std::array<const ImDrawVert*, 3> v{};
                for (int corner = 0; corner < 3; ++corner)
                    v[static_cast<std::size_t>(corner)] =
                        &commands->VtxBuffer[static_cast<int>(command.VtxOffset) +
                                             commands->IdxBuffer[static_cast<int>(command.IdxOffset + index) + corner]];
                const float x0 = v[0]->pos.x - data->DisplayPos.x, y0 = v[0]->pos.y - data->DisplayPos.y;
                const float x1 = v[1]->pos.x - data->DisplayPos.x, y1 = v[1]->pos.y - data->DisplayPos.y;
                const float x2 = v[2]->pos.x - data->DisplayPos.x, y2 = v[2]->pos.y - data->DisplayPos.y;
                const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (std::abs(area) < 1e-6F) continue;
                const int left = std::max(clip_x0, static_cast<int>(std::floor(std::min({x0, x1, x2}))));
                const int right = std::min(clip_x1, static_cast<int>(std::ceil(std::max({x0, x1, x2}))));
                const int top = std::max(clip_y0, static_cast<int>(std::floor(std::min({y0, y1, y2}))));
                const int bottom = std::min(clip_y1, static_cast<int>(std::ceil(std::max({y0, y1, y2}))));
                for (int y = top; y < bottom; ++y)
                    for (int x = left; x < right; ++x) {
                        const float px = static_cast<float>(x) + 0.5F, py = static_cast<float>(y) + 0.5F;
                        const float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area;
                        const float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area;
                        const float w2 = 1.0F - w0 - w1;
                        if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) continue;
                        std::array<float, 4> color{};
                        for (int channel = 0; channel < 4; ++channel) {
                            const auto part = [&](const ImDrawVert* vertex) {
                                return static_cast<float>((vertex->col >> (8 * channel)) & 0xffU) / 255.0F;
                            };
                            color[static_cast<std::size_t>(channel)] = part(v[0]) * w0 + part(v[1]) * w1 + part(v[2]) * w2;
                        }
                        const auto sample = texel(v[0]->uv.x * w0 + v[1]->uv.x * w1 + v[2]->uv.x * w2,
                                                  v[0]->uv.y * w0 + v[1]->uv.y * w1 + v[2]->uv.y * w2);
                        const float alpha = color[3] * sample[3];
                        auto* out = &frame[(static_cast<std::size_t>(y) * width + x) * 3U];
                        for (int channel = 0; channel < 3; ++channel)
                            out[channel] = out[channel] * (1.0F - alpha) +
                                           color[static_cast<std::size_t>(channel)] * sample[static_cast<std::size_t>(channel)] * alpha;
                    }
            }
        }
    }
    std::vector<std::uint8_t> rgb(frame.size());
    for (std::size_t index = 0; index < frame.size(); ++index)
        rgb[index] = static_cast<std::uint8_t>(std::clamp(frame[index], 0.0F, 1.0F) * 255.0F + 0.5F);
    write_png_rgb(path, width, height, rgb);
    return true;
}

} // namespace relay_test
