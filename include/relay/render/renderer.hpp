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

// The deterministic CPU frame: a pure function of the frame index and elapsed time. Rendering only
// records them; the pixels are drawn when a reader asks for the frame, so game steps that nobody
// captures cost nothing. Not thread-safe: read frames on the thread that renders.
class SoftwareRenderer {
public:
    SoftwareRenderer(std::uint32_t width, std::uint32_t height);

    void render(std::uint64_t frame_index, double elapsed_seconds);
    [[nodiscard]] FrameView frame() const;
    [[nodiscard]] bool capture_bmp(const std::filesystem::path& path, std::string& error) const;

private:
    void draw() const;

    std::uint32_t width_;
    std::uint32_t height_;
    std::uint64_t frame_index_{};
    double elapsed_seconds_{};
    mutable bool stale_{true};
    mutable std::vector<std::uint8_t> pixels_;
};

} // namespace relay

