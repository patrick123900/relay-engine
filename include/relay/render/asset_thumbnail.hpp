#pragma once

// Small preview pictures of project files for the editor's asset browser. Both functions read
// files only inside `root`, touch no shared engine state and are safe to call on worker threads.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

struct ThumbnailImage {
    std::uint32_t width{};
    std::uint32_t height{};
    // Straight (not premultiplied) 8-bit sRGB RGBA rows.
    std::vector<std::uint8_t> rgba;
};

// Decodes a PNG or JPEG and shrinks it, averaging pixels, to fit within `size` square, keeping its
// aspect ratio. Files over 128 MiB are refused.
[[nodiscard]] bool image_thumbnail(const std::filesystem::path& root, std::string_view filename,
                                   std::uint32_t size, ThumbnailImage& output, std::string& error);

// Imports a model into a private registry and scene and draws it from above and to the front on a
// transparent background, lit by one soft key light, `size` pixels square. Bind poses are used for
// skinned meshes; base colors and color textures are shown, other material inputs are not. With
// `only_mesh` (an imported mesh's registry name, matched by its index in the file) just the first
// node drawing that mesh is shown.
[[nodiscard]] bool model_thumbnail(const std::filesystem::path& root, std::string_view filename,
                                   std::uint32_t size, ThumbnailImage& output, std::string& error,
                                   std::string_view only_mesh = {});

} // namespace relay
