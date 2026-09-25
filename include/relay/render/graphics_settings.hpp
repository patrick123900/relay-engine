#pragma once

#include "relay/core/json.hpp"

#include <cstdint>
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
    // Frames per second while the game runs; 0 leaves the rate to presentation.
    std::uint32_t frame_rate_limit{0};
    // Present in step with the display. Off presents each frame as soon as it is ready, which is
    // faster and lower latency but can tear.
    bool vsync{false};

    friend bool operator==(const GraphicsSettings&, const GraphicsSettings&) = default;
};

inline constexpr std::uint32_t maximum_frame_rate_limit = 1000U;

[[nodiscard]] std::string graphics_settings_json(const GraphicsSettings& settings);
// Reads settings.graphics; unknown fields are rejected so typos do not silently do nothing.
[[nodiscard]] std::optional<GraphicsSettings> parse_graphics_settings(const JsonValue& value,
                                                                      std::string& error);

} // namespace relay
