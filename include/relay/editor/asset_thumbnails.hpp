#pragma once

// Thumbnail textures for the asset browser. Images and models are drawn on worker threads (at
// most two at once, visible tiles first); surface materials are rendered by the window's material
// preview, one per frame, and handed back through store_material. Thumbnails are redrawn when their
// files change and the least recently shown are released beyond a few hundred.

#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace relay {

class AssetThumbnails {
public:
    enum class Source : std::uint8_t { image, model };

    AssetThumbnails();
    ~AssetThumbnails();
    AssetThumbnails(const AssetThumbnails&) = delete;
    AssetThumbnails& operator=(const AssetThumbnails&) = delete;

    // Call once per ImGui frame before drawing thumbnails. Headless editors have no renderer, so
    // their textures are marked ready at once.
    void begin_frame(bool headless);
    // The project folder paths are relative to. Changing it drops every thumbnail.
    void set_root(const std::filesystem::path& root);

    // A project image or model's thumbnail, or null while it is drawn or when it cannot be.
    [[nodiscard]] ImTextureData* file(const std::string& path, Source source);
    // One imported mesh of a model file (its registry name), drawn alone.
    [[nodiscard]] ImTextureData* mesh(const std::string& model, const std::string& mesh);
    // A surface material's thumbnail. `shader` is the material's shader file, so edits to either
    // redraw it. Null until the renderer has drawn it.
    [[nodiscard]] ImTextureData* material(const std::string& path, const std::string& shader);
    // The material the renderer should draw next for a thumbnail, or empty.
    [[nodiscard]] std::string material_request() const;
    // A rendered material preview (8-bit sRGB RGBA). Ignored unless a thumbnail wants it.
    void store_material(const std::string& path, std::uint32_t width, std::uint32_t height,
                        std::vector<std::uint8_t> rgba);
    // True when the last attempt to draw this file failed (it shows its icon instead).
    [[nodiscard]] bool failed(const std::string& path) const;
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
