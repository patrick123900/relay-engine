#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace relay {

class Engine;

class ControlProtocol {
public:
    using CaptureHandler = std::function<bool(const std::filesystem::path&, std::string&)>;
    using RenderInspectionHandler = std::function<std::string(std::string_view)>;

    explicit ControlProtocol(Engine& engine, CaptureHandler capture_handler = {},
                             RenderInspectionHandler render_inspection_handler = {});

    [[nodiscard]] std::string handle(std::string_view request);

private:
    Engine& engine_;
    CaptureHandler capture_handler_;
    RenderInspectionHandler render_inspection_handler_;
};

} // namespace relay
