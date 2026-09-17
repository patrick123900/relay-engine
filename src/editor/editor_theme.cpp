#include "relay/editor/editor_theme.hpp"

#include <array>
#include <filesystem>

namespace relay {
namespace {

constexpr ImU32 rgb(const unsigned red, const unsigned green, const unsigned blue,
                    const unsigned alpha = 255U) {
    return IM_COL32(red, green, blue, alpha);
}

// Charcoal chrome based on HTML #202020, with brighter blue selections.
// The sRGB swapchain requires linear values; packed ImGui colours round the base to 4/255.
constexpr EditorPalette palette{
    .window = rgb(2, 2, 2),
    .panel = rgb(4, 4, 4),
    .input = rgb(2, 2, 2),
    .surface = rgb(5, 5, 5),
    .surface_hovered = rgb(7, 7, 7),
    .surface_active = rgb(10, 10, 10),

    .border = rgb(7, 7, 7),
    .border_soft = rgb(5, 5, 5),

    .text = rgb(221, 226, 233),
    .text_dim = rgb(150, 162, 179),
    .text_faint = rgb(119, 133, 151),

    .accent = rgb(0x0F, 0x23, 0x57),
    .accent_hovered = rgb(0x16, 0x30, 0x6D),
    .accent_active = rgb(0x09, 0x18, 0x3C),
    .accent_soft = rgb(0x0F, 0x23, 0x57, 0x38),

    .success = rgb(0x85, 0xCB, 0xA4),
    .danger = rgb(0xDE, 0x84, 0x83),
    .warning = rgb(0xCF, 0xA8, 0x5A),
};

// Preference order, best-looking first. Each entry is tried until one exists on this system.
constexpr std::array<const char*, 8> proportional_candidates{
    "/usr/share/fonts/inter/Inter-Regular.ttf",
    "/usr/share/fonts/TTF/Inter-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/FiraSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
};

constexpr std::array<const char*, 8> medium_candidates{
    "/usr/share/fonts/inter/Inter-SemiBold.ttf",
    "/usr/share/fonts/TTF/Inter-SemiBold.ttf",
    "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
    "/usr/share/fonts/noto/NotoSans-Medium.ttf",
    "/usr/share/fonts/TTF/FiraSans-Medium.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "C:/Windows/Fonts/segoeuisb.ttf",
};

constexpr std::array<const char*, 7> monospace_candidates{
    "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/FiraMono-Regular.ttf",
    "C:/Windows/Fonts/consola.ttf",
};

template <std::size_t Count>
const char* first_existing(const std::array<const char*, Count>& candidates) {
    for (const auto* candidate : candidates) {
        std::error_code failure;
        if (std::filesystem::exists(candidate, failure) && !failure) return candidate;
    }
    return nullptr;
}

ImFont* load_face(const char* const path, const float size) {
    if (path == nullptr) return nullptr;
    ImFontConfig configuration;
    configuration.OversampleH = 2;
    configuration.OversampleV = 1;
    configuration.PixelSnapH = false;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path, size, &configuration);
}

} // namespace

const EditorPalette& editor_palette() { return palette; }

ImVec4 editor_color(const ImU32 packed) { return ImGui::ColorConvertU32ToFloat4(packed); }

EditorFonts load_editor_fonts(const float scale) {
    EditorFonts fonts;
    fonts.body_size = 17.0F * scale;
    fonts.heading_size = 17.0F * scale;
    fonts.monospace_size = 16.0F * scale;

    fonts.body = load_face(first_existing(proportional_candidates), fonts.body_size);
    fonts.heading = load_face(first_existing(medium_candidates), fonts.heading_size);
    fonts.monospace = load_face(first_existing(monospace_candidates), fonts.monospace_size);

    auto& io = ImGui::GetIO();
    if (fonts.body == nullptr) {
        // No system face was found. The built-in bitmap font is ugly but keeps the editor usable.
        ImFontConfig configuration;
        configuration.SizePixels = fonts.body_size;
        fonts.body = io.Fonts->AddFontDefault(&configuration);
    }
    if (fonts.heading == nullptr) fonts.heading = fonts.body;
    if (fonts.monospace == nullptr) fonts.monospace = fonts.body;
    io.FontDefault = fonts.body;
    return fonts;
}

void apply_editor_theme(const float scale) {
    auto& style = ImGui::GetStyle();
    style = ImGuiStyle{};
    style.WindowMenuButtonPosition = ImGuiDir_Right;

    style.WindowRounding = 7.0F;
    style.ChildRounding = 7.0F;
    style.FrameRounding = 5.0F;
    style.PopupRounding = 7.0F;
    style.ScrollbarRounding = 9.0F;
    style.GrabRounding = 4.0F;
    style.TabRounding = 6.0F;

    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.SeparatorTextBorderSize = 1.0F;

    style.WindowPadding = ImVec2(12.0F, 10.0F);
    style.FramePadding = ImVec2(10.0F, 5.0F);
    style.ItemSpacing = ImVec2(8.0F, 7.0F);
    style.ItemInnerSpacing = ImVec2(7.0F, 5.0F);
    style.CellPadding = ImVec2(8.0F, 5.0F);
    style.IndentSpacing = 18.0F;
    style.ScrollbarSize = 11.0F;
    style.GrabMinSize = 10.0F;

    style.WindowTitleAlign = ImVec2(0.0F, 0.5F);
    style.ButtonTextAlign = ImVec2(0.5F, 0.5F);
    style.SelectableTextAlign = ImVec2(0.0F, 0.5F);
    style.SeparatorTextAlign = ImVec2(0.0F, 0.5F);
    style.SeparatorTextPadding = ImVec2(16.0F, 6.0F);

    // Rounded corners look wrong when the curve is approximated too coarsely at this size.
    style.CircleTessellationMaxError = 0.18F;
    style.CurveTessellationTol = 1.0F;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;

    auto* colors = style.Colors;
    const auto set = [colors](const ImGuiCol_ slot, const ImU32 packed) {
        colors[slot] = ImGui::ColorConvertU32ToFloat4(packed);
    };
    const auto set_alpha = [colors](const ImGuiCol_ slot, const ImU32 packed, const float alpha) {
        auto color = ImGui::ColorConvertU32ToFloat4(packed);
        color.w = alpha;
        colors[slot] = color;
    };

    set(ImGuiCol_Text, palette.text);
    set(ImGuiCol_TextDisabled, palette.text_dim);
    set(ImGuiCol_WindowBg, palette.panel);
    set(ImGuiCol_ChildBg, palette.window);
    set(ImGuiCol_PopupBg, palette.panel);
    set(ImGuiCol_Border, palette.border);
    set_alpha(ImGuiCol_BorderShadow, palette.window, 0.0F);

    set(ImGuiCol_FrameBg, palette.input);
    set(ImGuiCol_FrameBgHovered, palette.surface);
    set(ImGuiCol_FrameBgActive, palette.surface_hovered);

    set(ImGuiCol_TitleBg, palette.panel);
    set(ImGuiCol_TitleBgActive, palette.panel);
    set(ImGuiCol_TitleBgCollapsed, palette.panel);
    set(ImGuiCol_MenuBarBg, palette.panel);
    set_alpha(ImGuiCol_DockingPreview, palette.accent, 0.55F);
    set(ImGuiCol_DockingEmptyBg, palette.window);

    set_alpha(ImGuiCol_ScrollbarBg, palette.window, 0.0F);
    set(ImGuiCol_ScrollbarGrab, palette.surface);
    set(ImGuiCol_ScrollbarGrabHovered, palette.surface_hovered);
    set(ImGuiCol_ScrollbarGrabActive, palette.surface_active);

    set(ImGuiCol_CheckMark, palette.accent);
    set(ImGuiCol_SliderGrab, palette.accent);
    set(ImGuiCol_SliderGrabActive, palette.accent_hovered);

    set(ImGuiCol_Button, palette.surface);
    set(ImGuiCol_ButtonHovered, palette.surface_hovered);
    set(ImGuiCol_ButtonActive, palette.surface_active);

    set(ImGuiCol_Header, palette.accent_soft);
    set_alpha(ImGuiCol_HeaderHovered, palette.accent, 0.30F);
    set_alpha(ImGuiCol_HeaderActive, palette.accent, 0.45F);

    set(ImGuiCol_Separator, palette.border_soft);
    set_alpha(ImGuiCol_SeparatorHovered, palette.accent, 0.60F);
    set(ImGuiCol_SeparatorActive, palette.accent);

    set_alpha(ImGuiCol_ResizeGrip, palette.accent, 0.0F);
    set_alpha(ImGuiCol_ResizeGripHovered, palette.accent, 0.35F);
    set_alpha(ImGuiCol_ResizeGripActive, palette.accent, 0.60F);

    set(ImGuiCol_Tab, palette.panel);
    set(ImGuiCol_TabHovered, palette.surface_hovered);
    set(ImGuiCol_TabSelected, palette.surface);
    set(ImGuiCol_TabSelectedOverline, palette.accent);
    set(ImGuiCol_TabDimmed, palette.panel);
    set(ImGuiCol_TabDimmedSelected, palette.surface);

    set(ImGuiCol_PlotLines, palette.accent);
    set(ImGuiCol_PlotLinesHovered, palette.accent_hovered);
    set(ImGuiCol_PlotHistogram, palette.accent);
    set(ImGuiCol_PlotHistogramHovered, palette.accent_hovered);

    set(ImGuiCol_TableHeaderBg, palette.surface);
    set(ImGuiCol_TableBorderStrong, palette.border);
    set(ImGuiCol_TableBorderLight, palette.border_soft);
    set_alpha(ImGuiCol_TableRowBg, palette.window, 0.0F);
    set_alpha(ImGuiCol_TableRowBgAlt, palette.surface, 0.22F);

    set_alpha(ImGuiCol_TextSelectedBg, palette.accent, 0.35F);
    set(ImGuiCol_DragDropTarget, palette.accent_hovered);
    set(ImGuiCol_NavCursor, palette.accent);
    set_alpha(ImGuiCol_NavWindowingHighlight, palette.accent, 0.70F);
    set_alpha(ImGuiCol_NavWindowingDimBg, palette.window, 0.55F);
    set_alpha(ImGuiCol_ModalWindowDimBg, palette.window, 0.65F);

    if (scale != 1.0F) style.ScaleAllSizes(scale);
}

} // namespace relay
