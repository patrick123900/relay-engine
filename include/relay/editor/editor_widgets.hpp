#pragma once

// Small custom controls shared by every editor panel: a compact checkbox, a slider with a track and
// a round knob, and one icon per kind of asset. They follow the theme in editor_theme.hpp.

#include <imgui.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Like ImGui::Checkbox, with a small box centred in a normal field's height, so rows of
// checkboxes line up with other fields.
bool editor_checkbox(const char* label, bool* value);

// A horizontal slider: a thin track filled up to the value, a round knob, and the value in a small
// box on the right. Click or drag the track to set the value; click the value box, double-click or
// Ctrl+click to type one. Works with IsItemActive/IsItemActivated/IsItemDeactivatedAfterEdit like
// ImGui's sliders. `format` is a printf format for one double (see slider_format); logarithmic
// needs a positive minimum.
bool editor_slider(const char* label, double* value, double minimum, double maximum, const char* format,
                   bool logarithmic = false);
bool editor_slider(const char* label, float* value, float minimum, float maximum, const char* format,
                   bool logarithmic = false);

// A printf format with as many decimals as a range needs: steps decide when given, otherwise
// ranges up to 10 get two decimals, up to 100 one and wider ranges none. `suffix` follows the number.
[[nodiscard]] std::string slider_format(double minimum, double maximum, double step = 0.0,
                                        std::string_view suffix = {});

enum class AssetIcon : std::uint8_t {
    folder, model, mesh, scene, node_template, image, material, sky_material, post_material, shader,
    script, text, audio, video, font, node, other
};

// The icon for an asset kind as the protocol names it ("model", "material" and so on). Materials
// are all "material"; pass a surface/post_process/sky `material_type` to tell them apart.
[[nodiscard]] AssetIcon asset_icon_for_kind(std::string_view kind, std::string_view material_type = {});
// The icon for a file name, from its extension.
[[nodiscard]] AssetIcon asset_icon_for_file(std::string_view name);
// A short name for the kind of asset, for tooltips ("Material", "Sound").
[[nodiscard]] const char* asset_icon_label(AssetIcon icon);
// The icon's own colour (linear, like the palette).
[[nodiscard]] ImU32 asset_icon_color(AssetIcon icon, float alpha = 1.0F);
// Draws the icon inside the square at `min` with side `size`.
void draw_asset_type_icon(ImDrawList* list, ImVec2 min, float size, AssetIcon icon, float alpha = 1.0F);

// A file name without its folders or extensions ("materials/Rock.relay-material" -> "Rock").
// Relay's double extensions (.relay-material, .relay-template.json, .relay.json) go too.
[[nodiscard]] std::string asset_display_name(std::string_view path);

// Converts 8-bit sRGB RGBA rows in place to the linear values ImGui textures need on an sRGB
// swapchain, which would otherwise show them too bright.
void srgb_rows_to_linear(std::vector<std::uint8_t>& rgba);

} // namespace relay
