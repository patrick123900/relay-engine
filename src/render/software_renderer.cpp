#include "relay/render/renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>

namespace relay {
namespace {

void write_u16(std::ostream& stream, const std::uint16_t value) {
    const std::array bytes{static_cast<char>(value & 0xffU), static_cast<char>((value >> 8U) & 0xffU)};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u32(std::ostream& stream, const std::uint32_t value) {
    const std::array bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

SoftwareRenderer::SoftwareRenderer(const std::uint32_t width, const std::uint32_t height)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4U, 0) {
    render(0, 0.0);
}

void SoftwareRenderer::render(const std::uint64_t frame_index, const double elapsed_seconds) {
    const auto square_size = std::max<std::uint32_t>(24U, std::min(width_, height_) / 7U);
    const auto travel = width_ > square_size ? width_ - square_size : 1U;
    const auto square_x = static_cast<std::uint32_t>(frame_index * 4U % travel);
    const auto square_y = height_ / 2U - std::min(square_size, height_) / 2U;
    const auto pulse = static_cast<std::uint8_t>(160.0 + 70.0 * std::sin(elapsed_seconds * 2.0));

    for (std::uint32_t y = 0; y < height_; ++y) {
        for (std::uint32_t x = 0; x < width_; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width_ + x) * 4U;
            const bool in_square = x >= square_x && x < square_x + square_size &&
                                   y >= square_y && y < square_y + square_size;
            if (in_square) {
                pixels_[offset + 0U] = 238U;
                pixels_[offset + 1U] = pulse;
                pixels_[offset + 2U] = 70U;
            } else {
                pixels_[offset + 0U] = static_cast<std::uint8_t>(12U + (x * 24U / std::max(width_, 1U)));
                pixels_[offset + 1U] = static_cast<std::uint8_t>(18U + (y * 28U / std::max(height_, 1U)));
                pixels_[offset + 2U] = 35U;
            }
            pixels_[offset + 3U] = 255U;
        }
    }
}

FrameView SoftwareRenderer::frame() const {
    return FrameView{width_, height_, pixels_};
}

bool SoftwareRenderer::capture_bmp(const std::filesystem::path& path, std::string& error) const {
    constexpr std::uint32_t header_size = 54U;
    const std::uint64_t pixel_bytes_64 = static_cast<std::uint64_t>(width_) * height_ * 4U;
    if (pixel_bytes_64 > std::numeric_limits<std::uint32_t>::max() - header_size) {
        error = "frame is too large for the BMP format";
        return false;
    }

    if (path.has_parent_path()) {
        std::error_code filesystem_error;
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "could not create capture directory: " + filesystem_error.message();
            return false;
        }
    }

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        error = "could not open capture path";
        return false;
    }

    const auto pixel_bytes = static_cast<std::uint32_t>(pixel_bytes_64);
    stream.put('B');
    stream.put('M');
    write_u32(stream, header_size + pixel_bytes);
    write_u16(stream, 0U);
    write_u16(stream, 0U);
    write_u32(stream, header_size);
    write_u32(stream, 40U);
    write_u32(stream, width_);
    write_u32(stream, height_);
    write_u16(stream, 1U);
    write_u16(stream, 32U);
    write_u32(stream, 0U);
    write_u32(stream, pixel_bytes);
    write_u32(stream, 2835U);
    write_u32(stream, 2835U);
    write_u32(stream, 0U);
    write_u32(stream, 0U);

    for (std::uint32_t row = height_; row > 0; --row) {
        const auto y = row - 1U;
        for (std::uint32_t x = 0; x < width_; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width_ + x) * 4U;
            const std::array pixel{
                static_cast<char>(pixels_[offset + 2U]),
                static_cast<char>(pixels_[offset + 1U]),
                static_cast<char>(pixels_[offset + 0U]),
                static_cast<char>(pixels_[offset + 3U]),
            };
            stream.write(pixel.data(), static_cast<std::streamsize>(pixel.size()));
        }
    }

    if (!stream) {
        error = "capture write failed";
        return false;
    }
    return true;
}

} // namespace relay

