#pragma once

#include "relay/core/json.hpp"

#include <optional>
#include <string>

namespace relay {

// Per-project rendering options, saved in the project file under settings.graphics. The renderer
// uses an effect only when the GPU and build support it and otherwise keeps its fallback.
struct GraphicsSettings {
    // FidelityFX Brixelizer GI: diffuse and specular indirect light instead of the analytic sky.
    bool global_illumination{true};
    // Hardware ray traced reflections on smooth surfaces.
    bool reflections{true};

    friend bool operator==(const GraphicsSettings&, const GraphicsSettings&) = default;
};

[[nodiscard]] std::string graphics_settings_json(const GraphicsSettings& settings);
// Reads settings.graphics; unknown fields are rejected so typos do not silently do nothing.
[[nodiscard]] std::optional<GraphicsSettings> parse_graphics_settings(const JsonValue& value,
                                                                      std::string& error);

} // namespace relay
