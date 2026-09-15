#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace relay {

struct FrameView {
    std::uint32_t width{};
    std::uint32_t height{};
    std::span<const std::uint8_t> rgba;
};

class SoftwareRenderer {
public:
    SoftwareRenderer(std::uint32_t width, std::uint32_t height);

    void render(std::uint64_t frame_index, double elapsed_seconds);
    [[nodiscard]] FrameView frame() const;
    [[nodiscard]] bool capture_bmp(const std::filesystem::path& path, std::string& error) const;

private:
    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> pixels_;
};

} // namespace relay

