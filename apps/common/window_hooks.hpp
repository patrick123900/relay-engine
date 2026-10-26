#pragma once

// Hooks that connect the engine to a VulkanWindow, shared by the editor and the built game's
// player so both draw the project the same way.

#include "relay/core/engine.hpp"
#include "relay/platform/vulkan_window.hpp"
#include "relay/particles/particle_system.hpp"
#include "relay/ui/ui_system.hpp"

#include <cstdlib>
#include <string_view>

namespace relay_apps {

// Applies the project's graphics settings to the renderer. RELAY_GLOBAL_ILLUMINATION,
// RELAY_REFLECTIONS and RELAY_VSYNC set to 0 or 1 override them, for comparisons and smoke runs.
inline void apply_graphics_settings(relay::VulkanWindow& window, const relay::Engine& engine) {
    bool global_illumination = engine.graphics_settings().global_illumination;
    if (const char* value = std::getenv("RELAY_GLOBAL_ILLUMINATION")) {
        if (std::string_view(value) == "0") global_illumination = false;
        if (std::string_view(value) == "1") global_illumination = true;
    }
    window.set_global_illumination(global_illumination);
    bool reflections = engine.graphics_settings().reflections;
    if (const char* value = std::getenv("RELAY_REFLECTIONS")) {
        if (std::string_view(value) == "0") reflections = false;
        if (std::string_view(value) == "1") reflections = true;
    }
    window.set_reflections(reflections);
    bool vsync = engine.graphics_settings().vsync;
    if (const char* value = std::getenv("RELAY_VSYNC")) {
        if (std::string_view(value) == "0") vsync = false;
        if (std::string_view(value) == "1") vsync = true;
    }
    window.set_vsync(vsync);
}

// Draws the game interface over the game view, and only while the game runs: the editor's own
// camera never shows it. Files the interface could not use are reported in the log.
inline void connect_game_ui(relay::VulkanWindow& window, relay::Engine& engine) {
    window.set_game_ui_source([&engine](std::uint32_t width, std::uint32_t height) -> const relay::UiDrawList* {
        if (engine.status().mode != relay::RuntimeMode::game) return nullptr;
        engine.ui().set_view_size(width, height);
        const auto& list = engine.ui().draw(engine.scene(), width, height);
        for (auto& warning : engine.ui().painter().take_warnings())
            engine.logs().write(relay::LogLevel::warning, "Interface: " + warning);
        return &list;
    });
}

// Draws the scene's particles in every view. `alpha`, when given, is the share of a game step
// since the latest one, so particles move smoothly between steps; texture files the particles
// could not use are reported in the log.
inline void connect_particles(relay::VulkanWindow& window, relay::Engine& engine, const double* alpha = nullptr) {
    window.set_particle_source([&engine, alpha](const relay::ParticleView& view) -> const relay::ParticleRenderList* {
        const auto& list = engine.particle_render_list(view, alpha ? *alpha : 1.0);
        for (auto& warning : engine.particles().take_warnings())
            engine.logs().write(relay::LogLevel::warning, "Particles: " + warning);
        return &list;
    });
}

} // namespace relay_apps
