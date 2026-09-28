#pragma once

// Draws the game interface inside an editor panel through Dear ImGui, for the Interface editor:
// the same draw list the game's window draws, with its textures kept as ImGui textures.

#include "relay/ui/ui_render.hpp"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace relay {

class InterfacePreview {
public:
    InterfacePreview() = default;
    ~InterfacePreview();
    InterfacePreview(const InterfacePreview&) = delete;
    InterfacePreview& operator=(const InterfacePreview&) = delete;

    // Once per ImGui frame, before draw(). Headless editors have no renderer, so their textures
    // are ready at once.
    void begin_frame(bool headless);
    // Draws `list` with the view's top-left at `origin` and `scale` panel pixels per view pixel,
    // clipped to the panel rectangle `clip_min` to `clip_max`.
    void draw(ImDrawList* target, const UiDrawList& list, ImVec2 origin, float scale, ImVec2 clip_min,
              ImVec2 clip_max);
    void clear();

private:
    struct Texture {
        std::unique_ptr<ImTextureData> data;
        std::uint64_t revision{};
        int unused{};
    };
    ImTextureData* texture(const UiTexture& source);
    void retire(std::unique_ptr<ImTextureData> texture);

    bool headless_{};
    std::unordered_map<std::uint64_t, Texture> textures_;
    std::vector<std::unique_ptr<ImTextureData>> retired_;
};

} // namespace relay
