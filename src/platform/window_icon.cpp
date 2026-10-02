#include "relay/platform/window_icon.hpp"

#include "relay/render/image_decode.hpp"
#include "relay/ui/ui_font.hpp"

#include <SDL3/SDL.h>

#include <string>

namespace relay {

void apply_window_icon(SDL_Window* window) {
    if (window == nullptr) return;
    for (const auto& file : embedded_files()) {
        if (file.name != "relay-icon.png") continue;
        TextureAsset icon;
        std::string error;
        if (!decode_image_rgba(file.bytes, "png", icon, error) || icon.width == 0U || icon.height == 0U) return;
        SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(icon.width), static_cast<int>(icon.height),
                                                     SDL_PIXELFORMAT_RGBA32, icon.rgba.data(),
                                                     static_cast<int>(icon.width * 4U));
        if (surface == nullptr) return;
        (void)SDL_SetWindowIcon(window, surface); // SDL copies the pixels.
        SDL_DestroySurface(surface);
        return;
    }
}

} // namespace relay
