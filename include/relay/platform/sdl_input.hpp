#pragma once

#include <SDL3/SDL.h>

#include <optional>
#include <string>

namespace relay {

// Translates an SDL event into the engine's input event text (see InputState::apply), naming keys
// by physical position so bindings do not depend on the keyboard layout. Key repeats are dropped.
[[nodiscard]] std::optional<std::string> sdl_input_event(const SDL_Event& event);
// The control a binding capture should record for this event: a key, mouse button or gamepad
// button going down, or a gamepad stick or trigger pushed past half travel.
[[nodiscard]] std::optional<std::string> sdl_binding_control(const SDL_Event& event);
// Opens gamepads as they connect and closes them as they go; SDL sends no gamepad input otherwise.
void sdl_track_gamepads(const SDL_Event& event);

} // namespace relay
