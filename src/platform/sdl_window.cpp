#include "relay/platform/sdl_window.hpp"

#include "relay/render/renderer.hpp"

#include <SDL3/SDL.h>

#include <utility>
#include <vector>

namespace relay {

struct SdlWindow::Impl {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    std::uint32_t texture_width{};
    std::uint32_t texture_height{};
    std::string error;
    std::vector<std::string> pending_input_events;
};

SdlWindow::SdlWindow(std::string title, const std::uint32_t width, const std::uint32_t height)
    : impl_(std::make_unique<Impl>()) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        impl_->error = SDL_GetError();
        return;
    }
    impl_->window = SDL_CreateWindow(title.c_str(), static_cast<int>(width), static_cast<int>(height),
                                     SDL_WINDOW_RESIZABLE);
    if (impl_->window == nullptr) {
        impl_->error = SDL_GetError();
        return;
    }
    impl_->renderer = SDL_CreateRenderer(impl_->window, nullptr);
    if (impl_->renderer == nullptr) {
        impl_->error = SDL_GetError();
    }
}

SdlWindow::~SdlWindow() {
    if (!impl_) return;
    SDL_DestroyTexture(impl_->texture);
    SDL_DestroyRenderer(impl_->renderer);
    SDL_DestroyWindow(impl_->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}

SdlWindow::SdlWindow(SdlWindow&&) noexcept = default;
SdlWindow& SdlWindow::operator=(SdlWindow&&) noexcept = default;

bool SdlWindow::valid() const {
    return impl_ && impl_->window != nullptr && impl_->renderer != nullptr;
}

std::string SdlWindow::error() const {
    return impl_ ? impl_->error : "window has no implementation";
}

bool SdlWindow::poll_quit() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
            impl_->pending_input_events.push_back(
                std::string{"key:"} + (event.type == SDL_EVENT_KEY_DOWN ? "down:" : "up:") +
                std::to_string(static_cast<std::int64_t>(event.key.key)));
        } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
            impl_->pending_input_events.push_back("mouse_motion:" + std::to_string(event.motion.x) + ':' +
                                                  std::to_string(event.motion.y));
        } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
            impl_->pending_input_events.push_back("gamepad_axis:" + std::to_string(event.gaxis.which) + ':' +
                                                  std::to_string(event.gaxis.axis) + ':' +
                                                  std::to_string(event.gaxis.value));
        } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                   event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            impl_->pending_input_events.push_back(
                std::string{"gamepad_button:"} +
                (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down:" : "up:") +
                std::to_string(event.gbutton.which) + ':' + std::to_string(event.gbutton.button));
        }
        if (event.type == SDL_EVENT_QUIT) return true;
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) return true;
    }
    return false;
}

std::vector<std::string> SdlWindow::drain_input_events() {
    if (!impl_) return {};
    auto events = std::move(impl_->pending_input_events);
    impl_->pending_input_events.clear();
    return events;
}

void SdlWindow::present(const FrameView& frame) {
    if (!valid()) return;
    if (impl_->texture == nullptr || impl_->texture_width != frame.width ||
        impl_->texture_height != frame.height) {
        SDL_DestroyTexture(impl_->texture);
        impl_->texture = SDL_CreateTexture(impl_->renderer, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           static_cast<int>(frame.width), static_cast<int>(frame.height));
        impl_->texture_width = frame.width;
        impl_->texture_height = frame.height;
    }
    if (impl_->texture == nullptr) return;
    SDL_UpdateTexture(impl_->texture, nullptr, frame.rgba.data(), static_cast<int>(frame.width * 4U));
    SDL_RenderClear(impl_->renderer);
    SDL_RenderTexture(impl_->renderer, impl_->texture, nullptr, nullptr);
    SDL_RenderPresent(impl_->renderer);
}

} // namespace relay
