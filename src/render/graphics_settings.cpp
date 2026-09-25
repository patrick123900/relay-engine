#include "relay/render/graphics_settings.hpp"

#include <cmath>

namespace relay {

std::string graphics_settings_json(const GraphicsSettings& settings) {
    return std::string{"{\"global_illumination\":"} +
           (settings.global_illumination ? "true" : "false") + ",\"reflections\":" +
           (settings.reflections ? "true" : "false") +
           ",\"frame_rate_limit\":" + std::to_string(settings.frame_rate_limit) +
           ",\"vsync\":" + (settings.vsync ? "true" : "false") + '}';
}

std::optional<GraphicsSettings> parse_graphics_settings(const JsonValue& value,
                                                        std::string& error) {
    const auto* object = value.object();
    if (object == nullptr) {
        error = "graphics settings must be an object";
        return std::nullopt;
    }
    GraphicsSettings settings;
    for (const auto& [name, entry] : *object) {
        if (name == "frame_rate_limit") {
            const auto* number = entry.number();
            if (number == nullptr || *number < 0.0 || *number > maximum_frame_rate_limit ||
                std::floor(*number) != *number) {
                error = "graphics setting frame_rate_limit must be a whole number from 0 to " +
                        std::to_string(maximum_frame_rate_limit);
                return std::nullopt;
            }
            settings.frame_rate_limit = static_cast<std::uint32_t>(*number);
            continue;
        }
        bool* target = name == "global_illumination" ? &settings.global_illumination
                       : name == "reflections"       ? &settings.reflections
                       : name == "vsync"             ? &settings.vsync
                                                     : nullptr;
        if (target == nullptr) {
            error = "unknown graphics setting " + name;
            return std::nullopt;
        }
        if (entry.boolean() == nullptr) {
            error = "graphics setting " + name + " must be true or false";
            return std::nullopt;
        }
        *target = *entry.boolean();
    }
    return settings;
}

} // namespace relay
