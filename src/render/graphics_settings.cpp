#include "relay/render/graphics_settings.hpp"

namespace relay {

std::string graphics_settings_json(const GraphicsSettings& settings) {
    return std::string{"{\"global_illumination\":"} +
           (settings.global_illumination ? "true" : "false") + ",\"reflections\":" +
           (settings.reflections ? "true" : "false") + '}';
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
        bool* target = name == "global_illumination" ? &settings.global_illumination
                       : name == "reflections"       ? &settings.reflections
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
