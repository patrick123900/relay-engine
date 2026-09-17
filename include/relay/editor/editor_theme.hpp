#pragma once

// Visual language for the editor: one palette, one set of metrics, one place to change them.
//
// Keeping the colours named and centralized means panels ask for "surface" or "accent" rather than
// inventing literals, so the interface stays coherent as panels are added.

#include <imgui.h>

namespace relay {

struct EditorPalette {
    // Backgrounds, darkest to lightest.
    ImU32 window;
    ImU32 panel;
    ImU32 input;
    ImU32 surface;
    ImU32 surface_hovered;
    ImU32 surface_active;

    ImU32 border;
    ImU32 border_soft;

    ImU32 text;
    ImU32 text_dim;
    ImU32 text_faint;

    ImU32 accent;
    ImU32 accent_hovered;
    ImU32 accent_active;
    ImU32 accent_soft;

    ImU32 success;
    ImU32 danger;
    ImU32 warning;
};

[[nodiscard]] const EditorPalette& editor_palette();
[[nodiscard]] ImVec4 editor_color(ImU32 packed);

struct EditorFonts {
    ImFont* body{};
    ImFont* heading{};
    ImFont* monospace{};
    float body_size{17.0F};
    float heading_size{17.0F};
    float monospace_size{16.0F};
};

// Loads proportional and monospaced faces from the system. Falls back to the built-in bitmap font
// when none are found, so a bare container still renders.
[[nodiscard]] EditorFonts load_editor_fonts(float scale);

// Applies colours, rounding and spacing. `scale` multiplies metrics for high-density displays.
void apply_editor_theme(float scale);

} // namespace relay
