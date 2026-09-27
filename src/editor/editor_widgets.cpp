#include "relay/editor/editor_widgets.hpp"

#include "relay/editor/editor_theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace relay {
namespace {

// Packs an sRGB colour as the linear value the sRGB swapchain expects, like the palette's colours.
ImU32 srgb(const int red, const int green, const int blue, const float alpha = 1.0F) {
    const auto channel = [](const int value) {
        const float c = static_cast<float>(value) / 255.0F;
        const float linear = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
        return static_cast<int>(std::lround(linear * 255.0F));
    };
    return IM_COL32(channel(red), channel(green), channel(blue), static_cast<int>(std::lround(alpha * 255.0F)));
}

ImU32 with_alpha(const ImU32 color, const float alpha) {
    const auto base = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFU);
    return (color & ~IM_COL32_A_MASK) |
           (static_cast<ImU32>(std::lround(std::clamp(base * alpha, 0.0F, 255.0F))) << IM_COL32_A_SHIFT);
}

bool has_conversion(const char* format) {
    for (const char* at = format; *at; ++at)
        if (at[0] == '%' && at[1] != '%' && at[1] != '\0') return true;
    return false;
}

double to_position(const double value, const double minimum, const double maximum, const bool logarithmic) {
    if (maximum <= minimum) return 0.0;
    if (logarithmic && minimum > 0.0)
        return std::log(std::max(value, minimum) / minimum) / std::log(maximum / minimum);
    return (value - minimum) / (maximum - minimum);
}

double from_position(const double position, const double minimum, const double maximum, const bool logarithmic) {
    const double t = std::clamp(position, 0.0, 1.0);
    if (logarithmic && minimum > 0.0 && maximum > minimum) return minimum * std::pow(maximum / minimum, t);
    return minimum + (maximum - minimum) * t;
}

// Rounds to what the format shows, so a slider never holds more precision than it displays.
double round_to_format(const double value, const char* format) {
    if (!has_conversion(format)) return value;
    std::array<char, 64> text{};
    ImGui::DataTypeFormatString(text.data(), static_cast<int>(text.size()), ImGuiDataType_Double, &value, format);
    const char* start = text.data();
    while (*start && !(std::isdigit(static_cast<unsigned char>(*start)) || *start == '-' || *start == '.')) ++start;
    char* end = nullptr;
    const double parsed = std::strtod(start, &end);
    return end != start && std::isfinite(parsed) ? parsed : value;
}

} // namespace

bool editor_checkbox(const char* label, bool* value) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const float row = label_size.y + style.FramePadding.y * 2.0F;
    const float box = std::round(std::min(ImGui::GetFrameHeight(), g.FontSize * 0.86F));
    const ImVec2 position = window->DC.CursorPos;
    const ImRect total(position, ImVec2(position.x + box + (label_size.x > 0.0F ? style.ItemInnerSpacing.x + label_size.x : 0.0F),
                                        position.y + row));
    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ImGui::ButtonBehavior(total, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }
    const auto& palette = editor_palette();
    auto* draw = window->DrawList;
    const float top = std::round(position.y + (row - box) * 0.5F);
    const ImVec2 low(position.x, top), high(position.x + box, top + box);
    const float rounding = std::max(2.0F, box * 0.24F);
    ImGui::RenderNavCursor(total, id);
    if (*value) {
        draw->AddRectFilled(low, high, hovered ? palette.accent_hovered : palette.accent, rounding);
        const float pad = std::round(box * 0.2F);
        ImGui::RenderCheckMark(draw, ImVec2(low.x + pad, low.y + pad), palette.text, box - pad * 2.0F);
    } else {
        draw->AddRectFilled(low, high, held ? palette.surface_active : palette.input, rounding);
        draw->AddRect(low, high, with_alpha(palette.text_faint, hovered ? 0.85F : 0.5F), rounding, 0, 1.0F);
    }
    if (label_size.x > 0.0F)
        ImGui::RenderText(ImVec2(high.x + style.ItemInnerSpacing.x, position.y + style.FramePadding.y), label);
    return pressed;
}

bool editor_slider(const char* label, double* value, const double minimum, const double maximum, const char* format,
                   const bool logarithmic) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) return false;
    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const float width = ImGui::CalcItemWidth();
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 position = window->DC.CursorPos;
    const ImRect frame(position, ImVec2(position.x + width, position.y + ImGui::GetFrameHeight()));
    const ImRect total(frame.Min, ImVec2(frame.Max.x + (label_size.x > 0.0F ? style.ItemInnerSpacing.x + label_size.x : 0.0F),
                                         frame.Max.y));
    ImGui::ItemSize(total, style.FramePadding.y);
    if (!ImGui::ItemAdd(total, id, &frame, ImGuiItemFlags_Inputable)) return false;

    // The value box is as wide as the widest value the range can show, so it does not jump.
    const auto text_width = [&](const double number) {
        std::array<char, 64> text{};
        ImGui::DataTypeFormatString(text.data(), static_cast<int>(text.size()), ImGuiDataType_Double, &number, format);
        return ImGui::CalcTextSize(text.data()).x;
    };
    const float box_width = std::clamp(std::max({text_width(minimum), text_width(maximum), text_width(*value)}) +
                                           style.FramePadding.x * 1.4F,
                                       g.FontSize * 2.6F, std::max(g.FontSize * 2.6F, width * 0.45F));
    const ImRect box(ImVec2(frame.Max.x - box_width, frame.Min.y), frame.Max);
    const float knob = std::round(g.FontSize * 0.36F);
    const float track_left = frame.Min.x + knob, track_right = box.Min.x - style.ItemInnerSpacing.x - knob;

    const bool hovered = ImGui::ItemHoverable(frame, id, g.LastItemData.ItemFlags);
    bool typing = ImGui::TempInputIsActive(id);
    if (!typing) {
        const bool clicked = hovered && ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
        const bool double_clicked = hovered && g.IO.MouseClickedCount[0] == 2 && ImGui::TestKeyOwner(ImGuiKey_MouseLeft, id);
        const bool activate = clicked || double_clicked || g.NavActivateId == id;
        if (activate && (clicked || double_clicked)) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
        const bool on_box = g.IO.MousePos.x >= box.Min.x;
        if (activate && ((clicked && (g.IO.KeyCtrl || on_box)) || double_clicked ||
                         (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput))))
            typing = true;
        if (activate && !typing) {
            ImGui::SetActiveID(id, window);
            ImGui::SetFocusID(id, window);
            ImGui::FocusWindow(window);
            g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
        }
    }
    if (typing) return ImGui::TempInputScalar(frame, id, label, ImGuiDataType_Double, value, format, &minimum, &maximum);

    bool changed = false;
    const auto set = [&](const double next) {
        const double rounded = std::clamp(round_to_format(next, format), minimum, maximum);
        if (rounded != *value) {
            *value = rounded;
            changed = true;
        }
    };
    if (g.ActiveId == id) {
        if (g.ActiveIdSource == ImGuiInputSource_Mouse) {
            if (!g.IO.MouseDown[0]) {
                ImGui::ClearActiveID();
            } else if (track_right > track_left) {
                set(from_position((g.IO.MousePos.x - track_left) / (track_right - track_left), minimum, maximum,
                                  logarithmic));
            }
        } else {
            const float tweak = ImGui::GetNavTweakPressedAmount(ImGuiAxis_X);
            if (tweak != 0.0F)
                set(from_position(to_position(*value, minimum, maximum, logarithmic) + tweak * 0.01, minimum,
                                  maximum, logarithmic));
            if (g.NavActivatePressedId == id && !g.ActiveIdIsJustActivated) ImGui::ClearActiveID();
        }
    }
    if (changed) ImGui::MarkItemEdited(id);

    const auto& palette = editor_palette();
    auto* draw = window->DrawList;
    const bool active = g.ActiveId == id;
    const float middle = std::round((frame.Min.y + frame.Max.y) * 0.5F);
    const float thickness = std::max(3.0F, std::round(g.FontSize * 0.2F));
    const float t = static_cast<float>(std::clamp(to_position(*value, minimum, maximum, logarithmic), 0.0, 1.0));
    const float knob_x = track_left + (track_right - track_left) * t;
    ImGui::RenderNavCursor(frame, id);
    draw->AddRectFilled(ImVec2(track_left - knob * 0.5F, middle - thickness * 0.5F),
                        ImVec2(track_right + knob * 0.5F, middle + thickness * 0.5F), palette.surface_active, thickness);
    draw->AddRectFilled(ImVec2(track_left - knob * 0.5F, middle - thickness * 0.5F),
                        ImVec2(knob_x, middle + thickness * 0.5F),
                        hovered || active ? palette.accent_hovered : palette.accent, thickness);
    if (hovered || active) draw->AddCircleFilled(ImVec2(knob_x, middle), knob + 4.0F, palette.accent_soft, 24);
    draw->AddCircleFilled(ImVec2(knob_x, middle), knob, active ? palette.text : with_alpha(palette.text, 0.88F), 24);
    draw->AddCircle(ImVec2(knob_x, middle), knob, palette.accent, 24, 1.5F);

    const bool box_hovered = hovered && g.IO.MousePos.x >= box.Min.x && !active;
    draw->AddRectFilled(box.Min, box.Max, box_hovered ? palette.surface : palette.input, style.FrameRounding);
    std::array<char, 64> text{};
    const auto length = ImGui::DataTypeFormatString(text.data(), static_cast<int>(text.size()), ImGuiDataType_Double,
                                                    value, format);
    ImGui::RenderTextClipped(box.Min, box.Max, text.data(), text.data() + length, nullptr, ImVec2(0.5F, 0.5F));
    if (label_size.x > 0.0F)
        ImGui::RenderText(ImVec2(frame.Max.x + style.ItemInnerSpacing.x, frame.Min.y + style.FramePadding.y), label);
    return changed;
}

bool editor_slider(const char* label, float* value, const float minimum, const float maximum, const char* format,
                   const bool logarithmic) {
    double wide = *value;
    const bool changed = editor_slider(label, &wide, minimum, maximum, format, logarithmic);
    if (changed) *value = static_cast<float>(wide);
    return changed;
}

std::string slider_format(const double minimum, const double maximum, const double step, const std::string_view suffix) {
    int decimals = 0;
    if (step > 0.0) {
        decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(step) - 1e-9)), 0, 4);
    } else {
        const double range = std::abs(maximum - minimum);
        decimals = range <= 10.0 ? 2 : range <= 100.0 ? 1 : 0;
    }
    std::string format = "%." + std::to_string(decimals) + "f";
    if (!suffix.empty()) format += std::string(suffix);
    return format;
}

AssetIcon asset_icon_for_kind(const std::string_view kind, const std::string_view material_type) {
    if (kind == "folder") return AssetIcon::folder;
    if (kind == "model") return AssetIcon::model;
    if (kind == "mesh") return AssetIcon::mesh;
    if (kind == "scene") return AssetIcon::scene;
    if (kind == "template") return AssetIcon::node_template;
    if (kind == "image") return AssetIcon::image;
    if (kind == "material")
        return material_type == "sky" ? AssetIcon::sky_material
               : material_type == "post_process" ? AssetIcon::post_material
                                                 : AssetIcon::material;
    if (kind == "shader") return AssetIcon::shader;
    if (kind == "script") return AssetIcon::script;
    if (kind == "text") return AssetIcon::text;
    if (kind == "audio") return AssetIcon::audio;
    if (kind == "media") return AssetIcon::video;
    if (kind == "node") return AssetIcon::node;
    return AssetIcon::other;
}

AssetIcon asset_icon_for_file(const std::string_view name) {
    std::string lower(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto ends = [&](const std::string_view suffix) { return lower.ends_with(suffix); };
    if (ends(".relay-material")) return AssetIcon::material;
    if (ends(".relay-shader")) return AssetIcon::shader;
    if (ends(".relay-template.json")) return AssetIcon::node_template;
    if (ends(".relay.json")) return AssetIcon::scene;
    for (const auto* model : {".glb", ".gltf", ".fbx", ".obj", ".dae", ".blend"})
        if (ends(model)) return AssetIcon::model;
    for (const auto* image : {".png", ".jpg", ".jpeg", ".webp", ".tga", ".bmp", ".hdr", ".exr"})
        if (ends(image)) return AssetIcon::image;
    for (const auto* sound : {".wav", ".flac", ".mp3", ".ogg"})
        if (ends(sound)) return AssetIcon::audio;
    for (const auto* video : {".mp4", ".webm", ".mov", ".mkv"})
        if (ends(video)) return AssetIcon::video;
    for (const auto* code : {".cpp", ".hpp", ".h", ".cc", ".cxx"})
        if (ends(code)) return AssetIcon::script;
    for (const auto* text : {".txt", ".md", ".json"})
        if (ends(text)) return AssetIcon::text;
    return AssetIcon::other;
}

const char* asset_icon_label(const AssetIcon icon) {
    switch (icon) {
    case AssetIcon::folder: return "Folder";
    case AssetIcon::model: return "Model";
    case AssetIcon::mesh: return "Mesh";
    case AssetIcon::scene: return "Scene";
    case AssetIcon::node_template: return "Template";
    case AssetIcon::image: return "Image";
    case AssetIcon::material: return "Material";
    case AssetIcon::sky_material: return "Sky material";
    case AssetIcon::post_material: return "Post-processing material";
    case AssetIcon::shader: return "Shader";
    case AssetIcon::script: return "Script";
    case AssetIcon::text: return "Text";
    case AssetIcon::audio: return "Sound";
    case AssetIcon::video: return "Video";
    case AssetIcon::node: return "Node";
    case AssetIcon::other: break;
    }
    return "File";
}

ImU32 asset_icon_color(const AssetIcon icon, const float alpha) {
    switch (icon) {
    case AssetIcon::folder: return srgb(222, 176, 88, alpha);
    case AssetIcon::model: return srgb(96, 168, 236, alpha);
    case AssetIcon::mesh: return srgb(118, 206, 214, alpha);
    case AssetIcon::scene: return srgb(160, 144, 232, alpha);
    case AssetIcon::node_template: return srgb(214, 140, 232, alpha);
    case AssetIcon::image: return srgb(110, 196, 140, alpha);
    case AssetIcon::material: return srgb(236, 150, 92, alpha);
    case AssetIcon::sky_material: return srgb(120, 186, 244, alpha);
    case AssetIcon::post_material: return srgb(196, 160, 244, alpha);
    case AssetIcon::shader: return srgb(238, 200, 96, alpha);
    case AssetIcon::script: return srgb(176, 208, 108, alpha);
    case AssetIcon::text: return srgb(168, 176, 190, alpha);
    case AssetIcon::audio: return srgb(236, 110, 142, alpha);
    case AssetIcon::video: return srgb(226, 104, 92, alpha);
    case AssetIcon::node: return srgb(146, 178, 255, alpha);
    case AssetIcon::other: break;
    }
    return srgb(138, 148, 163, alpha);
}

void draw_asset_type_icon(ImDrawList* list, const ImVec2 min, const float size, const AssetIcon icon, const float alpha) {
    const ImU32 color = asset_icon_color(icon, alpha);
    const ImU32 soft = asset_icon_color(icon, alpha * 0.35F);
    const float stroke = std::max(1.0F, size * 0.085F);
    const auto at = [&](const float x, const float y) { return ImVec2(min.x + x * size, min.y + y * size); };
    const auto line = [&](const float x0, const float y0, const float x1, const float y1) {
        list->AddLine(at(x0, y0), at(x1, y1), color, stroke);
    };
    const auto page = [&] {
        // A sheet with a folded corner.
        list->PathLineTo(at(0.22F, 0.1F));
        list->PathLineTo(at(0.6F, 0.1F));
        list->PathLineTo(at(0.8F, 0.3F));
        list->PathLineTo(at(0.8F, 0.9F));
        list->PathLineTo(at(0.22F, 0.9F));
        list->PathStroke(color, ImDrawFlags_Closed, stroke);
        line(0.6F, 0.1F, 0.6F, 0.3F);
        line(0.6F, 0.3F, 0.8F, 0.3F);
    };
    switch (icon) {
    case AssetIcon::folder:
        list->AddRectFilled(at(0.08F, 0.2F), at(0.46F, 0.34F), color, size * 0.06F);
        list->AddRectFilled(at(0.08F, 0.3F), at(0.92F, 0.84F), color, size * 0.08F);
        list->AddLine(at(0.08F, 0.4F), at(0.92F, 0.4F), asset_icon_color(icon, alpha * 0.55F), std::max(1.0F, size * 0.05F));
        break;
    case AssetIcon::model: {
        // An isometric cube with a lit top.
        const ImVec2 top = at(0.5F, 0.1F), left = at(0.12F, 0.3F), right = at(0.88F, 0.3F), centre = at(0.5F, 0.5F);
        const ImVec2 bottom = at(0.5F, 0.92F), lower_left = at(0.12F, 0.72F), lower_right = at(0.88F, 0.72F);
        list->AddQuadFilled(top, right, centre, left, color);
        list->AddQuadFilled(left, centre, bottom, lower_left, asset_icon_color(icon, alpha * 0.55F));
        list->AddQuadFilled(centre, right, lower_right, bottom, asset_icon_color(icon, alpha * 0.8F));
        break;
    }
    case AssetIcon::mesh: {
        // A wireframe of triangles.
        const std::array<ImVec2, 5> points{at(0.12F, 0.8F), at(0.5F, 0.86F), at(0.88F, 0.72F), at(0.3F, 0.2F), at(0.74F, 0.16F)};
        list->AddTriangleFilled(points[0], points[1], points[3], soft);
        for (const auto& [a, b] : std::array<std::pair<int, int>, 7>{{{0, 1}, {1, 2}, {0, 3}, {3, 1}, {1, 4}, {3, 4}, {4, 2}}})
            list->AddLine(points[static_cast<std::size_t>(a)], points[static_cast<std::size_t>(b)], color, stroke);
        for (const auto& point : points) list->AddCircleFilled(point, stroke * 1.2F, color, 8);
        break;
    }
    case AssetIcon::scene:
        // A landscape in a frame: hills under a sun.
        list->AddRect(at(0.08F, 0.16F), at(0.92F, 0.84F), color, size * 0.08F, 0, stroke);
        list->AddTriangleFilled(at(0.14F, 0.78F), at(0.4F, 0.42F), at(0.64F, 0.78F), color);
        list->AddTriangleFilled(at(0.46F, 0.78F), at(0.66F, 0.52F), at(0.86F, 0.78F), asset_icon_color(icon, alpha * 0.6F));
        list->AddCircleFilled(at(0.72F, 0.34F), size * 0.08F, color, 12);
        break;
    case AssetIcon::node_template: {
        // A painter's palette.
        list->AddCircleFilled(at(0.5F, 0.52F), size * 0.4F, color, 24);
        list->AddCircleFilled(at(0.62F, 0.68F), size * 0.09F, IM_COL32(0, 0, 0, static_cast<int>(150 * alpha)), 12);
        const ImU32 dot = IM_COL32(255, 255, 255, static_cast<int>(200 * alpha));
        list->AddCircleFilled(at(0.34F, 0.4F), size * 0.06F, dot, 8);
        list->AddCircleFilled(at(0.52F, 0.3F), size * 0.06F, dot, 8);
        list->AddCircleFilled(at(0.7F, 0.42F), size * 0.06F, dot, 8);
        break;
    }
    case AssetIcon::image:
        list->AddRect(at(0.08F, 0.16F), at(0.92F, 0.84F), color, size * 0.08F, 0, stroke);
        list->AddTriangleFilled(at(0.16F, 0.76F), at(0.42F, 0.46F), at(0.66F, 0.76F), color);
        list->AddTriangleFilled(at(0.5F, 0.76F), at(0.68F, 0.56F), at(0.84F, 0.76F), asset_icon_color(icon, alpha * 0.6F));
        list->AddCircle(at(0.7F, 0.34F), size * 0.08F, color, 12, stroke);
        break;
    case AssetIcon::material: {
        // A shaded ball with a highlight.
        const ImVec2 centre = at(0.5F, 0.52F);
        list->AddCircleFilled(centre, size * 0.4F, asset_icon_color(icon, alpha * 0.55F), 24);
        list->AddCircleFilled(at(0.44F, 0.46F), size * 0.3F, color, 24);
        list->AddCircleFilled(at(0.36F, 0.36F), size * 0.1F, IM_COL32(255, 255, 255, static_cast<int>(190 * alpha)), 12);
        break;
    }
    case AssetIcon::sky_material:
        // A sun setting over the horizon.
        list->PathArcTo(at(0.5F, 0.66F), size * 0.26F, IM_PI, 2.0F * IM_PI, 16);
        list->PathFillConvex(color);
        line(0.06F, 0.68F, 0.94F, 0.68F);
        line(0.2F, 0.82F, 0.8F, 0.82F);
        line(0.5F, 0.1F, 0.5F, 0.24F);
        line(0.14F, 0.3F, 0.24F, 0.38F);
        line(0.86F, 0.3F, 0.76F, 0.38F);
        break;
    case AssetIcon::post_material:
        // Stacked layers, as effects applied one over another.
        for (int layer = 2; layer >= 0; --layer) {
            const float y = 0.22F + static_cast<float>(layer) * 0.2F;
            const std::array<ImVec2, 4> diamond{at(0.5F, y - 0.14F), at(0.9F, y + 0.06F), at(0.5F, y + 0.26F), at(0.1F, y + 0.06F)};
            list->AddQuadFilled(diamond[0], diamond[1], diamond[2], diamond[3],
                                layer == 0 ? color : asset_icon_color(icon, alpha * (0.75F - 0.25F * static_cast<float>(layer))));
        }
        break;
    case AssetIcon::shader:
        // Two nodes joined by a wire.
        list->AddRectFilled(at(0.06F, 0.18F), at(0.4F, 0.48F), color, size * 0.07F);
        list->AddRectFilled(at(0.6F, 0.52F), at(0.94F, 0.82F), color, size * 0.07F);
        list->AddBezierCubic(at(0.4F, 0.33F), at(0.58F, 0.33F), at(0.42F, 0.67F), at(0.6F, 0.67F), color, stroke, 12);
        break;
    case AssetIcon::script: {
        // Braces.
        line(0.36F, 0.14F, 0.26F, 0.2F);
        line(0.26F, 0.2F, 0.26F, 0.42F);
        line(0.26F, 0.42F, 0.14F, 0.5F);
        line(0.14F, 0.5F, 0.26F, 0.58F);
        line(0.26F, 0.58F, 0.26F, 0.8F);
        line(0.26F, 0.8F, 0.36F, 0.86F);
        line(0.64F, 0.14F, 0.74F, 0.2F);
        line(0.74F, 0.2F, 0.74F, 0.42F);
        line(0.74F, 0.42F, 0.86F, 0.5F);
        line(0.86F, 0.5F, 0.74F, 0.58F);
        line(0.74F, 0.58F, 0.74F, 0.8F);
        line(0.74F, 0.8F, 0.64F, 0.86F);
        break;
    }
    case AssetIcon::text:
        page();
        line(0.34F, 0.46F, 0.68F, 0.46F);
        line(0.34F, 0.6F, 0.68F, 0.6F);
        line(0.34F, 0.74F, 0.56F, 0.74F);
        break;
    case AssetIcon::audio:
        // A pair of beamed notes.
        list->AddCircleFilled(at(0.28F, 0.76F), size * 0.12F, color, 12);
        list->AddCircleFilled(at(0.72F, 0.66F), size * 0.12F, color, 12);
        line(0.38F, 0.76F, 0.38F, 0.2F);
        line(0.82F, 0.66F, 0.82F, 0.1F);
        list->AddQuadFilled(at(0.38F, 0.2F), at(0.82F, 0.1F), at(0.82F, 0.24F), at(0.38F, 0.34F), color);
        break;
    case AssetIcon::video:
        list->AddRect(at(0.08F, 0.2F), at(0.92F, 0.8F), color, size * 0.1F, 0, stroke);
        list->AddTriangleFilled(at(0.4F, 0.34F), at(0.68F, 0.5F), at(0.4F, 0.66F), color);
        break;
    case AssetIcon::node:
        // A node with its three axes.
        line(0.5F, 0.5F, 0.5F, 0.12F);
        line(0.5F, 0.5F, 0.86F, 0.7F);
        line(0.5F, 0.5F, 0.14F, 0.7F);
        list->AddCircleFilled(at(0.5F, 0.5F), size * 0.16F, color, 16);
        break;
    case AssetIcon::other:
        page();
        break;
    }
}

std::string asset_display_name(const std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? path : path.substr(slash + 1U));
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string_view suffix : {".relay-template.json", ".relay.json"})
        if (lower.size() > suffix.size() && lower.ends_with(suffix)) return name.substr(0, name.size() - suffix.size());
    const auto dot = name.rfind('.');
    return dot == std::string::npos || dot == 0U ? name : name.substr(0, dot);
}

void srgb_rows_to_linear(std::vector<std::uint8_t>& rgba) {
    static const auto table = [] {
        std::array<std::uint8_t, 256> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const float c = static_cast<float>(index) / 255.0F;
            const float linear = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
            values[index] = static_cast<std::uint8_t>(std::lround(linear * 255.0F));
        }
        return values;
    }();
    for (std::size_t index = 0; index + 3U < rgba.size(); index += 4U)
        for (std::size_t channel = 0; channel < 3U; ++channel) rgba[index + channel] = table[rgba[index + channel]];
}

} // namespace relay
