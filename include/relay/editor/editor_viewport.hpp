#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace relay {

struct EditorViewport {
    // Fractions of the framebuffer: layout uses logical desktop coordinates, Vulkan uses pixels.
    double x{}, y{}, width{1}, height{1};
    struct Pixels {
        std::uint32_t x{}, y{}, width{}, height{};
    };
    [[nodiscard]] Pixels pixels(std::uint32_t framebuffer_width,
                                std::uint32_t framebuffer_height) const {
        if (!framebuffer_width || !framebuffer_height) return {};
        const auto edge = [](double value, std::uint32_t extent) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * extent));
        };
        const auto left = std::min(edge(x, framebuffer_width), framebuffer_width - 1U);
        const auto top = std::min(edge(y, framebuffer_height), framebuffer_height - 1U);
        const auto right = edge(x + width, framebuffer_width);
        const auto bottom = edge(y + height, framebuffer_height);
        return {left, top, std::max(right, left + 1U) - left, std::max(bottom, top + 1U) - top};
    }
};

} // namespace relay
