#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace relay {

struct FrameView;

class SdlWindow {
public:
    SdlWindow(std::string title, std::uint32_t width, std::uint32_t height);
    ~SdlWindow();

    SdlWindow(const SdlWindow&) = delete;
    SdlWindow& operator=(const SdlWindow&) = delete;
    SdlWindow(SdlWindow&&) noexcept;
    SdlWindow& operator=(SdlWindow&&) noexcept;

    [[nodiscard]] bool valid() const;
    [[nodiscard]] std::string error() const;
    [[nodiscard]] bool poll_quit();
    [[nodiscard]] std::vector<std::string> drain_input_events();
    void present(const FrameView& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
