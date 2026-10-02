#pragma once

struct SDL_Window;

namespace relay {

// Gives a window Relay's mark as its icon (title bar, taskbar, task switcher), from the PNG built
// into the engine. Harmless when the icon cannot be decoded or the platform ignores window icons.
void apply_window_icon(SDL_Window* window);

} // namespace relay
