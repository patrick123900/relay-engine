#include "relay/editor/editor_ui.hpp"
#include "relay/editor/chat_media.hpp"
#include "relay/audio/audio_settings.hpp"

#include "relay/core/input.hpp"
#include "relay/core/json.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/editor/editor_camera.hpp"
#include "relay/editor/wrapped_input.hpp"
#include "relay/editor/editor_layout.hpp"
#include "relay/editor/file_browser.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_state.hpp"
#include "relay/editor/editor_selection.hpp"
#include "relay/editor/editor_timeline.hpp"
#include "relay/editor/editor_theme.hpp"
#include "relay/editor/editor_widgets.hpp"
#include "relay/editor/asset_thumbnails.hpp"
#include "relay/editor/shader_graph_canvas.hpp"
#include "relay/platform/sdl_input.hpp"
#include "relay/render/graphics_settings.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/render/shader_graph.hpp"

#include <ImGuizmo.h>
#include <TextEditor.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <array>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>
#include <mutex>
#include <filesystem>

namespace relay {
namespace {

// Read-only queries are untraced. Bound idle polling to keep large scene/asset snapshots
// inexpensive; mutations invalidate the scene immediately instead of waiting for the next idle
// refresh.
constexpr double refresh_interval_seconds = 0.5;
// While a selected animator is playing, the inspector is showing a value that moves every frame,
// and a twice-a-second playhead is not usable for judging a pose. Polling faster stays read-only
// and untraced, and the cadence drops back the moment playback stops.
constexpr double playback_refresh_interval_seconds = 0.05;
constexpr std::size_t maximum_log_lines = 512U;

std::string number_text(const double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return stream.str();
}

double number_or(const JsonValue::Object& object, const std::string_view name,
                 const double fallback) {
    const auto* value = field(object, name);
    if (value == nullptr) return fallback;
    const auto* number = value->number();
    return number == nullptr ? fallback : *number;
}

std::string string_or(const JsonValue::Object& object, const std::string_view name,
                      const std::string& fallback = {}) {
    const auto* value = field(object, name);
    if (value == nullptr) return fallback;
    const auto* text = value->string();
    return text == nullptr ? fallback : *text;
}

bool boolean_or(const JsonValue::Object& object, const std::string_view name, const bool fallback) {
    const auto* value = field(object, name);
    if (value == nullptr) return fallback;
    const auto* flag = value->boolean();
    return flag == nullptr ? fallback : *flag;
}

// Returns the named sub-object when it is present and not JSON null, which is how the scene
// serializer encodes an absent component.
const JsonValue::Object* component(const JsonValue::Object& entity, const std::string_view name) {
    const auto* value = field(entity, name);
    return value == nullptr ? nullptr : value->object();
}

// Dockable panels, in the order of EditorUi::Impl::panel_open.
constexpr std::array<const char*, 12> panel_names{"Hierarchy", "Inspector", "Assets", "History",
                                                  "Diagnostics", "Viewport", "Timeline", "Project",
                                                  "Agent", "Profiler", "Mixer", "Shader Editor"};
constexpr std::array<bool, panel_names.size()> default_panels{true, true, true, false, true,
                                                              true, false, false, false, false,
                                                              false, false};

// A dim caption in a fixed column, so every inspector row lines up down the panel.
void row_label(const char* const label, const float width) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(editor_color(editor_palette().text_dim), "%s", label);
    ImGui::SameLine(width);
}

// Three axis-tinted fields. Tinting X, Y and Z is the convention every 3D editor uses, and it
// makes a row of six identical numbers readable at a glance.
} // namespace

struct EditorUi::Impl {
    // Scrollable regions read as part of the panel, not as cards floating on top of it.
    bool begin_region(const char* const name, const ImVec2 size = ImVec2(0.0F, 0.0F)) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        const bool open = ImGui::BeginChild(name, size, ImGuiChildFlags_None);
        ImGui::PopStyleColor();
        return open;
    }

    enum class ToolIcon { previous, restart, loop, snap, play, stop, pause, step, undo, redo, camera, focus, move, rotate, scale, local, world };
    bool toolbar_button(const char* id, ToolIcon icon, bool active, const char* tooltip) {
        const auto& palette = editor_palette();
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, palette.accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette.accent_hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette.accent_active);
        }
        const bool pressed = ImGui::Button(id, ImVec2(32.0F * ui_scale, 28.0F * ui_scale));
        if (active)
            ImGui::PopStyleColor(3);
        auto* draw = ImGui::GetWindowDrawList();
        const auto top = ImGui::GetItemRectMin();
        const auto color = ImGui::GetColorU32(ImGuiCol_Text);
        const auto point = [&](float x, float y) {
            return ImVec2(top.x + x * ui_scale, top.y + y * ui_scale);
        };
        const auto line = [&](float x, float y, float u, float v) {
            draw->AddLine(point(x, y), point(u, v), color, 1.6F * ui_scale);
        };
        if (icon == ToolIcon::previous || icon == ToolIcon::restart) {
            line(8, 7, 8, 21);
            draw->AddTriangleFilled(point(22, 7), point(22, 21), point(10, 14), color);
            if (icon == ToolIcon::restart) line(5, 7, 5, 21);
        } else if (icon == ToolIcon::loop) {
            // Two opposing arrows, with rounded returns and arrowheads along the path.
            draw->PathLineTo(point(8, 16));
            draw->PathLineTo(point(8, 12));
            draw->PathBezierCubicCurveTo(point(8, 9), point(10, 8), point(13, 8));
            draw->PathLineTo(point(24, 8));
            draw->PathStroke(color, 0, 1.6F * ui_scale);
            line(24, 8, 20, 4);
            line(24, 8, 20, 12);
            draw->PathLineTo(point(24, 12));
            draw->PathLineTo(point(24, 16));
            draw->PathBezierCubicCurveTo(point(24, 19), point(22, 20), point(19, 20));
            draw->PathLineTo(point(8, 20));
            draw->PathStroke(color, 0, 1.6F * ui_scale);
            line(8, 20, 12, 16);
            line(8, 20, 12, 24);
        } else if (icon == ToolIcon::snap) {
            draw->AddRect(point(9, 6), point(23, 22), color, 2 * ui_scale, 0, 1.6F * ui_scale);
            line(13, 6, 13, 11); line(19, 17, 23, 17); line(13, 6, 13, 22);
        } else if (icon == ToolIcon::stop) {
            draw->AddRectFilled(point(10, 8), point(22, 20), color);
        } else if (icon == ToolIcon::pause) {
            draw->AddRectFilled(point(11, 7), point(14, 21), color);
            draw->AddRectFilled(point(18, 7), point(21, 21), color);
        } else if (icon == ToolIcon::play || icon == ToolIcon::step) {
            draw->AddTriangleFilled(point(10, 7), point(10, 21), point(22, 14), color);
            if (icon == ToolIcon::step)
                line(24, 7, 24, 21);
        } else if (icon == ToolIcon::undo || icon == ToolIcon::redo) {
            const auto x = [&](float value) {
                return icon == ToolIcon::undo ? value : 32.0F - value;
            };
            line(x(9), 10, x(20), 10);
            line(x(20), 10, x(24), 14);
            line(x(24), 14, x(24), 21);
            line(x(9), 10, x(14), 5);
            line(x(9), 10, x(14), 15);
        } else if (icon == ToolIcon::camera) {
            draw->AddRect(point(7, 8), point(21, 20), color, 2.0F * ui_scale, 0, 1.6F * ui_scale);
            line(21, 11, 26, 8);
            line(26, 8, 26, 20);
            line(26, 20, 21, 17);
        } else if (icon == ToolIcon::focus) {
            for (const auto x : {8.0F, 24.0F})
                for (const auto y : {6.0F, 22.0F}) {
                    line(x, y, x + (x < 16 ? 5.0F : -5.0F), y);
                    line(x, y, x, y + (y < 14 ? 5.0F : -5.0F));
                }
            draw->AddCircleFilled(point(16, 14), 2.0F * ui_scale, color, 16);
        } else if (icon == ToolIcon::move) {
            line(7, 14, 25, 14);
            line(16, 5, 16, 23);
            line(7, 14, 11, 10);
            line(7, 14, 11, 18);
            line(25, 14, 21, 10);
            line(25, 14, 21, 18);
            line(16, 5, 12, 9);
            line(16, 5, 20, 9);
            line(16, 23, 12, 19);
            line(16, 23, 20, 19);
        } else if (icon == ToolIcon::rotate) {
            // The arc ends at the top, where its clockwise tangent points right.
            draw->PathArcTo(point(16, 14), 8.0F * ui_scale, -0.35F, 4.712389F, 32);
            draw->PathStroke(color, 0, 1.6F * ui_scale);
            line(16, 6, 13, 3);
            line(16, 6, 13, 9);
        } else if (icon == ToolIcon::scale) {
            draw->AddRect(point(8, 15), point(14, 21), color, 0, 0, 1.6F * ui_scale);
            line(14, 15, 24, 5);
            line(18, 5, 24, 5);
            line(24, 5, 24, 11);
        } else if (icon == ToolIcon::world) {
            draw->AddCircle(point(16, 14), 9.0F * ui_scale, color, 24, 1.4F * ui_scale);
            line(7, 14, 25, 14);
            line(16, 5, 16, 23);
            draw->AddEllipse(point(16, 14), ImVec2(4 * ui_scale, 9 * ui_scale), color, 0, 24,
                             1.4F * ui_scale);
        } else {
            line(16, 14, 16, 5);
            line(16, 14, 25, 18);
            line(16, 14, 8, 21);
            draw->AddCircleFilled(point(16, 14), 2.0F * ui_scale, color);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
        return pressed;
    }

    void toolbar_divider() {
        const auto& palette = editor_palette();
        const auto origin = ImGui::GetCursorScreenPos();
        const auto height = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + 4.0F, origin.y + 3.0F),
                                            ImVec2(origin.x + 4.0F, origin.y + height - 3.0F),
                                            palette.border, 1.0F);
        ImGui::Dummy(ImVec2(9.0F, height));
        ImGui::SameLine();
    }

    explicit Impl(RequestHandler handler) : request(std::move(handler)) {}

    std::map<ImGuiID, EditorScalarDraft> drafts;
    bool drawing_inspector{};

    // How a field's name reads in the label column: any "##id" dropped, underscores as spaces and a
    // capital first letter, so script and shader names like wave_height read "Wave height".
    static std::string field_display_name(const std::string_view label) {
        std::string text(label.substr(0, label.find("##")));
        std::replace(text.begin(), text.end(), '_', ' ');
        if (!text.empty()) text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        return text;
    }

    void inspector_field_label(const char* raw_label) const {
        const auto shown = field_display_name(raw_label);
        const char* label = shown.c_str();
        const float label_width = 108.0F * ui_scale;
        if (ImGui::CalcTextSize(label).x > label_width - 12.0F * ui_scale ||
            ImGui::GetContentRegionAvail().x < 230.0F * ui_scale) {
            ImGui::TextColored(editor_color(editor_palette().text_dim), "%s", label);
        } else {
            row_label(label, ImGui::GetWindowContentRegionMin().x + label_width);
        }
        ImGui::SetNextItemWidth(inspector_field_reserve > 0.0F ? -inspector_field_reserve : -FLT_MIN);
    }

    // A checkbox with its name in the label column on the left, like every other Inspector field.
    bool inspector_checkbox(const char* label, bool* value) {
        inspector_field_label(label);
        ImGui::PushID(label);
        const bool changed = editor_checkbox("##value", value);
        ImGui::PopID();
        return changed;
    }

    bool inspector_begin_combo(const char* label, const char* preview) const {
        inspector_field_label(label);
        const auto id = std::string{"##"} + label;
        return ImGui::BeginCombo(id.c_str(), preview);
    }

    // Reports every change while the field is dragged, so the scene follows the mouse. The whole
    // drag shares one gesture token, which the next mutation carries so it stays one undo step.
    bool drag_scalar(const char* label, double& current, float speed, const char* format = "%.6f") {
        auto& draft = drafts[ImGui::GetID(label)];
        const bool inspector_field = drawing_inspector && label[0] != '#';
        if (inspector_field) {
            ImGui::PushID(label);
            inspector_field_label(label);
        }
        const bool changed = ImGui::DragScalar(
            inspector_field ? "##value" : label, ImGuiDataType_Double, &draft.begin(current), speed,
            nullptr, nullptr, inspector_field && std::string_view(format) == "%.6f" ? "%.3f" : format);
        if (ImGui::IsItemActivated()) inspector_gesture = ++gesture_serial;
        const bool commit = changed || ImGui::IsItemDeactivatedAfterEdit();
        if (commit) pending_gesture = inspector_gesture;
        current = draft.value;
        draft.finish(ImGui::IsItemActive());
        if (inspector_field) ImGui::PopID();
        return commit;
    }

    static double linear_to_srgb(const double value) {
        const double c = std::max(value, 0.0);
        return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    }

    static double srgb_to_linear(const double value) {
        const double c = std::max(value, 0.0);
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }

    // A linear color edited as it looks: the fields and picker show sRGB, values above 1 are
    // allowed for bright skies, and the scene stores linear. Like drag_scalar, every change applies
    // at once, and one pick or drag is one undo step.
    bool color_edit(const char* label, std::array<double, 3>& linear) {
        ImGui::PushID(label);
        inspector_field_label(label);
        std::array<float, 3> shown{};
        for (std::size_t channel = 0; channel < 3U; ++channel)
            shown[channel] = static_cast<float>(linear_to_srgb(linear[channel]));
        const bool changed = ImGui::ColorEdit3("##color", shown.data(),
                                               ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        const auto key = ImGui::GetID("##color");
        ImGui::PushID("##color");
        const bool editing = ImGui::IsItemActive() || ImGui::IsPopupOpen("picker");
        ImGui::PopID();
        if (editing && color_gestures.insert(key).second) inspector_gesture = ++gesture_serial;
        if (!editing) color_gestures.erase(key);
        if (changed) {
            pending_gesture = inspector_gesture;
            for (std::size_t channel = 0; channel < 3U; ++channel)
                linear[channel] = std::min(srgb_to_linear(shown[channel]), 1000.0);
        }
        ImGui::PopID();
        return changed;
    }

    // A bar running between two linear colors, as a preview of a gradient.
    void draw_color_ramp(const std::array<double, 3>& from, const std::array<double, 3>& to,
                         const char* tooltip) {
        const auto packed = [](const std::array<double, 3>& color) {
            const auto channel = [&](const std::size_t index) {
                return static_cast<int>(std::clamp(linear_to_srgb(color[index]), 0.0, 1.0) * 255.0 + 0.5);
            };
            return IM_COL32(channel(0), channel(1), channel(2), 255);
        };
        inspector_field_label("");
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = 10.0F * ui_scale;
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            origin, ImVec2(origin.x + width, origin.y + height), packed(from), packed(to),
            packed(to), packed(from));
    }

    // Keep the draft stable across refreshes, but apply each change for live pose scrubbing.
    bool slider_scalar(const char* label, double& current, const double minimum,
                       const double maximum, const char* format) {
        auto& draft = drafts[ImGui::GetID(label)];
        const bool inspector_field = drawing_inspector && label[0] != '#';
        if (inspector_field) {
            ImGui::PushID(label);
            inspector_field_label(label);
        }
        const bool changed = editor_slider(inspector_field ? "##value" : label, &draft.begin(current), minimum,
                                           maximum, format);
        if (ImGui::IsItemActivated()) animation_gesture = ++gesture_serial;
        current = std::clamp(draft.value, minimum, maximum);
        draft.finish(ImGui::IsItemActive());
        if (inspector_field) ImGui::PopID();
        return changed;
    }

    unsigned drag_vector3(const char* label, std::array<double, 3>& value, float speed,
                          float label_width) {
        constexpr std::array<const char*, 3> names{"X", "Y", "Z"};
        constexpr std::array<ImU32, 3> tints{IM_COL32(226, 109, 109, 255),
                                             IM_COL32(125, 200, 125, 255),
                                             IM_COL32(109, 156, 226, 255)};
        ImGui::PushID(label);
        const auto& style = ImGui::GetStyle();
        const auto shown = field_display_name(label);
        const bool stacked = ImGui::GetContentRegionAvail().x < label_width + 220.0F * ui_scale ||
                             ImGui::CalcTextSize(shown.c_str()).x + 12.0F * ui_scale > label_width;
        if (stacked) {
            ImGui::TextColored(editor_color(editor_palette().text_dim), "%s", shown.c_str());
        } else {
            row_label(shown.c_str(), label_width);
        }
        const float tag = ImGui::CalcTextSize("X").x + style.ItemInnerSpacing.x;
        const float width =
            (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 2.0F) / 3.0F - tag;
        unsigned committed = 0;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            ImGui::PushID(static_cast<int>(axis));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(editor_color(tints[axis]), "%s", names[axis]);
            ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(width);
            if (drag_scalar("##field", value[axis], speed, "%.3f")) committed |= 1U << axis;
            if (label == std::string_view{"Half extents"})
                note_item(std::string("collider:half_extent:") + names[axis]);
            if (axis < 2) ImGui::SameLine(0.0F, style.ItemSpacing.x);
            ImGui::PopID();
        }
        ImGui::PopID();
        return committed;
    }

    static std::string vector_fields(const std::array<double, 3>& values,
                                     const std::array<const char*, 3>& names, unsigned mask = 7U) {
        std::string result;
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (mask & (1U << axis))
                result += ",\"" + std::string(names[axis]) + "\":" + number_text(values[axis]);
        return result;
    }

    RequestHandler request;
    std::uint64_t next_request_id{1};

    bool imgui_context_created{false};
    bool headless{false};
    // Width Inspector fields leave free on their right, for a button after them.
    float inspector_field_reserve{0.0F};
    // The Inspector's material preview: the surface material shown this frame and last frame
    // (what the renderer is asked for), and the last sphere rendered, as one reused texture.
    std::string preview_wanted, preview_request;
    std::string preview_path;
    std::unique_ptr<ImTextureData> preview_texture;
    struct DeliveredPreview {
        std::string path;
        std::uint32_t width{}, height{};
        std::vector<std::uint8_t> rgba;
    } last_preview;
    AssetThumbnails thumbnails;
    std::map<std::string, std::array<float, 4>, std::less<>> headless_items;
    void note_item(const std::string& key) {
        note_rect(key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }
    void note_rect(const std::string& key, const ImVec2 minimum, const ImVec2 maximum) {
        if (!headless) return;
        headless_items[key] = {minimum.x, minimum.y, maximum.x, maximum.y};
    }
    bool sdl_backend_started{false};
    bool vulkan_backend_started{false};
    bool frame_open{false};
    VkDevice device{};

    // Cached runtime state. The editor never touches Scene, SceneHistory or AssetRegistry directly.
    JsonValue scene_list;
    JsonValue collider_boxes;
    // audio.debug_shapes: reverb zones and spatial source ranges for the viewport.
    JsonValue audio_shapes;
    std::string scene_list_reply, collider_boxes_reply, agent_review_reply, agent_audit_reply;
    std::string audio_shapes_reply;
    static constexpr int refresh_stages = 4;
    int startup_frames{}; // Frames built since the ImGui context and saved layout were created.
    int refresh_stage{-1}; // Next periodic refresh stage, or -1 between refreshes.
    std::vector<const JsonValue::Object*> entities;
    std::map<std::string, std::vector<std::size_t>, std::less<>> children;
    std::vector<std::size_t> roots;
    std::map<std::string, std::size_t, std::less<>> entity_index;
    JsonValue runtime_status;
    JsonValue project_status;
    std::vector<std::string> project_files;
    std::array<char, 129> project_name{"My project"};
    std::string pending_scene_filename, pending_project_filename;
    std::vector<std::string> mesh_names;
    std::vector<std::string> material_names;
    std::deque<std::string> log_lines;
    std::uint64_t last_log_sequence{0};

    EditorSelection selections;
    // The Hierarchy row the left button went down on, selected if it comes up there undragged.
    std::string hierarchy_pressed;
    bool hierarchy_press_double{};
    std::string& selection = selections.primary_handle;
    std::vector<std::string> visible_rows, drawing_rows;
    bool clipboard_ready{false};
    std::string status_message;
    bool status_is_error{false};
    double seconds_since_refresh{refresh_interval_seconds};
    // The asset and model lists are far more expensive to serialize than the scene listing, so
    // they keep the slow cadence even when the rest of the panels are polling for a playhead.
    double seconds_since_assets{refresh_interval_seconds};
    bool animator_playing{false};
    bool refresh_pending{true};
    bool assets_pending{true};

    [[nodiscard]] double refresh_interval() const {
        return animator_playing ? playback_refresh_interval_seconds : refresh_interval_seconds;
    }

    // Inspector edit buffers, so a drag in progress is not overwritten by a background refresh.
    std::map<std::string, std::vector<double>, std::less<>> morph_defaults;
    struct ClipInfo { std::string name; double duration_seconds{}; };
    // Clip names and lengths per model, so the animator can offer a named list and a time slider
    // bounded by the clip rather than a bare index field and an unbounded number.
    std::map<std::string, std::vector<ClipInfo>, std::less<>> model_clips;

    // Editor viewpoint. This is view state, not scene state: it creates no entity, is never saved,
    // never enters the undo history. Protocol camera controls share this view-only state; navigating the viewport
    // at mouse rate produces no trace entries.
    bool camera_enabled{true};
    bool grid_enabled{true};
    bool collider_wireframes_enabled{true};
    bool node_icons_enabled{true};
    bool camera_wireframes_enabled{true};
    struct NodeMarker { std::string handle; std::string label; ImVec2 screen; float depth; float radius; };
    std::vector<NodeMarker> node_markers;
    EditorCamera navigation_camera;
    bool freelook_latched{false};
    bool navigating{false};
    std::set<SDL_Keycode> editor_keys;
    SDL_Window* sdl_window{};
    bool mouse_captured{false};
    ImVec2 mouse_anchor{}, relative_delta{};
    ViewOverride view;
    EditorViewport viewport;
    EditorLayout layout;
    bool viewport_visible{false}, viewport_hovered{false};
    ImDrawList* viewport_draw_list{};
    const std::function<void()>* scene_recorder{};
    static void draw_scene_callback(const ImDrawList*, const ImDrawCmd* command) {
        const auto* self = static_cast<const Impl*>(command->UserCallbackData);
        if (self->scene_recorder) (*self->scene_recorder)();
    }
    EditorFonts fonts;
    float ui_scale{1.0F};
    bool gizmo_active{false};
    // A fresh token per drag. Updates sharing it collapse into one undo entry; a new drag must not
    // fold into an earlier, unrelated edit of the same entity. Every kind of drag draws its token
    // from one counter, so a gizmo drag and an inspector drag can never share a token.
    std::uint64_t gesture_serial{0};
    std::uint64_t gizmo_gesture{0};
    std::uint64_t animation_gesture{0};
    std::uint64_t timeline_gesture{0};
    std::uint64_t inspector_gesture{0};
    // Set by an inspector drag that changed its value this frame; the next mutation carries it.
    std::uint64_t pending_gesture{0};
    int timeline_fps{30};
    bool timeline_snap{false};
    double timeline_preview_time{};
    std::vector<std::string> timeline_scrub_targets;
    std::string timeline_metadata_key;
    JsonValue timeline_metadata;
    ImGuizmo::OPERATION gizmo_operation{ImGuizmo::TRANSLATE};
    ImGuizmo::MODE gizmo_mode{ImGuizmo::LOCAL};
    // Screen-space bounds of the area left clear for the scene, in the window's pixel coordinates.
    ImVec2 viewport_min{}, viewport_max{};

    std::array<char, 129> scene_filename{"main.relay.json"};
    std::array<char, 129> package_filename{"project.tar"};
    // The scene's current content revision and the one last written to or read from a file. They
    // only differ when there is unsaved authoring work, because undoing back to a saved state
    // restores that state's revision. Playback time never enters the history and so never counts.
    std::uint64_t scene_revision{0}, saved_revision{0};
    // False until this scene has actually been written to or opened from `scene_filename`, so the
    // title can distinguish a named file from the untitled scene the editor starts with.
    bool scene_has_file{false};
    std::string window_title;
    [[nodiscard]] bool scene_modified() const { return scene_revision != saved_revision; }

    std::array<char, 129> model_filename{};
    // One inline rename at a time, drawn in place of a hierarchy row or asset entry label.
    enum class RenameKind { none, entity, asset };
    struct InlineRename {
        RenameKind kind{RenameKind::none};
        std::string target;
        std::array<char, 129> buffer{};
        int frames{};
        bool activated{};
    } inline_rename;
    // A second, slower click on the only selected row renames it once the double-click time passes.
    struct SlowClick {
        RenameKind kind{RenameKind::none};
        std::string target;
        double time{};
    } slow_click;
    struct AssetEntry {
        std::string name, path, kind;
        bool folder{}, importable{}, locked{};
    };
    // Search and kind filters. While either is active the panel lists matches flat, not the tree.
    std::array<char, 65> asset_query{};
    std::set<std::string> asset_kind_filters;
    std::vector<AssetEntry> asset_results;
    bool asset_results_truncated{}, asset_search_focus{};
    EditorUi::FileBrowserHandler file_browser = show_in_file_browser;

    void open_in_file_browser(const std::string& path, const bool directory) {
        if (file_browser) file_browser(std::filesystem::path(assets_root) / path, directory);
    }
    // Listings of the project root ("") and every expanded folder, keyed by folder path.
    std::map<std::string, std::vector<AssetEntry>> asset_folders;
    std::set<std::string> expanded_asset_folders;
    std::string asset_selection, asset_delete_pending;
    // Native gameplay scripts. Run Game asks the engine first and reacts to why it refused.
    JsonValue script_status;
    std::string script_status_reply;
    bool play_after_build{}, open_trust_dialog{}, open_new_script_dialog{};
    bool auto_build_scripts{true};
    std::array<char, 129> new_script_name{};
    std::string attach_new_script_to;
    // Add Component window, and per-field text buffers for script properties being typed.
    JsonValue component_catalog;
    std::array<char, 65> component_query{};
    std::string component_selected, component_category{"All"};
    bool open_component_window{};
    // Add Node window: the node type tree, with the project's templates under their root's type.
    JsonValue node_type_catalog;
    std::array<char, 65> node_query{};
    std::array<char, 129> node_name{};
    std::string node_selected{"Node"}, node_parent;
    bool open_node_window{}, node_as_child{};
    // Game Configuration window. The input page edits a working copy and saves each change.
    bool game_config_open{};
    int game_config_page{};
    InputMap input_edit;
    bool input_file_saved{};
    JsonValue input_live;
    // The Graphics page: graphics.settings, refreshed while the page is visible for live status.
    JsonValue graphics_status;
    // Audio: the project's sound files and mixer buses for pickers, audio.settings for the Audio
    // page, audio.status (voices and meters) while a source is selected or the page is open, and
    // decoded clip summaries for the Inspector's waveform.
    std::vector<std::string> audio_files;
    // Shader Editor: open .relay-shader files, one tab each, as node graphs. The shading language
    // is the saved form: the graph is read from the file's text and written back to it, so agents'
    // code and people's graphs are the same file. A tab previews its text in the scene
    // (shaders.preview) a moment after an edit, and saves on Ctrl+S.
    struct ShaderTab {
        std::string path;
        std::string saved;     // The file's text.
        std::string text;      // The text now: the graph written out, or typed code.
        std::string previewed; // The text last sent for preview; empty when showing the file.
        ShaderGraph graph;
        GraphSource source;    // Where each node's code is, to put compile errors on nodes.
        std::string stage{"fragment"};
        std::map<std::string, ShaderGraphView> views;
        std::vector<std::string> history; // Texts for undo.
        std::size_t history_index{};
        // Code the graph cannot show: the shader's functions, and a stage that uses control flow.
        // A file whose declarations do not parse is edited whole as text until they do.
        std::unique_ptr<TextEditor> functions_editor, code_editor;
        std::size_t functions_undo{}, code_undo{};
        bool raw{};
        bool dirty{};
        bool preview_pending{};
        double changed_at{};
        JsonValue status; // The latest shader report (errors, uniforms).
        std::string message;
        // The file is checked for changes made elsewhere (by an agent, say) once a second. A tab
        // without edits reloads; one with edits keeps them and holds the file's new text here
        // until the person chooses.
        double checked_at{};
        std::optional<std::string> changed_on_disk;
    };
    std::vector<ShaderTab> shader_tabs;
    std::size_t shader_tab_selected{};
    std::string shader_tab_select_request;
    std::string shader_tab_closing;
    bool shader_live_preview{true};
    bool shader_editor_focused{};
    bool shader_editor_focus_pending{};
    std::vector<std::string> shader_files;
    // Sky materials and the images they can show, for the Sky section's pickers, and each
    // material's settings and image status from assets.sky_material.
    std::vector<std::string> sky_material_files, panorama_files;
    std::map<std::string, JsonValue> sky_material_info;
    // Surface and post-processing materials as assets.material reports them; null for sky materials.
    std::map<std::string, JsonValue> shader_material_info;
    // Color fields with an edit under way, so a whole pick or drag is one undo step.
    std::set<ImGuiID> color_gestures;
    JsonValue audio_settings;
    JsonValue audio_status;
    std::map<std::string, JsonValue, std::less<>> audio_clip_info;
    std::array<char, 65> new_bus_name{};
    std::string bus_rename_target;
    std::array<char, 65> bus_rename{};
    // A bus volume being dragged, saved to the project when the drag ends.
    std::string bus_volume_target;
    double bus_volume_edit{};
    // The Mixer panel's selected effect: a bus name and an index into its chain, or -1.
    std::string mixer_bus;
    int mixer_effect{-1};
    // The frame rate limit field's text, kept while it is being typed in and saved when it is left.
    int frame_rate_limit_edit{};
    bool frame_rate_limit_editing{};
    std::array<char, 65> new_action_name{}, new_axis_name{};
    std::map<std::string, std::array<char, 65>> input_name_buffers;
    struct BindingCapture {
        enum class Target { action, pair_negative, pair_positive, analog };
        bool active{};
        Target target{Target::action};
        std::size_t index{};
        std::string negative;
        std::array<float, 4> zone{};
    } binding_capture;
    // During Run Game the viewport owns keyboard and mouse only after a click; Escape returns them.
    bool game_input_focus{}, game_lock_mouse{}, capture_requested{};
    std::string last_runtime_mode;
    std::map<std::string, std::array<char, 1025>> script_text_buffers;
    // The project's saved node trees (custom templates).
    struct TemplateEntry {
        std::string id, name, type;
        std::vector<std::string> components, behaviours;
    };
    std::vector<TemplateEntry> node_templates;
    std::string template_save_entity;
    std::array<char, 65> template_name{};
    bool open_template_dialog{}, template_replace{};
    bool asset_listing_truncated{}, assets_focused{}, hierarchy_focused{};
    // Hierarchy search: a name fragment and node types; either lists matching nodes flat.
    std::array<char, 65> hierarchy_query{};
    std::set<std::string> hierarchy_type_filters;
    bool hierarchy_search_focus{};
    // Applied to every row with children for one frame by the collapse/expand-all button.
    std::optional<bool> hierarchy_open_all;
    bool hierarchy_any_open{}, hierarchy_rows_open{};
    // "Show in hierarchy": ancestors to open and the row to scroll to on the next frame.
    std::set<std::string> hierarchy_reveal;
    std::string hierarchy_scroll_to;
    std::string assets_root = "assets";
    std::vector<std::string> undo_labels, redo_labels;
    std::array<bool, panel_names.size()> panel_open{default_panels};
    JsonValue agent_review, agent_audit, chat_status;
    std::array<char, 4001> chat_message{};
    std::array<char, 8003> chat_display{};
    ChatMedia chat_media;
    WrappedInput wrapped_chat;
    struct AttachmentInbox { std::mutex mutex; std::vector<std::string> paths; };
    std::shared_ptr<AttachmentInbox> attachment_inbox{std::make_shared<AttachmentInbox>()};
    AttachmentPicker attachment_picker;
    std::vector<std::string> attachments;
    bool chat_busy{};
    std::optional<std::array<float, 4>> composer_rect;
    bool drop_hover{};
    void receive_attachments() {
        std::lock_guard lock(attachment_inbox->mutex);
        for (auto& filename : attachment_inbox->paths) {
            if (attachments.size() >= 8) break;
            if (std::find(attachments.begin(), attachments.end(), filename) == attachments.end()) attachments.push_back(std::move(filename));
        }
        attachment_inbox->paths.clear();
    }
    void pick_attachments() {
        if (attachment_picker) {
            auto inbox = attachment_inbox;
            attachment_picker([inbox](std::vector<std::string> paths) { std::lock_guard lock(inbox->mutex); inbox->paths = std::move(paths); });
            return;
        }
        if (headless) return;
        auto* state = new std::shared_ptr<AttachmentInbox>(attachment_inbox);
        SDL_ShowOpenFileDialog([](void* userdata, const char* const* files, int) {
            std::unique_ptr<std::shared_ptr<AttachmentInbox>> inbox(static_cast<std::shared_ptr<AttachmentInbox>*>(userdata));
            if (!files) return;
            std::lock_guard lock((*inbox)->mutex);
            for (std::size_t i = 0; files[i] && i < 8; ++i) (*inbox)->paths.emplace_back(files[i]);
        }, state, sdl_window, nullptr, 0, nullptr, true);
    }
    std::array<char, 129> grant_target{}, audit_filename{"session-audit.jsonl"};
    int grant_method{}, grant_kind{};
    std::array<char, 2049> provider_endpoint{};
    std::array<char, 257> provider_model{};
    std::array<char, 129> provider_header{};
    std::array<char, 4097> provider_credential{};
    int provider_auth{};
    bool provider_loaded{}, provider_clear{}, agent_expand_pending{}, chat_follow{true};
    bool agent_account_pending{}, chat_jump_pending{};
    int agent_reasoning_index{};
    std::string agent_reasoning_model, agent_reasoning_effort;
    enum class FileAction { none, open, save_as, import, screenshot, recording, new_project, open_project, add_project_scene };
    FileAction file_action{FileAction::none};
    // What to carry out once the user has answered the unsaved-work prompt.
    enum class PendingAction { none, new_scene, open_scene, quit, new_project, open_project, project_scene };
    PendingAction pending_action{PendingAction::none};
    bool open_discard_dialog{false};
    bool open_file_dialog{false}, show_help{false}, show_about{false};
    std::array<char, 129> action_filename{};
    std::string dialog_error;
    std::uint64_t capture_serial{0};
    struct DeferredAction { std::string method, fields, success; };
    std::vector<DeferredAction> deferred_actions;
    bool defer_action(std::string method, std::string fields, std::string success) {
        deferred_actions.push_back({std::move(method), std::move(fields), std::move(success)});
        return true;
    }

    std::string request_line(const std::string_view method, const std::string_view fields) {
        std::string line = "{\"id\":" + std::to_string(next_request_id++) + ",\"method\":\"" +
                           std::string(method) + '"';
        if (!fields.empty()) {
            line += ',';
            line += fields;
        }
        return line + '}';
    }

    std::optional<JsonValue> call(const std::string_view method,
                                  const std::string_view fields = {},
                                  const bool report_errors = true) {
        return parse_response(method, request(request_line(method, fields)), report_errors);
    }

    // For large periodic reads: returns nothing when the reply matches `previous`, so an unchanged
    // scene listing or collider overlay is not parsed again every refresh.
    std::optional<JsonValue> call_if_changed(const std::string_view method, std::string& previous) {
        const auto response = request(request_line(method, {}));
        const auto body = response.find(",\"ok\":");
        const auto key = body == std::string::npos ? response : response.substr(body);
        if (key == previous) return std::nullopt;
        auto result = parse_response(method, response, true);
        previous = result ? key : std::string{};
        return result;
    }

    std::optional<JsonValue> parse_response(const std::string_view method,
                                            const std::string& response, const bool report_errors) {
        JsonParser parser{response};
        auto parsed = parser.parse();
        if (!parsed) {
            set_status("malformed response to " + std::string(method), true);
            return std::nullopt;
        }
        const auto* object = parsed->object();
        if (object == nullptr) {
            set_status("malformed response to " + std::string(method), true);
            return std::nullopt;
        }
        if (!boolean_or(*object, "ok", false)) {
            if (report_errors)
                set_status(std::string(method) + ": " + string_or(*object, "error", "failed"), true);
            return std::nullopt;
        }
        const auto* result = field(*object, "result");
        if (result) {
            if (const auto revision = response_revision(*result)) scene_revision = *revision;
        }
        return result == nullptr ? JsonValue{} : *result;
    }

    // Like call(), but returns the engine's refusal message instead of reporting it.
    std::string call_error(const std::string_view method, const std::string_view fields = {}) {
        const auto response = request(request_line(method, fields));
        JsonParser parser{response};
        const auto parsed = parser.parse();
        const auto* object = parsed ? parsed->object() : nullptr;
        if (!object) return "malformed response to " + std::string(method);
        if (boolean_or(*object, "ok", false)) {
            refresh_pending = true;
            return {};
        }
        return string_or(*object, "error", "failed");
    }

    const JsonValue::Object* scripts() const { return script_status.object(); }

    void refresh_game_input() {
        const auto* status = runtime_status.object();
        const auto mode = status ? string_or(*status, "mode") : std::string{};
        if (mode != "game") set_game_input_focus(false);
        if (mode == "game" && last_runtime_mode != "game") {
            const auto map = call("input.map", {}, false);
            const auto* object = map ? map->object() : nullptr;
            const auto* current = object ? field(*object, "map") : nullptr;
            game_lock_mouse = current && current->object() &&
                              boolean_or(*current->object(), "lock_mouse", false);
        }
        last_runtime_mode = mode;
        if (game_config_open && mode == "game") {
            if (auto state = call("input.state", {}, false)) input_live = std::move(*state);
        } else {
            input_live = JsonValue{};
        }
        if (game_config_open && game_config_page == 1) {
            if (auto settings = call("graphics.settings", {}, false))
                graphics_status = std::move(*settings);
        }
        refresh_audio_status();
    }

    void refresh_sky_files() {
        const auto search = [&](const char* kind) {
            std::vector<std::string> files;
            if (auto found = call("assets.search", std::string("\"kinds\":[\"") + kind + "\"]", false);
                found && found->object())
                if (const auto* entries = field(*found->object(), "entries"); entries && entries->array())
                    for (const auto& entry : *entries->array())
                        if (const auto* object = entry.object(); object && string_or(*object, "type") == "file")
                            files.push_back(string_or(*object, "path"));
            return files;
        };
        sky_material_files = search("material");
        panorama_files.clear();
        for (auto& file : search("image"))
            if (panorama_file(file)) panorama_files.push_back(std::move(file));
        // Read again, so a changed or newly added image shows up.
        sky_material_info.clear();
        shader_material_info.clear();
    }

    static bool panorama_file(const std::string_view path) {
        const auto dot = path.rfind('.');
        std::string extension(dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1U));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == "png" || extension == "jpg" || extension == "jpeg";
    }

    // A sky material's settings and whether its image loads, or null when it cannot be read.
    const JsonValue::Object* sky_material_summary(const std::string& path) {
        auto found = sky_material_info.find(path);
        if (found == sky_material_info.end()) {
            auto info = call("assets.sky_material", "\"path\":\"" + json_escape(path) + '"', false);
            found = sky_material_info.emplace(path, info ? std::move(*info) : JsonValue{}).first;
        }
        const auto* object = found->second.object();
        const auto* material = object ? field(*object, "material") : nullptr;
        return material ? material->object() : nullptr;
    }

    // A surface or post-processing material's shader, parameters and problems, or null.
    const JsonValue::Object* shader_material_summary(const std::string& path) {
        auto found = shader_material_info.find(path);
        if (found == shader_material_info.end()) {
            auto info = call("assets.material", "\"path\":\"" + json_escape(path) + '"', false);
            found = shader_material_info.emplace(path, info ? std::move(*info) : JsonValue{}).first;
        }
        const auto* object = found->second.object();
        const auto* material = object ? field(*object, "material") : nullptr;
        return material ? material->object() : nullptr;
    }

    std::vector<std::string> shader_materials_of_type(const std::string_view type) {
        std::vector<std::string> paths;
        for (const auto& path : sky_material_files)
            if (const auto* material = shader_material_summary(path); material && string_or(*material, "type") == type)
                paths.push_back(path);
        return paths;
    }

    // Material edits write the file rather than the scene, so a drag leaves no undo gesture behind.
    void set_material_parameter(const std::string& path, const std::string& fields) {
        pending_gesture = 0U;
        if (call("assets.set_material_parameter", "\"path\":\"" + json_escape(path) + "\"" + fields))
            shader_material_info.erase(path);
    }

    // One shader parameter's field. `apply` receives the request fields for a new value
    // (",\"name\":...,\"value\":[...]" or an image's ",\"texture\":..."); `reset`, when given, shows a
    // Reset button. `own` replaces the value shown (an object's own value); `key` prefixes the
    // headless test keys.
    void draw_material_parameter(const JsonValue::Object& parameter, const std::function<void(const std::string&)>& apply,
                                 const std::function<void()>& reset, const std::vector<double>* own,
                                 const std::string& key, const char* reset_tip) {
        const auto name = string_or(parameter, "name");
        const auto type = string_or(parameter, "type");
        const auto hint = string_or(parameter, "hint");
        ImGui::PushID(name.c_str());
        std::array<double, 4> values{};
        std::size_t count = 0;
        if (const auto* list = field(parameter, "value"); list && list->array())
            for (const auto& item : *list->array())
                if (item.number() && count < values.size()) values[count++] = *item.number();
        if (own)
            for (std::size_t index = 0; index < std::min(own->size(), values.size()); ++index) values[index] = (*own)[index];
        // Room for the Reset button beside the field.
        if (reset)
            inspector_field_reserve = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2.0F +
                                      ImGui::GetStyle().ItemSpacing.x;
        const auto send = [&](const std::size_t components) {
            std::string fields = ",\"name\":\"" + json_escape(name) + "\",\"value\":[";
            for (std::size_t index = 0; index < components; ++index)
                fields += (index ? "," : "") + number_text(values[index]);
            apply(fields + ']');
        };
        const auto label = name.c_str();
        if (type == "sampler2D") {
            const auto texture = string_or(parameter, "texture");
            const char* fallback = hint == "hint_normal" ? "Flat normal" : hint == "hint_black" ? "Black" : "White";
            // Copied into the pick, which may be chosen frames later.
            const auto set_texture = [apply = apply, name](const std::string& file) {
                apply(",\"name\":\"" + json_escape(name) + "\",\"texture\":\"" + json_escape(file) + '"');
            };
            asset_field(
                label, texture, key + name,
                [=, this] {
                    AssetPick pick;
                    pick.key = key + name;
                    pick.title = "Choose an image for " + name;
                    pick.current = texture;
                    pick.kinds = {"image"};
                    pick.accepts = [](const std::string& path) { return panorama_file(path); };
                    pick.what = "a PNG or JPEG image";
                    pick.choices.push_back({"", std::string("Default: ") + fallback, AssetIcon::image, "", "", std::nullopt});
                    pick.choose = set_texture;
                    return pick;
                },
                fallback, AssetIcon::image);
        } else if (type == "bool") {
            bool value = values[0] != 0.0;
            inspector_field_label(label);
            if (editor_checkbox("##value", &value)) {
                values[0] = value ? 1.0 : 0.0;
                send(1U);
            }
            note_item(key + name);
        } else if ((type == "vec3" || type == "vec4") && hint == "source_color") {
            std::array<double, 3> color{values[0], values[1], values[2]};
            if (color_edit(label, color)) {
                values[0] = color[0];
                values[1] = color[1];
                values[2] = color[2];
                send(type == "vec4" ? 4U : 3U);
            }
            note_item(key + name);
            if (type == "vec4" && drag_scalar("Alpha", values[3], 0.005F, "%.3f")) {
                values[3] = std::clamp(values[3], 0.0, 1.0);
                send(4U);
            }
        } else if (type == "float" || type == "int") {
            const bool integer = type == "int";
            if (hint == "hint_range") {
                const double minimum = number_or(parameter, "minimum", 0.0);
                const double maximum = number_or(parameter, "maximum", 1.0);
                const double step = number_or(parameter, "step", 0.0);
                const auto format = integer ? std::string{"%.0f"} : slider_format(minimum, maximum, step);
                if (slider_scalar(label, values[0], minimum, maximum, format.c_str())) {
                    if (step > 0.0)
                        values[0] = minimum + std::round((values[0] - minimum) / step) * step;
                    if (integer) values[0] = std::round(values[0]);
                    send(1U);
                }
            } else if (drag_scalar(label, values[0], integer ? 0.2F : 0.01F, integer ? "%.0f" : "%.3f")) {
                if (integer) values[0] = std::round(values[0]);
                send(1U);
            }
            note_item(key + name);
        } else {
            const auto components = type == "vec2" ? 2 : type == "vec3" ? 3 : 4;
            inspector_field_label(label);
            if (ImGui::DragScalarN("##value", ImGuiDataType_Double, values.data(), components, 0.01F, nullptr,
                                   nullptr, "%.3f"))
                send(static_cast<std::size_t>(components));
            note_item(key + name);
        }
        inspector_field_reserve = 0.0F;
        if (reset) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) reset();
            note_item(key + name + ":reset");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", reset_tip);
        }
        ImGui::PopID();
    }

    // A material file's parameter, saved to the file as it changes.
    void draw_material_parameter(const std::string& path, const JsonValue::Object& parameter) {
        const auto name = string_or(parameter, "name");
        draw_material_parameter(
            parameter, [this, path](const std::string& fields) { set_material_parameter(path, fields); },
            boolean_or(parameter, "set", false)
                ? std::function<void()>([&] {
                      set_material_parameter(path, ",\"name\":\"" + json_escape(name) + "\",\"reset\":true");
                  })
                : std::function<void()>{},
            nullptr, "inspector:material:", "Back to the shader's default");
    }

    // A surface material on a sphere, rendered by the window a frame or two after it changes.
    void draw_material_preview(const std::string& path) {
        preview_wanted = path;
        const float side = std::min(ImGui::GetContentRegionAvail().x, 144.0F * ui_scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - side) * 0.5F);
        if (preview_texture && preview_path == path) {
            if (headless) {
                preview_texture->SetTexID(1);
                preview_texture->SetStatus(ImTextureStatus_OK);
            } else if (preview_texture->Status == ImTextureStatus_Destroyed) {
                preview_texture->SetStatus(ImTextureStatus_WantCreate);
            }
            ImGui::Image(preview_texture->GetTexRef(), {side, side});
        } else {
            const auto corner = ImGui::GetCursorScreenPos();
            ImGui::Dummy({side, side});
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(corner, {corner.x + side, corner.y + side}, ImGui::GetColorU32(editor_color(editor_palette().surface_active)));
            const char* label = "Rendering preview...";
            const auto text = ImGui::CalcTextSize(label);
            draw->AddText({corner.x + (side - text.x) * 0.5F, corner.y + (side - text.y) * 0.5F},
                          ImGui::GetColorU32(editor_color(editor_palette().text_faint)), label);
        }
        note_item("inspector:material:preview");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s on a sphere, under Relay's default sky and sun. Values set for one object "
                              "alone are not shown here.", base_name(path).c_str());
    }

    void material_preview_ready(const std::string& path, const std::uint32_t width, const std::uint32_t height,
                                std::vector<std::uint8_t> rgba) {
        if (width == 0U || height == 0U || rgba.size() != static_cast<std::size_t>(width) * height * 4U) return;
        // Kept as delivered for thumbnails: the renderer draws a material again only when it
        // changes, so a thumbnail of what it drew last is taken from here.
        last_preview = {path, width, height, rgba};
        thumbnails.store_material(path, width, height, rgba);
        // ImGui textures are sampled as linear values on the sRGB swapchain.
        srgb_rows_to_linear(rgba);
        preview_path = path;
        const bool reuse = preview_texture && preview_texture->Width == static_cast<int>(width) &&
                           preview_texture->Height == static_cast<int>(height) &&
                           preview_texture->Status != ImTextureStatus_WantDestroy;
        if (!reuse) {
            if (preview_texture) ImGui::UnregisterUserTexture(preview_texture.get());
            preview_texture = std::make_unique<ImTextureData>();
            preview_texture->Create(ImTextureFormat_RGBA32, static_cast<int>(width), static_cast<int>(height));
            preview_texture->RefCount = 1;
            std::memcpy(preview_texture->Pixels, rgba.data(), rgba.size());
            preview_texture->SetStatus(ImTextureStatus_WantCreate);
            ImGui::RegisterUserTexture(preview_texture.get());
            return;
        }
        std::memcpy(preview_texture->Pixels, rgba.data(), rgba.size());
        if (preview_texture->Status == ImTextureStatus_OK) {
            const ImTextureRect whole{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
            preview_texture->UpdateRect = whole;
            preview_texture->Updates.resize(0);
            preview_texture->Updates.push_back(whole);
            preview_texture->SetStatus(ImTextureStatus_WantUpdates);
        }
    }

    // A surface or post-processing material's shader and parameters, saved to its file as they
    // change. `post` picks which shaders it may use.
    void draw_shader_material_fields(const std::string& path, const bool post) {
        const auto& palette = editor_palette();
        const auto* material = shader_material_summary(path);
        if (!material) {
            ImGui::TextColored(editor_color(palette.warning), "%s cannot be read", path.c_str());
            return;
        }
        ImGui::PushID(path.c_str());
        if (!post) draw_material_preview(path);
        const auto shader = string_or(*material, "shader");
        const auto set_shader = [this, path](const std::string& chosen) {
            if (call("assets.set_material", "\"path\":\"" + json_escape(path) + "\",\"shader\":\"" +
                                                json_escape(chosen) + '"'))
                shader_material_info.erase(path);
        };
        asset_field(
            "Shader", shader, "inspector:material:shader",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:material:shader";
                pick.title = "Choose a shader for " + asset_display_name(path);
                pick.current = shader;
                pick.kinds = {"shader"};
                pick.accepts = [](const std::string& file) { return file.ends_with(".relay-shader"); };
                pick.actions.emplace_back(post ? "New post-processing shader" : "New surface shader", [this, post, set_shader] {
                    (void)call("assets.create_folder", "\"path\":\"shaders\"", false);
                    if (const auto created = create_shader("shaders", post)) set_shader(*created);
                });
                pick.choose = set_shader;
                return pick;
            },
            "None", AssetIcon::shader);
        if (!shader.empty()) {
            inspector_field_label("");
            if (ImGui::SmallButton("Edit shader")) open_shader(shader);
            note_item("inspector:material:edit_shader");
        }
        if (const auto* error = field(*material, "error"); error && error->string()) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.warning));
            ImGui::TextWrapped("%s%s", error->string()->c_str(), post ? "" : "; the surface shows a checkerboard");
            ImGui::PopStyleColor();
        }
        if (const auto* warnings = field(*material, "warnings"); warnings && warnings->array())
            for (const auto& warning : *warnings->array())
                if (warning.string()) ImGui::TextColored(editor_color(palette.text_faint), "%s", warning.string()->c_str());
        if (const auto* parameters = field(*material, "parameters"); parameters && parameters->array())
            for (const auto& item : *parameters->array())
                if (const auto* parameter = item.object()) draw_material_parameter(path, *parameter);
        ImGui::PopID();
    }

    void draw_post_process_section(const JsonValue::Object& entity) {
        const auto* post = component(entity, "post_process");
        if (!post || !component_header("Post process", "post_process")) return;
        const auto& palette = editor_palette();
        const auto request_fields = entity_field(selection);
        std::vector<std::string> materials;
        std::vector<bool> enabled, in_editor;
        if (const auto* effects = field(*post, "effects"); effects && effects->array())
            for (const auto& item : *effects->array())
                if (const auto* effect = item.object()) {
                    materials.push_back(string_or(*effect, "material"));
                    enabled.push_back(boolean_or(*effect, "enabled", true));
                    in_editor.push_back(boolean_or(*effect, "editor", true));
                }
        const auto set_list = [this, request_fields](const std::vector<std::string>& list, const char* label) {
            std::string fields = request_fields + ",\"effects\":[";
            for (std::size_t index = 0; index < list.size(); ++index)
                fields += (index ? ",\"" : "\"") + json_escape(list[index]) + '"';
            mutate("scene.set_post_process", fields + ']', label);
        };
        std::size_t nodes = 0;
        bool used = false;
        for (const auto* other : entities)
            if (component(*other, "post_process")) {
                if (nodes++ == 0U) used = string_or(*other, "entity") == selection;
            }
        if (nodes > 1U)
            ImGui::TextColored(editor_color(palette.warning),
                               used ? "%zu nodes have post processing; this one is used."
                                    : "%zu nodes have post processing; only the first is used, not this one.",
                               nodes);
        if (materials.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            ImGui::TextWrapped("No effects yet. Add a post-processing material below; its shader changes the "
                               "whole image before tone mapping, in the editor and the game.");
            ImGui::PopStyleColor();
        }
        for (std::size_t index = 0; index < materials.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            bool on = enabled[index];
            if (editor_checkbox("##on", &on))
                mutate("scene.set_post_effect",
                       request_fields + ",\"index\":" + std::to_string(index) + ",\"enabled\":" + (on ? "true" : "false"),
                       on ? "Effect on" : "Effect off");
            note_item("inspector:post:enabled:" + std::to_string(index));
            ImGui::SameLine();
            const bool open = ImGui::TreeNodeEx("##effect", ImGuiTreeNodeFlags_SpanAvailWidth, "%s",
                                                asset_display_name(materials[index]).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", materials[index].c_str());
            note_item("inspector:post:effect:" + std::to_string(index));
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 3.0F * ImGui::GetFrameHeight());
            ImGui::BeginDisabled(index == 0U);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                auto list = materials;
                std::swap(list[index], list[index - 1U]);
                set_list(list, "Effect moved");
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0.0F, 2.0F);
            ImGui::BeginDisabled(index + 1U == materials.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                auto list = materials;
                std::swap(list[index], list[index + 1U]);
                set_list(list, "Effect moved");
            }
            ImGui::EndDisabled();
            ImGui::SameLine(0.0F, 2.0F);
            if (ImGui::SmallButton("x")) {
                auto list = materials;
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                set_list(list, "Effect removed");
            }
            note_item("inspector:post:remove:" + std::to_string(index));
            if (open) {
                bool shown = in_editor[index];
                inspector_field_label("Editor view");
                if (editor_checkbox("##editor_view", &shown))
                    mutate("scene.set_post_effect",
                           request_fields + ",\"index\":" + std::to_string(index) + ",\"editor\":" +
                               (shown ? "true" : "false"),
                           shown ? "Effect shown in the editor view" : "Effect kept to the game camera");
                note_item("inspector:post:editor:" + std::to_string(index));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Also show this effect while looking around in the editor. The game "
                                      "camera's view always shows it.");
                draw_shader_material_fields(materials[index], true);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        const auto add = [materials, set_list](const std::string& path) {
            auto list = materials;
            list.push_back(path);
            set_list(list, "Effect added");
        };
        asset_add_button(
            "+ Add effect...", "inspector:post:add",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:post:add";
                pick.title = "Add a post-processing effect";
                pick.kinds = {"material"};
                pick.accepts = [this](const std::string& path) { return material_facts_of(path).type == "post_process"; };
                pick.what = "a post-processing material";
                for (const auto& effect : shader_effects())
                    pick.choices.push_back({"@ready:" + std::string{effect.id}, std::string{effect.name}, AssetIcon::post_material,
                                            "@ready",
                                            std::string{effect.description} +
                                                ". Copied into the project's shaders and materials folders, where you can "
                                                "change it like any other shader.",
                                            std::nullopt});
                pick.actions.emplace_back("New post-processing material", [this, add] {
                    (void)call("assets.create_folder", "\"path\":\"materials\"", false);
                    if (const auto created = create_material("materials", true)) add(*created);
                });
                pick.choose = [this, add](const std::string& value) {
                    if (!value.starts_with("@ready:")) return add(value);
                    for (const auto& effect : shader_effects())
                        if ("@ready:" + std::string{effect.id} == value)
                            if (const auto material = add_ready_made_effect(effect)) add(*material);
                };
                return pick;
            });
    }

    // A ready-made effect's shader and material in the project (shaders/Bloom.relay-shader and
    // materials/Bloom.relay-material, say), made from Relay's copy unless they already exist.
    // Returns the material.
    std::optional<std::string> add_ready_made_effect(const ShaderEffect& effect) {
        const std::string name{effect.name};
        const auto shader = "shaders/" + name + ".relay-shader";
        const auto material = "materials/" + name + ".relay-material";
        if (std::find(shader_files.begin(), shader_files.end(), shader) == shader_files.end()) {
            (void)call("assets.create_folder", "\"path\":\"shaders\"", false);
            if (!call("shaders.write", "\"path\":\"" + json_escape(shader) + "\",\"create\":true,\"effect\":\"" +
                                           std::string{effect.id} + '"'))
                return std::nullopt;
            refresh_shader_files();
        }
        if (std::find(sky_material_files.begin(), sky_material_files.end(), material) == sky_material_files.end()) {
            (void)call("assets.create_folder", "\"path\":\"materials\"", false);
            if (!call("assets.set_material", "\"path\":\"" + json_escape(material) +
                                                 "\",\"create\":true,\"type\":\"post_process\",\"shader\":\"" +
                                                 json_escape(shader) + '"'))
                return std::nullopt;
            refresh_sky_files();
        }
        refresh_asset_listing();
        return material;
    }

    // Creates "New sky material.relay-material" (numbered when taken) in `folder`. With `sky`
    // set, that node's sky shows it; otherwise the new file is selected for renaming.
    void create_sky_material(const std::string& folder, const std::string& sky = {}) {
        const auto taken = [&](const std::string& path) {
            return std::find(sky_material_files.begin(), sky_material_files.end(), path) !=
                   sky_material_files.end();
        };
        std::string path = join_path(folder, "New sky material.relay-material");
        for (int suffix = 2; taken(path); ++suffix)
            path = join_path(folder, "New sky material " + std::to_string(suffix) + ".relay-material");
        if (!call("assets.set_sky_material", "\"path\":\"" + json_escape(path) + "\",\"create\":true"))
            return;
        refresh_sky_files();
        refresh_asset_listing();
        if (!sky.empty()) {
            mutate("scene.set_sky", entity_field(sky) + ",\"material\":\"" + json_escape(path) + '"',
                   "Created " + path);
        } else {
            if (!folder.empty()) set_asset_folder_open(folder, true);
            asset_selection = path;
            begin_rename(RenameKind::asset, path, base_name(path));
        }
    }

    // Relay's shading language, for the code boxes: GLSL plus its declarations and built-ins.
    static const TextEditor::Language* relay_shader_language() {
        static const TextEditor::Language language = [] {
            auto relay = *TextEditor::Language::Glsl();
            relay.name = "Relay shader";
            for (const char* keyword : {"shader_type", "render_mode", "surface", "post_process",
                                        "transparent", "unshaded", "source_color", "hint_range",
                                        "hint_normal", "hint_black", "hint_white"})
                relay.keywords.insert(keyword);
            for (const char* builtin :
                 {"VERTEX", "NORMAL", "TANGENT", "UV", "TIME", "MODEL_MATRIX", "WORLD_POSITION", "VIEW",
                  "CAMERA_POSITION", "FRAGCOORD", "FRONT_FACING", "ALBEDO", "ALPHA", "METALLIC",
                  "ROUGHNESS", "EMISSION", "AO", "NORMAL_MAP", "NORMAL_MAP_DEPTH", "ALPHA_SCISSOR_THRESHOLD",
                  "SCREEN_UV", "SCREEN_PIXEL_SIZE", "COLOR", "DEPTH", "SCREEN_TEXTURE", "MOTION", "DELTA_TIME",
                  "scene_color", "scene_depth", "scene_normal", "scene_motion", "vertex", "fragment"})
                relay.identifiers.insert(builtin);
            return relay;
        }();
        return &language;
    }

    static std::unique_ptr<TextEditor> make_code_editor(const std::string& text) {
        auto editor = std::make_unique<TextEditor>();
        editor->SetLanguage(relay_shader_language());
        editor->SetTabSize(4);
        editor->SetShowWhitespacesEnabled(false);
        editor->SetPalette(TextEditor::GetDarkPalette());
        editor->SetText(text);
        return editor;
    }

    // The text without its trailing layout comment: what compiles.
    static std::string shader_code_part(const std::string& text) {
        const auto at = text.rfind("\n// relay-graph ");
        return at == std::string::npos ? text : text.substr(0U, at + 1U);
    }

    // Reads text into a tab: as a graph, or as text when its declarations do not parse.
    void load_shader_text(ShaderTab& tab, const std::string& text) {
        tab.text = text;
        tab.graph = shader_to_graph(text);
        tab.raw = !tab.graph.errors.empty();
        tab.source = tab.raw ? GraphSource{} : graph_to_shader(tab.graph);
        if (tab.raw) {
            tab.code_editor = make_code_editor(text);
            tab.code_undo = tab.code_editor->GetUndoIndex();
            tab.functions_editor.reset();
        } else {
            tab.functions_editor = make_code_editor(tab.graph.functions);
            tab.functions_undo = tab.functions_editor->GetUndoIndex();
            refresh_stage_code_editor(tab);
        }
        if (tab.graph.type == ShaderType::post_process) tab.stage = "fragment";
        tab.dirty = tab.text != tab.saved;
    }

    void refresh_stage_code_editor(ShaderTab& tab) {
        const auto* stage = tab.graph.stage(tab.stage);
        if (stage && stage->code_only) {
            tab.code_editor = make_code_editor(stage->code);
            tab.code_undo = tab.code_editor->GetUndoIndex();
        } else {
            tab.code_editor.reset();
        }
    }

    // Writes the graph back to text after an edit. `committed` edits are remembered for undo and
    // previewed; others (a node being dragged) only update the text.
    void graph_changed(ShaderTab& tab, const bool committed) {
        if (auto* stage = tab.graph.stage(tab.stage); stage && !stage->code_only)
            (void)update_graph_types(tab.graph, *stage);
        tab.source = graph_to_shader(tab.graph);
        const bool code_changed = shader_code_part(tab.source.text) != shader_code_part(tab.text);
        tab.text = tab.source.text;
        tab.dirty = tab.text != tab.saved;
        if (!committed) return;
        remember_shader_text(tab);
        if (code_changed) {
            tab.preview_pending = true;
            tab.changed_at = ImGui::GetTime();
        }
    }

    void remember_shader_text(ShaderTab& tab) {
        if (!tab.history.empty() && tab.history[tab.history_index] == tab.text) return;
        if (!tab.history.empty()) tab.history.resize(tab.history_index + 1U);
        tab.history.push_back(tab.text);
        if (tab.history.size() > 200U) tab.history.erase(tab.history.begin());
        tab.history_index = tab.history.size() - 1U;
    }

    void restore_shader_text(ShaderTab& tab, const std::size_t index) {
        if (index >= tab.history.size()) return;
        tab.history_index = index;
        const auto stage = tab.stage;
        load_shader_text(tab, tab.history[index]);
        tab.stage = stage;
        refresh_stage_code_editor(tab);
        tab.preview_pending = true;
        tab.changed_at = ImGui::GetTime();
    }

    // Opens a shader in the Shader Editor, or brings its tab forward.
    void open_shader(const std::string& path) {
        panel_open[11] = true;
        shader_editor_focus_pending = true;
        shader_tab_select_request = path;
        if (std::any_of(shader_tabs.begin(), shader_tabs.end(), [&](const ShaderTab& tab) { return tab.path == path; }))
            return;
        auto read = call("shaders.read", "\"path\":\"" + json_escape(path) + '"');
        const auto* object = read && read->object() ? field(*read->object(), "shader") : nullptr;
        if (!object || !object->object()) return;
        ShaderTab tab;
        tab.path = path;
        tab.saved = string_or(*object->object(), "text");
        const auto* preview = field(*object->object(), "preview");
        const auto text = preview && preview->string() ? *preview->string() : tab.saved;
        tab.previewed = preview && preview->string() ? *preview->string() : std::string{};
        tab.status = *object;
        load_shader_text(tab, text);
        remember_shader_text(tab);
        shader_tabs.push_back(std::move(tab));
    }

    void set_shader_status(ShaderTab& tab, const std::optional<JsonValue>& result) {
        const auto* object = result && result->object() ? field(*result->object(), "shader") : nullptr;
        if (object && object->object()) tab.status = *object;
        if (tab.code_editor) {
            // Code boxes mark their own errors: the whole file in text mode, else a code stage's lines.
            tab.code_editor->ClearMarkers();
            if (const auto* errors = field(*tab.status.object(), "errors"); errors && errors->array() && tab.raw)
                for (const auto& item : *errors->array())
                    if (const auto* error = item.object()) {
                        const auto line = static_cast<std::size_t>(number_or(*error, "line", 0.0));
                        const auto message = string_or(*error, "message");
                        if (line > 0U && line <= tab.code_editor->GetLineCount())
                            tab.code_editor->AddMarker(line - 1U, IM_COL32(230, 80, 80, 255), IM_COL32(230, 80, 80, 60),
                                                       message, message);
                    }
        }
        refresh_pending = true;
    }

    // Compile errors, by the node whose code they are in; the rest are listed for the whole file.
    std::map<std::string, std::string> shader_node_errors(const ShaderTab& tab, std::vector<std::string>& others) const {
        std::map<std::string, std::string> result;
        const auto* status = tab.status.object();
        const auto* errors = status ? field(*status, "errors") : nullptr;
        if (!errors || !errors->array()) return result;
        for (const auto& item : *errors->array()) {
            const auto* error = item.object();
            if (!error) continue;
            const auto line = static_cast<std::uint32_t>(number_or(*error, "line", 0.0));
            const auto message = string_or(*error, "message");
            const auto found = tab.source.node_lines.find(line);
            if (found != tab.source.node_lines.end() && found->second.first == tab.stage) {
                auto& text = result[found->second.second];
                text += (text.empty() ? "" : "\n") + message;
            } else if (found == tab.source.node_lines.end() || found->second.first == tab.stage) {
                others.push_back((line ? "Line " + std::to_string(line) + ": " : std::string{}) + message);
            } else {
                others.push_back("In the " + found->second.first + " stage: " + message);
            }
        }
        return result;
    }

    bool save_shader(ShaderTab& tab) {
        auto result = call("shaders.write", "\"path\":\"" + json_escape(tab.path) + "\",\"text\":\"" +
                                                json_escape(tab.text) + '"');
        if (!result) return false;
        tab.saved = tab.text;
        tab.previewed.clear();
        tab.dirty = false;
        tab.preview_pending = false;
        set_shader_status(tab, result);
        set_status("Saved " + tab.path, false);
        return true;
    }

    // Stops previewing unsaved text, so the scene shows the file again.
    void discard_shader_preview(ShaderTab& tab) {
        if (!tab.previewed.empty() || tab.dirty)
            (void)call("shaders.preview", "\"path\":\"" + json_escape(tab.path) + '"', false);
        tab.previewed.clear();
        refresh_pending = true;
    }

    // Creates a shader of the given type in `folder` from its template and opens it.
    std::optional<std::string> create_shader(const std::string& folder, const bool post) {
        const auto taken = [&](const std::string& path) {
            return std::find(shader_files.begin(), shader_files.end(), path) != shader_files.end();
        };
        const std::string stem = post ? "New post-processing shader" : "New surface shader";
        std::string path = join_path(folder, stem + ".relay-shader");
        for (int suffix = 2; taken(path); ++suffix)
            path = join_path(folder, stem + ' ' + std::to_string(suffix) + ".relay-shader");
        if (!call("shaders.write", "\"path\":\"" + json_escape(path) + "\",\"create\":true,\"type\":\"" +
                                       (post ? "post_process" : "surface") + '"'))
            return std::nullopt;
        refresh_shader_files();
        refresh_asset_listing();
        open_shader(path);
        return path;
    }

    // Creates a surface or post-processing material in `folder`. With `node` set it is given to
    // that node (its mesh renderer, or its post processing); otherwise it is selected for renaming.
    std::optional<std::string> create_material(const std::string& folder, const bool post,
                                               const std::string& shader = {}) {
        const auto taken = [&](const std::string& path) {
            return std::find(sky_material_files.begin(), sky_material_files.end(), path) !=
                   sky_material_files.end();
        };
        const std::string stem = post ? "New post-processing material" : "New material";
        std::string path = join_path(folder, stem + ".relay-material");
        for (int suffix = 2; taken(path); ++suffix)
            path = join_path(folder, stem + ' ' + std::to_string(suffix) + ".relay-material");
        if (!call("assets.set_material", "\"path\":\"" + json_escape(path) + "\",\"create\":true,\"type\":\"" +
                                             (post ? "post_process" : "surface") + "\",\"shader\":\"" +
                                             json_escape(shader) + '"'))
            return std::nullopt;
        refresh_sky_files();
        refresh_asset_listing();
        return path;
    }

    void refresh_shader_files() {
        shader_files.clear();
        if (auto found = call("assets.search", "\"kinds\":[\"shader\"]", false); found && found->object())
            if (const auto* entries = field(*found->object(), "entries"); entries && entries->array())
                for (const auto& entry : *entries->array())
                    if (const auto* object = entry.object();
                        object && string_or(*object, "type") == "file" &&
                        string_or(*object, "path").ends_with(".relay-shader"))
                        shader_files.push_back(string_or(*object, "path"));
    }

    void draw_shader_editor() {
        const auto& palette = editor_palette();
        shader_editor_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (shader_tabs.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            ImGui::TextWrapped("Double-click a .relay-shader file in Assets to edit its nodes here, or start a new "
                               "one. Surface shaders decide how meshes look; post-processing shaders "
                               "change the whole image.");
            ImGui::PopStyleColor();
            if (ImGui::Button("New surface shader")) {
                (void)call("assets.create_folder", "\"path\":\"shaders\"", false);
                (void)create_shader("shaders", false);
            }
            note_item("shader_editor:new_surface");
            ImGui::SameLine();
            if (ImGui::Button("New post-processing shader")) {
                (void)call("assets.create_folder", "\"path\":\"shaders\"", false);
                (void)create_shader("shaders", true);
            }
            note_item("shader_editor:new_post");
            return;
        }
        const double now = ImGui::GetTime();
        if (ImGui::BeginTabBar("##shader_tabs", ImGuiTabBarFlags_AutoSelectNewTabs |
                                                    ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (std::size_t index = 0; index < shader_tabs.size(); ++index) {
                auto& tab = shader_tabs[index];
                bool open = true;
                const auto label = base_name(tab.path) + (tab.dirty ? " *" : "") + "###" + tab.path;
                ImGuiTabItemFlags flags = tab.dirty ? ImGuiTabItemFlags_UnsavedDocument : 0;
                if (shader_tab_select_request == tab.path) {
                    flags |= ImGuiTabItemFlags_SetSelected;
                    shader_tab_select_request.clear();
                }
                const bool visible = ImGui::BeginTabItem(label.c_str(), &open, flags);
                note_item("shader_editor:tab:" + tab.path);
                if (!open) {
                    if (tab.dirty) shader_tab_closing = tab.path;
                    else {
                        discard_shader_preview(tab);
                        shader_tabs.erase(shader_tabs.begin() + static_cast<std::ptrdiff_t>(index));
                        if (visible) ImGui::EndTabItem();
                        break;
                    }
                }
                if (now - tab.checked_at >= 1.0) {
                    tab.checked_at = now;
                    check_shader_file(tab);
                }
                if (!visible) continue;
                shader_tab_selected = index;
                draw_shader_tab(tab, now);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        // Tabs with changes wait here for a choice before they close.
        if (!shader_tab_closing.empty()) ImGui::OpenPopup("Close shader");
        if (ImGui::BeginPopupModal("Close shader", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto found = std::find_if(shader_tabs.begin(), shader_tabs.end(),
                                            [&](const ShaderTab& tab) { return tab.path == shader_tab_closing; });
            ImGui::Text("%s has changes that are not saved.", shader_tab_closing.c_str());
            const auto close = [&](const bool save) {
                if (found != shader_tabs.end()) {
                    if (!save || save_shader(*found)) {
                        if (!save) discard_shader_preview(*found);
                        shader_tabs.erase(found);
                    }
                }
                shader_tab_closing.clear();
                ImGui::CloseCurrentPopup();
            };
            if (ImGui::Button("Save")) close(true);
            note_item("shader_editor:close:save");
            ImGui::SameLine();
            if (ImGui::Button("Discard")) close(false);
            note_item("shader_editor:close:discard");
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                shader_tab_closing.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // Picks up a shader file changed outside this tab: reloads a tab without edits (the old text
    // stays in its undo history), or remembers the new text for a tab with edits.
    void check_shader_file(ShaderTab& tab) {
        const auto read = call("shaders.read", "\"path\":\"" + json_escape(tab.path) + '"', false);
        const auto* object = read && read->object() ? field(*read->object(), "shader") : nullptr;
        if (!object || !object->object()) return;
        const auto text = string_or(*object->object(), "text");
        if (text == tab.saved) {
            tab.changed_on_disk.reset();
            return;
        }
        if (tab.dirty) {
            tab.changed_on_disk = text;
            return;
        }
        reload_shader_from_disk(tab, text);
    }

    // `text` is a copy: it may come from tab.changed_on_disk, which this clears.
    void reload_shader_from_disk(ShaderTab& tab, const std::string text) {
        tab.saved = text;
        tab.changed_on_disk.reset();
        load_shader_text(tab, text);
        remember_shader_text(tab);
        tab.preview_pending = false;
        discard_shader_preview(tab);
        set_shader_status(tab, call("shaders.read", "\"path\":\"" + json_escape(tab.path) + '"', false));
        tab.message = "Reloaded: the file was changed outside the editor";
    }

    // A code box for code the graph cannot show; returns true when it was edited this frame.
    bool draw_shader_code_box(TextEditor& editor, std::size_t& undo_index, const char* id, const ImVec2 size) {
        ImGui::PushFont(fonts.monospace, fonts.monospace_size);
        editor.Render(id, size, ImGuiChildFlags_Borders);
        ImGui::PopFont();
        if (editor.GetUndoIndex() == undo_index) return false;
        undo_index = editor.GetUndoIndex();
        return true;
    }

    void draw_shader_tab(ShaderTab& tab, const double now) {
        const auto& palette = editor_palette();
        auto& io = ImGui::GetIO();
        if (tab.changed_on_disk) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.warning));
            ImGui::TextWrapped("%s was changed outside the editor (by an agent or another program) while you "
                               "have unsaved changes here.", base_name(tab.path).c_str());
            ImGui::PopStyleColor();
            if (ImGui::Button("Load the new version")) reload_shader_from_disk(tab, *tab.changed_on_disk);
            note_item("shader_editor:disk:reload");
            ImGui::SameLine();
            if (tab.changed_on_disk && ImGui::Button("Keep mine")) {
                // Saving now replaces the other version, as the person chose.
                tab.saved = *tab.changed_on_disk;
                tab.changed_on_disk.reset();
            }
            note_item("shader_editor:disk:keep");
        }
        const bool code_box_focused = ImGui::GetActiveID() != 0 && io.WantTextInput;
        // Toolbar: save, revert, undo, preview and the compile status.
        const bool save_pressed = shader_editor_focused && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false);
        ImGui::BeginDisabled(!tab.dirty);
        if (ImGui::Button("Save") || (save_pressed && tab.dirty)) (void)save_shader(tab);
        ImGui::EndDisabled();
        note_item("shader_editor:save");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Save (Ctrl+S)");
        ImGui::SameLine();
        ImGui::BeginDisabled(!tab.dirty);
        if (ImGui::Button("Revert")) {
            load_shader_text(tab, tab.saved);
            remember_shader_text(tab);
            tab.preview_pending = false;
            discard_shader_preview(tab);
            set_shader_status(tab, call("shaders.read", "\"path\":\"" + json_escape(tab.path) + '"', false));
        }
        ImGui::EndDisabled();
        note_item("shader_editor:revert");
        ImGui::SameLine();
        const bool can_undo = tab.history_index > 0U, can_redo = tab.history_index + 1U < tab.history.size();
        ImGui::BeginDisabled(!can_undo);
        if (ImGui::ArrowButton("##undo", ImGuiDir_Left)) restore_shader_text(tab, tab.history_index - 1U);
        ImGui::EndDisabled();
        note_item("shader_editor:undo");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Undo (Ctrl+Z)");
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::BeginDisabled(!can_redo);
        if (ImGui::ArrowButton("##redo", ImGuiDir_Right)) restore_shader_text(tab, tab.history_index + 1U);
        ImGui::EndDisabled();
        note_item("shader_editor:redo");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Redo (Ctrl+Y)");
        if (shader_editor_focused && io.KeyCtrl && !code_box_focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.KeyShift && can_undo) restore_shader_text(tab, tab.history_index - 1U);
            else if ((ImGui::IsKeyPressed(ImGuiKey_Y, false) || (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyShift)) && can_redo)
                restore_shader_text(tab, tab.history_index + 1U);
        }
        ImGui::SameLine();
        if (editor_checkbox("Live preview", &shader_live_preview) && !shader_live_preview) discard_shader_preview(tab);
        note_item("shader_editor:live_preview");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show changes in the viewport as you make them, before saving");
        ImGui::SameLine();
        std::vector<std::string> other_errors;
        const auto node_errors = shader_node_errors(tab, other_errors);
        const auto* status = tab.status.object();
        const auto* errors = status ? field(*status, "errors") : nullptr;
        const std::size_t error_count = errors && errors->array() ? errors->array()->size() : 0U;
        if (error_count == 0U)
            ImGui::TextColored(editor_color(palette.success), "Compiled  ·  %s shader",
                               tab.graph.type == ShaderType::post_process ? "post-processing" : "surface");
        else
            ImGui::TextColored(editor_color(palette.danger), "%zu error%s", error_count, error_count == 1U ? "" : "s");
        note_item("shader_editor:status");
        if (!tab.message.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(editor_color(palette.warning), "%s", tab.message.c_str());
        }

        if (tab.raw) {
            // Declarations that do not parse (an agent's mistake, say) keep the graph closed.
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.warning));
            ImGui::TextWrapped("This shader's declarations have mistakes, so it cannot be shown as nodes. Fix them "
                               "here; the nodes come back once it reads correctly.");
            ImGui::PopStyleColor();
            for (const auto& message : tab.graph.errors)
                ImGui::TextColored(editor_color(palette.danger), "Line %u: %s", message.line, message.text.c_str());
            if (draw_shader_code_box(*tab.code_editor, tab.code_undo, "##shader_raw", {0.0F, -ImGui::GetFrameHeightWithSpacing()})) {
                tab.text = tab.code_editor->GetText();
                tab.dirty = tab.text != tab.saved;
                tab.preview_pending = true;
                tab.changed_at = now;
            }
            note_item("shader_editor:raw");
            const auto reparsed = shader_to_graph(tab.text);
            ImGui::BeginDisabled(!reparsed.errors.empty());
            if (ImGui::Button("Show as nodes")) {
                load_shader_text(tab, tab.text);
                remember_shader_text(tab);
            }
            ImGui::EndDisabled();
            note_item("shader_editor:show_nodes");
        } else {
            // Stages, then a side panel (parameters, settings, functions) beside the canvas.
            if (tab.graph.type == ShaderType::surface) {
                for (const auto* name : {"fragment", "vertex"}) {
                    if (name != std::string_view{"fragment"}) ImGui::SameLine();
                    const bool active = tab.stage == name;
                    if (active) ImGui::PushStyleColor(ImGuiCol_Button, editor_color(palette.accent_soft));
                    if (ImGui::Button(name == std::string_view{"fragment"} ? "Fragment (surface)" : "Vertex (shape)")) {
                        tab.stage = name;
                        refresh_stage_code_editor(tab);
                    }
                    if (active) ImGui::PopStyleColor();
                    note_item(std::string("shader_editor:stage:") + name);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Right-click or Space to add nodes; drag from pins to wire them.");
            }
            const float side = 250.0F * ui_scale;
            ImGui::BeginChild("##shader_side", {side, 0.0F}, ImGuiChildFlags_Borders);
            draw_shader_side_panel(tab);
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##shader_main", {0.0F, 0.0F});
            auto* stage = tab.graph.stage(tab.stage);
            const float errors_height = other_errors.empty()
                                            ? 0.0F
                                            : std::min<float>(static_cast<float>(other_errors.size()), 3.0F) *
                                                      ImGui::GetTextLineHeightWithSpacing() +
                                                  ImGui::GetStyle().WindowPadding.y * 2.0F;
            if (stage && stage->code_only) {
                ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
                ImGui::TextWrapped("This stage is code, because %s. Nodes cannot show that; edit it here, or remove "
                                   "it and use nodes.", stage->reason.c_str());
                ImGui::PopStyleColor();
                if (!tab.code_editor) refresh_stage_code_editor(tab);
                if (draw_shader_code_box(*tab.code_editor, tab.code_undo, "##shader_stage_code",
                                         {0.0F, -errors_height - ImGui::GetFrameHeightWithSpacing()})) {
                    stage->code = tab.code_editor->GetText();
                    graph_changed(tab, true);
                }
                note_item("shader_editor:stage_code");
                if (ImGui::Button("Try as nodes")) {
                    const auto name = tab.stage;
                    load_shader_text(tab, tab.text);
                    tab.stage = name;
                    refresh_stage_code_editor(tab);
                    if (auto* converted = tab.graph.stage(tab.stage); converted && converted->code_only)
                        tab.message = "Still code: " + converted->reason;
                }
                note_item("shader_editor:try_nodes");
            } else if (stage) {
                ImGui::BeginChild("##canvas_area", {0.0F, -errors_height});
                auto& canvas_view = tab.views[tab.stage];
                const auto events = draw_shader_graph_canvas(
                    tab.graph, *stage, canvas_view, node_errors,
                    [this](const std::string& key, const ImVec2 low, const ImVec2 high) { note_rect(key, low, high); },
                    ui_scale);
                ImGui::EndChild();
                if (!events.status.empty()) tab.message = events.status;
                else if (events.committed) tab.message.clear();
                if (events.changed || events.committed) graph_changed(tab, events.committed);
            }
            if (!other_errors.empty()) {
                ImGui::BeginChild("##shader_errors", {0.0F, errors_height}, ImGuiChildFlags_Borders);
                for (std::size_t index = 0; index < other_errors.size(); ++index) {
                    ImGui::TextColored(editor_color(palette.danger), "%s", other_errors[index].c_str());
                    note_item("shader_editor:error:" + std::to_string(index));
                }
                ImGui::EndChild();
            }
            ImGui::EndChild();
        }
        // A pause after an edit sends the text for preview; going back to the saved text stops it.
        if (tab.preview_pending && shader_live_preview && now - tab.changed_at > 0.35) {
            tab.preview_pending = false;
            if (tab.text == tab.saved) {
                discard_shader_preview(tab);
                set_shader_status(tab, call("shaders.read", "\"path\":\"" + json_escape(tab.path) + '"', false));
            } else {
                tab.previewed = tab.text;
                set_shader_status(tab, call("shaders.preview", "\"path\":\"" + json_escape(tab.path) +
                                                                   "\",\"text\":\"" + json_escape(tab.text) + '"'));
            }
        }
    }

    // Parameters (uniforms), render modes and the shader's own functions.
    void draw_shader_side_panel(ShaderTab& tab) {
        const auto& palette = editor_palette();
        auto& graph = tab.graph;
        ImGui::SeparatorText("Parameters");
        note_item("shader_editor:parameters");
        std::optional<std::size_t> remove;
        for (std::size_t index = 0; index < graph.uniforms.size(); ++index) {
            auto& uniform = graph.uniforms[index];
            ImGui::PushID(static_cast<int>(index));
            char name[64]{};
            std::snprintf(name, sizeof(name), "%s", uniform.name.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 3.0F * ImGui::GetFrameHeight());
            ImGui::InputText("##name", name, sizeof(name), ImGuiInputTextFlags_CharsNoBlank);
            if (ImGui::IsItemDeactivatedAfterEdit()) rename_shader_parameter(tab, index, name);
            note_item("shader_editor:parameter:" + uniform.name);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s parameter; materials set its value", std::string{shader_uniform_type_name(uniform.type)}.c_str());
            ImGui::SameLine(0.0F, 2.0F);
            if (ImGui::Button("+", {ImGui::GetFrameHeight(), 0.0F})) {
                auto* stage = graph.stage(tab.stage);
                if (stage && !stage->code_only) {
                    auto& canvas_view = tab.views[tab.stage];
                    const float zoom = canvas_view.zoom * ui_scale;
                    const ImVec2 at{(-canvas_view.pan.x + 60.0F) / zoom, (-canvas_view.pan.y + 60.0F) / zoom};
                    ShaderGraphEntry entry{uniform.name, "Parameters", {}, GraphNode::Kind::parameter, uniform.name, 0U, {}};
                    canvas_view.selected = {add_shader_graph_node(graph, *stage, entry, at)};
                    graph_changed(tab, true);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Put this parameter on the canvas");
            note_item("shader_editor:parameter:add:" + uniform.name);
            ImGui::SameLine(0.0F, 2.0F);
            if (ImGui::Button("x", {ImGui::GetFrameHeight(), 0.0F})) remove = index;
            note_item("shader_editor:parameter:remove:" + uniform.name);
            ImGui::TextColored(editor_color(palette.text_faint), "%s%s", std::string{shader_uniform_type_name(uniform.type)}.c_str(),
                               uniform.hint == ShaderUniform::Hint::none ? "" : ("  ·  " + std::string{shader_uniform_hint_name(uniform.hint)}).c_str());
            ImGui::PopID();
        }
        if (remove) {
            const auto name = graph.uniforms[*remove].name;
            graph.uniforms.erase(graph.uniforms.begin() + static_cast<std::ptrdiff_t>(*remove));
            for (auto* stage : {&graph.vertex, &graph.fragment}) {
                std::erase_if(stage->nodes, [&](const GraphNode& node) {
                    return node.kind == GraphNode::Kind::parameter && node.op == name;
                });
                for (auto& node : stage->nodes)
                    for (auto& input : node.inputs)
                        if (input.source == "uniform:" + name) input.source.clear();
            }
            graph_changed(tab, true);
        }
        if (ImGui::Button("Add parameter")) ImGui::OpenPopup("##new_parameter");
        note_item("shader_editor:parameter:new");
        if (ImGui::BeginPopup("##new_parameter")) {
            static constexpr std::array<std::tuple<const char*, ShaderUniform::Type, ShaderUniform::Hint>, 7> kinds{{
                {"Color", ShaderUniform::Type::vec3, ShaderUniform::Hint::color},
                {"Number", ShaderUniform::Type::float_value, ShaderUniform::Hint::none},
                {"Slider (0 to 1)", ShaderUniform::Type::float_value, ShaderUniform::Hint::range},
                {"Image", ShaderUniform::Type::sampler2d, ShaderUniform::Hint::color},
                {"Normal map", ShaderUniform::Type::sampler2d, ShaderUniform::Hint::normal},
                {"Vector (vec3)", ShaderUniform::Type::vec3, ShaderUniform::Hint::none},
                {"Switch", ShaderUniform::Type::bool_value, ShaderUniform::Hint::none}}};
            for (const auto& [label, type, hint] : kinds) {
                if (ImGui::Selectable(label)) {
                    ShaderUniform uniform;
                    uniform.type = type;
                    uniform.hint = hint;
                    if (hint == ShaderUniform::Hint::color && type == ShaderUniform::Type::vec3) uniform.default_value = {1, 1, 1, 1};
                    if (hint == ShaderUniform::Hint::range) uniform.default_value = {0.5, 0, 0, 0};
                    std::string stem = type == ShaderUniform::Type::sampler2d ? "image" : hint == ShaderUniform::Hint::color ? "color" : "value";
                    std::string name = stem;
                    for (int suffix = 2; std::any_of(graph.uniforms.begin(), graph.uniforms.end(),
                                                     [&](const ShaderUniform& other) { return other.name == name; });
                         ++suffix)
                        name = stem + "_" + std::to_string(suffix);
                    uniform.name = name;
                    graph.uniforms.push_back(uniform);
                    graph_changed(tab, true);
                }
                note_item(std::string("shader_editor:parameter:new:") + label);
            }
            ImGui::EndPopup();
        }
        if (graph.type == ShaderType::surface) {
            ImGui::SeparatorText("Surface");
            if (editor_checkbox("Transparent", &graph.transparent)) graph_changed(tab, true);
            note_item("shader_editor:transparent");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blend with what is behind, using the output's Alpha");
            if (editor_checkbox("Unshaded", &graph.unshaded)) graph_changed(tab, true);
            note_item("shader_editor:unshaded");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show Albedo and Emission as they are, without lighting");
        }
        ImGui::SeparatorText("Functions");
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_faint));
        ImGui::TextWrapped("GLSL functions that nodes can call by name.");
        ImGui::PopStyleColor();
        if (tab.functions_editor &&
            draw_shader_code_box(*tab.functions_editor, tab.functions_undo, "##shader_functions",
                                 {0.0F, std::max(120.0F * ui_scale, ImGui::GetContentRegionAvail().y)})) {
            graph.functions = tab.functions_editor->GetText();
            graph_changed(tab, true);
        }
        note_item("shader_editor:functions");
    }

    // Renames a uniform everywhere it is used, keeping the name unique and valid.
    void rename_shader_parameter(ShaderTab& tab, const std::size_t index, const std::string& name) {
        auto& graph = tab.graph;
        if (index >= graph.uniforms.size() || name == graph.uniforms[index].name) return;
        const bool valid = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) != 0 || name[0] == '_') &&
                           std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }) &&
                           std::none_of(graph.uniforms.begin(), graph.uniforms.end(), [&](const ShaderUniform& other) { return other.name == name; });
        if (!valid) {
            tab.message = "Parameter names are unique letters, digits and underscores";
            return;
        }
        const auto old = graph.uniforms[index].name;
        graph.uniforms[index].name = name;
        for (auto* stage : {&graph.vertex, &graph.fragment})
            for (auto& node : stage->nodes) {
                if (node.kind == GraphNode::Kind::parameter && node.op == old) {
                    node.op = name;
                    node.id = "uniform:" + name;
                }
                for (auto& input : node.inputs)
                    if (input.source == "uniform:" + old) input.source = "uniform:" + name;
            }
        graph_changed(tab, true);
        const auto check = parse_relay_shader(tab.text);
        if (!check.errors.empty()) tab.message = check.errors.front().text;
    }

    // Sound files for the clip picker, and the buses sources can play into.
    void refresh_audio_files() {
        std::vector<std::string> files;
        if (auto found = call("assets.search", "\"kinds\":[\"audio\"]", false); found && found->object())
            if (const auto* entries = field(*found->object(), "entries"); entries && entries->array())
                for (const auto& entry : *entries->array())
                    if (const auto* object = entry.object(); object && string_or(*object, "type") == "file")
                        files.push_back(string_or(*object, "path"));
        // A changed file list may mean a changed file, so its waveform is read again.
        if (files != audio_files) audio_clip_info.clear();
        audio_files = std::move(files);
        if (auto settings = call("audio.settings", {}, false)) audio_settings = std::move(*settings);
    }

    std::vector<std::string> audio_bus_names() const {
        std::vector<std::string> names;
        const auto* object = audio_settings.object();
        const auto* settings = object ? field(*object, "settings") : nullptr;
        const auto* buses = settings && settings->object() ? field(*settings->object(), "buses") : nullptr;
        if (buses && buses->array())
            for (const auto& bus : *buses->array())
                if (const auto* entry = bus.object()) names.push_back(string_or(*entry, "name"));
        return names;
    }

    // Voices and meters are only fetched while something shows them.
    void refresh_audio_status() {
        const auto* entity = selection.empty() ? nullptr : find_entity(selection);
        const bool shown = (game_config_open && game_config_page == 2) || panel_open[10] ||
                           (entity && (component(*entity, "audio_source") ||
                                       component(*entity, "reverb_zone") ||
                                       component(*entity, "music_player")));
        if (!shown) {
            audio_status = JsonValue{};
            return;
        }
        if (auto status = call("audio.status", {}, false)) audio_status = std::move(*status);
    }

    // The playing voice for `handle`, if audio.status lists one.
    const JsonValue::Object* audio_voice(const std::string& handle) const {
        const auto* status = audio_status.object();
        const auto* voices = status ? field(*status, "voices") : nullptr;
        if (voices && voices->array())
            for (const auto& voice : *voices->array())
                if (const auto* object = voice.object(); object && string_or(*object, "entity") == handle)
                    return object;
        return nullptr;
    }

    const JsonValue::Object* audio_clip_summary(const std::string& clip) {
        auto found = audio_clip_info.find(clip);
        if (found == audio_clip_info.end()) {
            auto info = call("audio.clip", "\"clip\":\"" + json_escape(clip) + "\",\"peaks\":96", false);
            found = audio_clip_info.emplace(clip, info ? std::move(*info) : JsonValue{}).first;
        }
        return found->second.object();
    }

    void set_game_input_focus(const bool focus) {
        if (focus == game_input_focus) return;
        game_input_focus = focus;
        if (imgui_context_created) {
            ImGui::GetIO().ClearInputKeys();
            ImGui::GetIO().ClearInputMouse();
        }
        if (focus) {
            if (game_lock_mouse) capture_pointer(true, false);
            return;
        }
        capture_pointer(false, false);
        (void)call("input.release", {}, false);
    }

    void draw_game_input_hint() {
        const auto* status = runtime_status.object();
        if (!viewport_visible || !viewport_draw_list || !status ||
            string_or(*status, "mode") != "game")
            return;
        const char* text = game_input_focus ? "Playing. Esc returns input to the editor."
                                            : "Click the viewport to give the game input.";
        const auto size = ImGui::CalcTextSize(text);
        const ImVec2 corner{viewport_min.x + 10.0F * ui_scale, viewport_min.y + 10.0F * ui_scale};
        const ImVec2 padding{8.0F * ui_scale, 4.0F * ui_scale};
        viewport_draw_list->AddRectFilled(
            ImVec2(corner.x - padding.x, corner.y - padding.y),
            ImVec2(corner.x + size.x + padding.x, corner.y + size.y + padding.y),
            IM_COL32(0, 0, 0, 150), 4.0F * ui_scale);
        viewport_draw_list->AddText(corner, IM_COL32(255, 255, 255, game_input_focus ? 170 : 255),
                                    text);
    }

    void refresh_scripts() {
        if (auto status = call_if_changed("scripts.status", script_status_reply))
            script_status = std::move(*status);
        const auto* status = scripts();
        if (!status) return;
        const auto state = string_or(*status, "state");
        if (auto_build_scripts && boolean_or(*status, "trusted", false) &&
            boolean_or(*status, "stale", false) && state != "building")
            start_script_build(false);
        if (!play_after_build || state == "building") return;
        play_after_build = false;
        if (state == "failed") {
            set_status("Script build failed; see Diagnostics", true);
            panel_open[4] = true;
        } else {
            request_play();
        }
    }

    void start_script_build(const bool report) {
        const auto error = call_error("scripts.build");
        script_status_reply.clear();
        if (auto status = call_if_changed("scripts.status", script_status_reply))
            script_status = std::move(*status);
        if (error.find("already running") != std::string::npos) return;
        if (!error.empty() && report) set_status("scripts.build: " + error, true);
        else if (error.empty()) set_status("Building scripts", false);
    }

    // Run Game, building or asking for script trust first when the engine says it must.
    void request_play() {
        const auto error = call_error("runtime.play");
        if (error.empty()) {
            set_status("Game running", false);
            return;
        }
        if (error.find("not trusted") != std::string::npos) {
            open_trust_dialog = true;
        } else if (error.find("scripts.build") != std::string::npos ||
                   error.find("still building") != std::string::npos) {
            if (error.find("still building") == std::string::npos) start_script_build(true);
            play_after_build = true;
            set_status("Building scripts before Run Game", false);
        } else {
            set_status("runtime.play: " + error, true);
            if (error.find("script") != std::string::npos) panel_open[4] = true;
        }
    }

    // Every mutation goes through this helper so the panels cannot accidentally grow a second,
    // untraced path into the scene.
    bool mutate(const std::string_view method, const std::string_view fields,
                const std::string_view success) {
        const auto gesture = std::exchange(pending_gesture, 0U);
        const bool tagged = gesture != 0U && fields.find("\"gesture\"") == std::string_view::npos;
        if (!call(method, tagged ? std::string(fields) + ",\"gesture\":" + std::to_string(gesture)
                                 : std::string(fields))
                 .has_value())
            return false;
        set_status(std::string(success), false);
        refresh_pending = true;
        if (method == "scene.load" || method == "scene.undo" || method == "scene.redo")
            assets_pending = true;
        return true;
    }

    void draw_agent() {
        const auto* review = agent_review.object();
        const auto* session = review ? field(*review, "session") : nullptr;
        const auto* session_object = session ? session->object() : nullptr;
        const auto& colors = editor_palette();
        const auto* status = chat_status.object();
        const bool connected = status && boolean_or(*status, "connected", false);
        const auto* projection = status ? field(*status, "view") : nullptr;
        const auto* object = projection ? projection->object() : nullptr;
        const auto* provider_value = object ? field(*object, "provider") : nullptr;
        const auto* provider_settings = provider_value ? provider_value->object() : nullptr;
        const bool is_openai = !provider_settings || string_or(*provider_settings, "selected", "openai") == "openai";
        const auto* openai_value = object ? field(*object, "openai") : nullptr;
        const auto* openai = openai_value ? openai_value->object() : nullptr;
        const bool busy = object && boolean_or(*object, "busy", false);
        chat_busy = busy;
        const auto control = [&](const std::string& action, const std::string& extra = "") {
            (void)call("chat.control", "\"action\":\"" + action + "\"" + (extra.empty() ? "" : "," + extra));
            refresh_pending = true;
        };
        if (!ImGui::BeginTabBar("AgentTabs")) return;
        const bool chat_tab = ImGui::BeginTabItem("Chat");
        note_item("agent:chat-tab");
        if (chat_tab) {
            receive_attachments();
            const auto* messages_value = object ? field(*object, "messages") : nullptr;
            const auto* messages = messages_value ? messages_value->array() : nullptr;
            const auto full_status = object ? string_or(*object, "status", "Connecting...") : "Waiting for bridge...";
            const bool show_status = !busy && full_status != "Ready" && !full_status.empty();
            const float usage_font_size = 13 * ui_scale;
            const float extra_usage_height = 18 * ui_scale + ImGui::GetStyle().ItemSpacing.y;
            const float transcript_height = std::max(80 * ui_scale, ImGui::GetContentRegionAvail().y - (attachments.empty() ? 155 : 190) * ui_scale - extra_usage_height - (show_status ? 36 * ui_scale : 0));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, editor_color(colors.panel));
            if (ImGui::BeginChild("Conversation", ImVec2(0, transcript_height), ImGuiChildFlags_Borders)) {
                chat_follow = chat_jump_pending || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2 * ui_scale;
                chat_jump_pending = false;
                if (!messages || messages->empty()) {
                    ImGui::Spacing(); ImGui::Spacing();
                    ImGui::PushFont(fonts.heading);
                    ImGui::TextWrapped("What would you like to build?");
                    ImGui::PopFont();
                    ImGui::TextWrapped("Inspect your scene, make changes, and follow every action here.");
                    ImGui::Spacing();
                    if (ImGui::Button("Inspect this scene", ImVec2(-1, 0))) { chat_message.fill('\0');
                        std::copy_n("Inspect this scene and suggest improvements.", 44, chat_message.data()); }
                    if (ImGui::Button("Add a light", ImVec2(-1, 0))) { chat_message.fill('\0');
                        std::copy_n("Add a light that suits this scene.", 34, chat_message.data()); }
                    if (is_openai && (!openai || !field(*openai, "account") || !field(*openai, "account")->object())) {
                        ImGui::Spacing(); ImGui::TextWrapped("Sign in with ChatGPT to get started.");
                        if (ImGui::Button("Connect OpenAI", ImVec2(-1, 0))) agent_account_pending = true;
                    }
                }
                if (messages) for (std::size_t index = 0; index < messages->size(); ++index) if (const auto* entry = (*messages)[index].object()) {
                    const auto role = string_or(*entry, "role", "assistant");
                    const auto content = string_or(*entry, "content", "");
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, editor_color(role == "user" ? colors.accent_soft : colors.panel));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * ui_scale, 10 * ui_scale));
                    if (ImGui::BeginChild("Message", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize)) {
                        std::istringstream lines(content); std::string line, text; bool code = false; int block = 0;
                        const auto flush = [&]() {
                            if (code) ImGui::PushFont(fonts.monospace);
                            selectable_chat_text(text, block++);
                            if (code) ImGui::PopFont();
                            text.clear();
                        };
                        while (std::getline(lines, line)) {
                            if (line.starts_with("```")) { flush(); code = !code; ImGui::Spacing(); continue; }
                            if (!code && line.starts_with("#")) {
                                flush(); const auto first = line.find_first_not_of("# ");
                                ImGui::PushFont(fonts.heading);
                                selectable_chat_text(first == std::string::npos ? "" : line.substr(first), block++);
                                ImGui::PopFont();
                            } else if (!code && line.find("![") != std::string::npos) {
                                flush(); if (!chat_media.draw(line, headless, [&](std::string_view value) { selectable_chat_text(std::string(value), block++); })) selectable_chat_text(line, block++);
                            } else { if (!text.empty()) text += '\n'; text += line; }
                        }
                        flush();
                    }
                    ImGui::EndChild(); ImGui::PopStyleVar(); ImGui::PopStyleColor(); ImGui::PopID(); ImGui::Spacing();
                }
                if (busy) ImGui::TextDisabled("Relay is working...");
                if (chat_follow) ImGui::SetScrollHereY(1.0F);
                if (!chat_follow) {
                    const auto position = ImGui::GetWindowPos();
                    const auto size = ImGui::GetWindowSize();
                    ImGui::SetCursorScreenPos(ImVec2(position.x + size.x * .5F - 16 * ui_scale, position.y + size.y - 44 * ui_scale));
                    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                    ImGui::BeginChild("JumpBottom", ImVec2(32 * ui_scale, 32 * ui_scale), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                    if (ImGui::InvisibleButton("##JumpToBottom", ImVec2(32 * ui_scale, 32 * ui_scale))) { chat_jump_pending = true; chat_follow = true; }
                    auto* arrow_draw = ImGui::GetWindowDrawList();
                    const auto arrow_center = ImVec2(position.x + size.x * .5F, position.y + size.y - 28 * ui_scale);
                    arrow_draw->AddCircleFilled(arrow_center, 16 * ui_scale, colors.panel);
                    arrow_draw->AddCircle(arrow_center, 16 * ui_scale, colors.text_dim);
                    arrow_draw->AddLine(ImVec2(arrow_center.x, arrow_center.y - 7 * ui_scale), ImVec2(arrow_center.x, arrow_center.y + 7 * ui_scale), colors.text, 2 * ui_scale);
                    arrow_draw->AddLine(ImVec2(arrow_center.x - 6 * ui_scale, arrow_center.y + 1 * ui_scale), ImVec2(arrow_center.x, arrow_center.y + 7 * ui_scale), colors.text, 2 * ui_scale);
                    arrow_draw->AddLine(ImVec2(arrow_center.x + 6 * ui_scale, arrow_center.y + 1 * ui_scale), ImVec2(arrow_center.x, arrow_center.y + 7 * ui_scale), colors.text, 2 * ui_scale);
                    note_item("agent:jump-bottom");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to latest message");
                    ImGui::EndChild(); ImGui::PopStyleVar();
                }
            }
            ImGui::EndChild(); ImGui::PopStyleColor();
            ImGui::Spacing();
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 16 * ui_scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * ui_scale, 10 * ui_scale));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, editor_color(colors.panel));
            bool send = false;
            if (ImGui::BeginChild("Composer", ImVec2(0, (attachments.empty() ? 135 : 170) * ui_scale), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
                receive_attachments();
                const bool had_attachments = !attachments.empty();
                if (had_attachments) ImGui::BeginChild("Attachments", ImVec2(0, 40 * ui_scale), 0, ImGuiWindowFlags_HorizontalScrollbar);
                for (std::size_t i = 0; i < attachments.size();) {
                    ImGui::PushID(static_cast<int>(i));
                    const auto label = std::filesystem::path(attachments[i]).filename().string() + "  x";
                    if (ImGui::SmallButton(label.c_str())) attachments.erase(attachments.begin() + static_cast<std::ptrdiff_t>(i)); else ++i;
                    note_item("agent:attachment"); ImGui::PopID();
                    if (i < attachments.size()) ImGui::SameLine();
                }
                if (had_attachments) ImGui::EndChild();
                const float action_size = 32 * ui_scale;
                const float input_height = std::max(ImGui::GetTextLineHeight() * 2, ImGui::GetContentRegionAvail().y - action_size - ImGui::GetStyle().ItemSpacing.y - 2 * ui_scale);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
                wrapped_chat.width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ScrollbarSize;
                if (wrapped_chat.raw != chat_message.data()) { wrapped_chat.raw = chat_message.data(); wrapped_chat.wrap(); }
                if (ImGui::GetActiveID() != ImGui::GetID("##ChatMessage")) wrapped_chat.wrap();
                std::copy(wrapped_chat.display.begin(), wrapped_chat.display.end(), chat_display.begin()); chat_display[wrapped_chat.display.size()] = '\0';
                send = ImGui::InputTextMultiline("##ChatMessage", chat_display.data(), chat_display.size(), ImVec2(ImGui::GetContentRegionAvail().x, input_height),
                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_CallbackAlways,
                    WrappedInput::callback, &wrapped_chat);
                wrapped_chat.edit(chat_display.data());
                std::copy(wrapped_chat.raw.begin(), wrapped_chat.raw.end(), chat_message.begin()); chat_message[wrapped_chat.raw.size()] = '\0';
                note_item("agent:message");
                const auto input_min = ImGui::GetItemRectMin(), input_max = ImGui::GetItemRectMax();
                composer_rect = std::array<float, 4>{input_min.x, input_min.y, input_max.x, input_max.y};
                if (drop_hover) {
                    auto* drop_draw = ImGui::GetWindowDrawList();
                    drop_draw->AddRectFilled(input_min, input_max, colors.panel, 8 * ui_scale);
                    drop_draw->AddRect(input_min, input_max, colors.accent, 8 * ui_scale, 0, 2 * ui_scale);
                    const char* hint = "Drop to attach file(s)";
                    const auto size = ImGui::CalcTextSize(hint);
                    drop_draw->AddText(ImVec2((input_min.x + input_max.x - size.x) * .5F, (input_min.y + input_max.y - size.y) * .5F), colors.text, hint);
                }
                if (!drop_hover && !chat_message[0] && !ImGui::IsItemActive()) {
                    const auto origin = ImGui::GetItemRectMin();
                    const auto padding = ImGui::GetStyle().FramePadding;
                    ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + padding.x, origin.y + padding.y), colors.text_faint, "Message Relay...");
                }
                ImGui::PopStyleColor();
                const auto model = openai ? string_or(*openai, "model", "") : "";
                const auto effort = openai ? string_or(*openai, "effort", "") : "";
                const auto* models_value = openai ? field(*openai, "models") : nullptr;
                const auto* models = models_value ? models_value->array() : nullptr;
                const JsonValue::Object* selected = nullptr;
                if (models) for (const auto& entry : *models) if (entry.object() && string_or(*entry.object(), "model", "") == model) selected = entry.object();
                const auto model_label = is_openai ? (selected ? string_or(*selected, "displayName", "Model") : "Model") :
                    (provider_settings ? string_or(*provider_settings, "model", "Model") : "Model");
                const float gap = ImGui::GetStyle().ItemSpacing.x;
                const float label_gap = effort.empty() || !is_openai ? 0 : 8 * ui_scale;
                const float model_text_width = ImGui::CalcTextSize(model_label.c_str()).x;
                const float effort_text_width = is_openai ? ImGui::CalcTextSize(effort.c_str()).x : 0;
                const float row_width = ImGui::GetContentRegionAvail().x;
                const float options_width = std::min(model_text_width + label_gap + effort_text_width + 36 * ui_scale,
                    std::max(32 * ui_scale, row_width - action_size * 2 - gap * 2));
                const float row_start = ImGui::GetCursorPosX();
                ImGui::BeginDisabled(busy || attachments.size() >= 8);
                if (ImGui::InvisibleButton("##Attach", ImVec2(action_size, action_size))) pick_attachments();
                note_item("agent:attach");
                const auto attach_min = ImGui::GetItemRectMin();
                auto* attach_draw = ImGui::GetWindowDrawList();
                const ImVec2 attach_center(attach_min.x + action_size * .5F, attach_min.y + action_size * .5F);
                attach_draw->AddLine(ImVec2(attach_center.x - 7 * ui_scale, attach_center.y), ImVec2(attach_center.x + 7 * ui_scale, attach_center.y), colors.text_dim, 2 * ui_scale);
                attach_draw->AddLine(ImVec2(attach_center.x, attach_center.y - 7 * ui_scale), ImVec2(attach_center.x, attach_center.y + 7 * ui_scale), colors.text_dim, 2 * ui_scale);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Attach files");
                ImGui::EndDisabled(); ImGui::SameLine();
                ImGui::SetCursorPosX(row_start + std::max(action_size + gap, row_width - options_width - gap - action_size));
                ImGui::BeginDisabled(busy || !is_openai || !models || models->empty());
                if (ImGui::InvisibleButton("##AgentOptions", ImVec2(options_width, action_size))) ImGui::OpenPopup("AgentOptions");
                note_item("agent:options");
                const auto options_position = ImGui::GetItemRectMin();
                const auto options_max = ImGui::GetItemRectMax();
                auto* options_draw = ImGui::GetWindowDrawList();
                options_draw->AddRectFilled(options_position, options_max, colors.panel, 8 * ui_scale);
                const float text_y = options_position.y + (action_size - ImGui::GetTextLineHeight()) * .5F;
                options_draw->PushClipRect(options_position, ImVec2(options_max.x - 24 * ui_scale, options_max.y), true);
                options_draw->AddText(ImVec2(options_position.x + 8 * ui_scale, text_y), colors.text, model_label.c_str());
                if (is_openai && !effort.empty()) options_draw->AddText(ImVec2(options_position.x + 8 * ui_scale + model_text_width + label_gap, text_y), colors.text_dim, effort.c_str());
                options_draw->PopClipRect();
                const auto chevron = ImVec2(options_max.x - 12 * ui_scale, options_position.y + action_size * .5F);
                options_draw->AddLine(ImVec2(chevron.x - 4 * ui_scale, chevron.y + 2 * ui_scale), ImVec2(chevron.x, chevron.y - 2 * ui_scale), colors.text_dim, 1.5F * ui_scale);
                options_draw->AddLine(ImVec2(chevron.x, chevron.y - 2 * ui_scale), ImVec2(chevron.x + 4 * ui_scale, chevron.y + 2 * ui_scale), colors.text_dim, 1.5F * ui_scale);
                ImGui::EndDisabled();
                ImGui::SetNextWindowPos(ImVec2(options_max.x, options_position.y - 8 * ui_scale), ImGuiCond_Always, ImVec2(1, 1));
                ImGui::SetNextWindowSize(ImVec2(std::min(340 * ui_scale, ImGui::GetWindowWidth() - 24 * ui_scale), 0));
                if (ImGui::BeginPopup("AgentOptions")) {
                    ImGui::BeginDisabled(busy);
                    ImGui::TextDisabled("Model");
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::BeginCombo("##AgentModel", model_label.c_str())) {
                        if (models) for (const auto& entry : *models) if (const auto* candidate = entry.object()) {
                            const auto id = string_or(*candidate, "model", "");
                            if (ImGui::Selectable(string_or(*candidate, "displayName", id).c_str(), id == model))
                                control("select", "\"model\":\"" + json_escape(id) + "\"");
                            note_item("agent:model-option:" + id);
                        }
                        ImGui::EndCombo();
                    }
                    note_item("agent:model-picker");
                    const auto* efforts_value = selected ? field(*selected, "supportedReasoningEfforts") : nullptr;
                    const auto* efforts = efforts_value ? efforts_value->array() : nullptr;
                    if (efforts && !efforts->empty()) {
                        if (agent_reasoning_model != model || agent_reasoning_effort != effort) {
                            agent_reasoning_model = model; agent_reasoning_effort = effort; agent_reasoning_index = 0;
                            for (std::size_t i = 0; i < efforts->size(); ++i) if ((*efforts)[i].object() && string_or(*(*efforts)[i].object(), "reasoningEffort", "") == effort) agent_reasoning_index = static_cast<int>(i);
                        }
                        agent_reasoning_index = std::clamp(agent_reasoning_index, 0, static_cast<int>(efforts->size()) - 1);
                        const auto* choice = (*efforts)[static_cast<std::size_t>(agent_reasoning_index)].object();
                        const auto level = choice ? string_or(*choice, "reasoningEffort", "Default") : "Default";
                        ImGui::Separator(); ImGui::TextDisabled("Reasoning");
                        ImGui::SetNextItemWidth(-1);
                        ImGui::SliderInt("##AgentReasoning", &agent_reasoning_index, 0, static_cast<int>(efforts->size()) - 1, level.c_str(), ImGuiSliderFlags_NoInput);
                        note_item("agent:reasoning-slider");
                        if (ImGui::IsItemDeactivatedAfterEdit()) {
                            const auto* chosen = (*efforts)[static_cast<std::size_t>(agent_reasoning_index)].object();
                            if (chosen) control("select", "\"model\":\"" + json_escape(model) + "\",\"effort\":\"" + json_escape(string_or(*chosen, "reasoningEffort", "")) + "\"");
                        }
                    }
                    ImGui::EndDisabled(); ImGui::EndPopup();
                }
                ImGui::SameLine(0, gap);
                ImGui::BeginDisabled(!connected || (!busy && !chat_message[0] && attachments.empty()));
                const bool activated = ImGui::InvisibleButton("##ChatAction", ImVec2(32 * ui_scale, 32 * ui_scale));
                note_item(busy ? "agent:stop" : "agent:send");
                const auto center = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * .5F, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * .5F);
                auto* draw = ImGui::GetWindowDrawList();
                draw->AddCircleFilled(center, 16 * ui_scale, colors.accent);
                if (busy) draw->AddRectFilled(ImVec2(center.x - 5 * ui_scale, center.y - 5 * ui_scale), ImVec2(center.x + 5 * ui_scale, center.y + 5 * ui_scale), colors.text, 1 * ui_scale);
                else {
                    draw->AddLine(ImVec2(center.x, center.y + 7 * ui_scale), ImVec2(center.x, center.y - 7 * ui_scale), colors.text, 2 * ui_scale);
                    draw->AddLine(ImVec2(center.x - 6 * ui_scale, center.y - 1 * ui_scale), ImVec2(center.x, center.y - 7 * ui_scale), colors.text, 2 * ui_scale);
                    draw->AddLine(ImVec2(center.x + 6 * ui_scale, center.y - 1 * ui_scale), ImVec2(center.x, center.y - 7 * ui_scale), colors.text, 2 * ui_scale);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", busy ? "Stop" : "Send message");
                if (activated && busy) (void)call("chat.cancel");
                if (activated && !busy) send = true;
                ImGui::EndDisabled();
            }
            ImGui::EndChild(); ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
            const float usage_gap = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4 * ui_scale);
            const float usage_row_width = std::max(1.0F, ImGui::GetContentRegionAvail().x - 4 * ui_scale);
                const auto* usage_value = openai ? field(*openai, "usage") : nullptr;
                const auto* usage = is_openai && usage_value ? usage_value->object() : nullptr;
                for (const auto& [key, label] : {std::pair{"fiveHour", "5-Hourly"}, std::pair{"weekly", "Weekly"}}) {
                    const auto* value = usage ? field(*usage, key) : nullptr;
                    const bool known = value && value->number();
                    const float percent = known ? std::clamp(static_cast<float>(*value->number()), 0.0F, 100.0F) : 0;
                    const float width = (usage_row_width - usage_gap) * .5F;
                    const float height = 18 * ui_scale;
                    ImGui::PushID(key);
                    ImGui::InvisibleButton("##Usage", ImVec2(width, height));
                    note_item(std::string("agent:usage:") + key);
                    const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
                    auto* meter_draw = ImGui::GetWindowDrawList();
                    const ImU32 fill = percent > 90 ? IM_COL32(242, 86, 86, 255) : percent > 80 ? IM_COL32(246, 199, 65, 255) : IM_COL32(195, 201, 211, 255);
                    meter_draw->AddRectFilled(minimum, maximum, colors.panel, height * .5F);
                    const float boundary = minimum.x + width * percent / 100;
                    if (known && percent > 0) {
                        meter_draw->PushClipRect(minimum, ImVec2(boundary, maximum.y), true);
                        meter_draw->AddRectFilled(minimum, maximum, fill, height * .5F); meter_draw->PopClipRect();
                    }
                    const std::string text = std::string(label) + ": " + (known ? std::to_string(static_cast<int>(std::round(percent))) + "%" : "--");
                    const float font_size = usage_font_size;
                    const auto size = ImGui::GetFont()->CalcTextSizeA(font_size, std::numeric_limits<float>::max(), 0, text.c_str());
                    const ImVec2 position(minimum.x + (width - size.x) * .5F, minimum.y + (height - size.y) * .5F);
                    meter_draw->PushClipRect(minimum, maximum, true);
                    meter_draw->PushClipRect(ImVec2(known ? boundary : minimum.x, minimum.y), maximum, true);
                    meter_draw->AddText(ImGui::GetFont(), font_size, position, IM_COL32(238, 241, 246, 255), text.c_str());
                    meter_draw->PopClipRect();
                    if (known && percent > 0) {
                        meter_draw->PushClipRect(minimum, ImVec2(boundary, maximum.y), true);
                        meter_draw->AddText(ImGui::GetFont(), font_size, position, IM_COL32(0, 0, 0, 255), text.c_str()); meter_draw->PopClipRect();
                    }
                    meter_draw->PopClipRect();
                    meter_draw->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border), height * .5F, 0, ui_scale);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", known ? "Account usage consumed in this window" : "Account usage is unavailable");
                    ImGui::PopID();
                    if (std::string_view(key) == "fiveHour") ImGui::SameLine(0, usage_gap);
                }

            if (send && connected && !busy && (chat_message[0] || !attachments.empty())) {
                std::string files = "[";
                for (const auto& filename : attachments) { if (files.size() > 1) files += ","; files += "\"" + json_escape(filename) + "\""; }
                files += "]";
                if (call("chat.submit", "\"message\":\"" + json_escape(chat_message.data()) + "\",\"attachments\":" + files)) { chat_message.fill('\0'); attachments.clear(); refresh_pending = true; }
            }
            if (show_status) { ImGui::TextWrapped("%s", full_status.substr(0, 120).c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", full_status.c_str()); }
            ImGui::EndTabItem();
        }
        const bool setup_tab = ImGui::BeginTabItem("Account", nullptr, agent_account_pending ? ImGuiTabItemFlags_SetSelected : 0);
        agent_account_pending = false;
        note_item("agent:setup-tab");
        if (setup_tab) {
            int provider_index = is_openai ? 0 : 1;
            ImGui::BeginDisabled(busy);
            if (ImGui::Combo("Provider", &provider_index, "OpenAI / ChatGPT\0Compatible API (advanced)\0"))
                control("provider", std::string("\"provider\":\"") + (provider_index == 0 ? "openai" : "compatible") + "\"");
            note_item("agent:provider-picker");
            ImGui::EndDisabled();
            if (is_openai) {
                ImGui::SeparatorText("OpenAI connection");
                const auto* account_value = openai ? field(*openai, "account") : nullptr;
                const auto* account = account_value ? account_value->object() : nullptr;
                const auto* login_value = openai ? field(*openai, "login") : nullptr;
                const auto* login = login_value ? login_value->object() : nullptr;
                if (account) {
                    ImGui::TextColored(editor_color(colors.success), "Connected to ChatGPT");
                    ImGui::TextWrapped("%s", string_or(*account, "email", "").c_str());
                    ImGui::TextDisabled("Plan: %s", string_or(*account, "plan", "").c_str());
                } else ImGui::TextWrapped("Use your ChatGPT account. No endpoint or API key is needed.");
                ImGui::BeginDisabled(busy || !connected);
                if (login) {
                    const auto code = string_or(*login, "userCode", "");
                    ImGui::TextDisabled("YOUR DEVICE CODE"); ImGui::PushFont(fonts.monospace, ImGui::GetFontSize() * 1.5F); ImGui::TextUnformatted(code.c_str()); ImGui::PopFont();
                    if (ImGui::Button("Copy code")) ImGui::SetClipboardText(code.c_str());
                    note_item("agent:copy-device-code");
                    if (ImGui::Button("Open sign-in page")) {
                        const auto url = string_or(*login, "verificationUrl", "");
                        if (!headless && url == "https://auth.openai.com/codex/device") (void)SDL_OpenURL(url.c_str());
                    }
                    note_item("agent:open-signin");
                    ImGui::TextWrapped("Enter the code on the OpenAI sign-in page. This panel updates when sign-in completes.");
                    if (ImGui::Button("Cancel sign-in")) control("cancel_signin");
                    note_item("agent:cancel-signin");
                } else {
                    if (ImGui::Button(account ? "Reconnect OpenAI" : "Sign in with ChatGPT", ImVec2(-1, 0))) control("signin");
                    note_item("agent:signin");
                }
                if (account) { if (ImGui::Button("Sign out")) control("signout"); note_item("agent:signout"); }
                if (ImGui::Button("Refresh account and models", ImVec2(-1, 0))) control("refresh");
                note_item("agent:refresh-models");
                ImGui::EndDisabled();
                ImGui::Spacing();
                ImGui::TextWrapped("%s", object ? string_or(*object, "status", "").c_str() : "Connecting...");
                ImGui::Separator();
                ImGui::TextDisabled("Private on this device  /  Excluded from Git");
                ImGui::TextWrapped("Relay keeps its own sign-in. Your other Codex apps keep their existing logins.");
            } else {
            const auto* settings = provider_settings;
            if (!provider_loaded && settings) {
                const auto copy = [](auto& buffer, const std::string& text) {
                    const auto count = std::min(buffer.size() - 1, text.size());
                    std::copy_n(text.data(), count, buffer.data()); buffer[count] = '\0';
                };
                copy(provider_endpoint, string_or(*settings, "endpoint", ""));
                copy(provider_model, string_or(*settings, "model", ""));
                copy(provider_header, string_or(*settings, "header", ""));
                const auto auth = string_or(*settings, "auth", "bearer");
                provider_auth = auth == "none" ? 2 : auth == "header" ? 1 : 0;
                provider_loaded = true;
            }
            ImGui::TextWrapped("Compatible chat completion endpoint. Authentication is stored privately in .relay/agent-provider.json by the external bridge.");
            ImGui::InputText("Endpoint", provider_endpoint.data(), provider_endpoint.size()); note_item("agent:endpoint");
            ImGui::InputText("Model", provider_model.data(), provider_model.size()); note_item("agent:model");
            ImGui::Combo("Authentication", &provider_auth, "Bearer token / API key\0Custom header\0None (local provider)\0");
            if (provider_auth == 1) ImGui::InputText("Header", provider_header.data(), provider_header.size());
            if (provider_auth != 2) {
                ImGui::InputText("Credential", provider_credential.data(), provider_credential.size(), ImGuiInputTextFlags_Password);
                note_item("agent:credential");
                ImGui::TextWrapped(settings && boolean_or(*settings, "authenticated", false) ? "Authentication configured. Leave blank to keep it." : "Enter an API key or authentication token.");
                editor_checkbox("Remove saved credential", &provider_clear);
            }
            ImGui::BeginDisabled(!connected || busy);
            if (ImGui::Button("Save provider")) {
                const auto payload = "\"endpoint\":\"" + json_escape(provider_endpoint.data()) + "\",\"model\":\"" + json_escape(provider_model.data()) +
                    "\",\"auth\":\"" + (provider_auth == 2 ? std::string("none") : provider_auth == 1 ? std::string("header") : std::string("bearer")) +
                    "\",\"header\":\"" + json_escape(provider_header.data()) + "\",\"credential\":\"" + json_escape(provider_credential.data()) +
                    "\",\"clear\":" + (provider_clear ? "true" : "false");
                if (call("chat.configure", payload)) { provider_clear = false; refresh_pending = true; }
                provider_credential.fill('\0');
            }
            note_item("agent:save-provider");
            ImGui::EndDisabled();
            if (object) ImGui::TextWrapped("%s", string_or(*object, "setup_status", "").c_str());
            }
            ImGui::EndTabItem();
        }
        const bool access_tab = ImGui::BeginTabItem("Access");
        note_item("agent:access-tab");
        if (access_tab) {
            bool automatic = session_object && boolean_or(*session_object, "auto_approval", false);
            if (editor_checkbox("Allow all actions", &automatic))
                (void)mutate("session.auto_approval", std::string("\"enabled\":") + (automatic ? "true" : "false"), automatic ? "All actions allowed" : "Action approvals required");
            note_item("agent:auto-approval");
            ImGui::TextDisabled("Project: %s", session_object ? string_or(*session_object, "project", "").c_str() : "");
            if (ImGui::Button("Revoke all", ImVec2(120 * ui_scale, 0)))
                (void)mutate("session.revoke", {}, "All agent access revoked");
            note_item("agent:revoke");
            if (const auto* pending = review ? field(*review, "pending") : nullptr; pending && pending->array()) {
                for (const auto& item : *pending->array()) if (const auto* entry = item.object()) {
                    const auto id = static_cast<std::uint64_t>(number_or(*entry, "request", 0));
                    ImGui::PushID(static_cast<int>(id));
                    const auto scope = string_or(*entry, "scope", "");
                    const auto* specification = find_protocol_method(scope);
                    ImGui::TextWrapped("%s: %s (%s) %s", specification && specification->destructive ? "Destructive request" : "Request",
                        scope.c_str(), string_or(*entry, "kind", "").c_str(), string_or(*entry, "target", "").c_str());
                    const auto decision = [&](bool allow) {
                        (void)mutate("session.decide", "\"request\":" + std::to_string(id) + ",\"allow\":" + (allow ? "true" : "false"), allow ? "Access approved" : "Access denied");
                    };
                    if (ImGui::Button("Allow", ImVec2(90 * ui_scale, 0))) decision(true);
                    note_item("agent:allow:" + std::to_string(id));
                    ImGui::SameLine();
                    if (ImGui::Button("Deny", ImVec2(90 * ui_scale, 0))) decision(false);
                    note_item("agent:deny:" + std::to_string(id));
                    if (specification) ImGui::TextWrapped("%s", std::string(specification->description).c_str());
                    ImGui::TextDisabled("Project: %s", string_or(*entry, "project", "").c_str());
                    ImGui::PopID();
                }
            }
            ImGui::SeparatorText("Grant limited access");
            const auto* methods_value = review ? field(*review, "methods") : nullptr;
            const auto* methods = methods_value ? methods_value->array() : nullptr;
            if (methods && !methods->empty()) {
                grant_method = std::clamp(grant_method, 0, static_cast<int>(methods->size()) - 1);
                const auto* chosen = (*methods)[static_cast<std::size_t>(grant_method)].object();
                const auto chosen_scope = string_or(*chosen, "scope", "");
                if (ImGui::BeginCombo("Action", chosen_scope.c_str())) {
                    for (std::size_t i = 0; i < methods->size(); ++i) {
                        const auto* option = (*methods)[i].object();
                        if (ImGui::Selectable(string_or(*option, "scope", "").c_str(), static_cast<int>(i) == grant_method)) {
                            grant_method = static_cast<int>(i);
                            grant_kind = 0;
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextWrapped("%s", string_or(*chosen, "description", "").c_str());
                if (boolean_or(*chosen, "destructive", false)) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(editor_palette().warning), "This action is destructive.");
                const char* kinds[]{"Whole method", "Exact entity", "Exact file"};
                if (ImGui::BeginCombo("Scope", kinds[grant_kind])) {
                    for (int i = 0; i < 3; ++i) {
                        const bool supported = i == 0 || boolean_or(*chosen, i == 1 ? "entity_scope" : "file_scope", false);
                        ImGui::BeginDisabled(!supported);
                        if (ImGui::Selectable(kinds[i], grant_kind == i)) grant_kind = i;
                        ImGui::EndDisabled();
                    }
                    ImGui::EndCombo();
                }
                if (grant_kind) {
                    ImGui::InputText("Target", grant_target.data(), grant_target.size());
                    if (grant_kind == 2) ImGui::TextWrapped("Restricts the source/output filename. Loads and imports may also restore dependencies and change the scene.");
                    if (grant_kind == 1 && !selection.empty() && ImGui::Button("Use selected entity"))
                        std::snprintf(grant_target.data(), grant_target.size(), "%s", selection.c_str());
                } else ImGui::TextWrapped("Applies across the current project. File actions retain their native dependency and scene effects.");
                if (ImGui::Button("Grant", ImVec2(90 * ui_scale, 0))) {
                    constexpr const char* keys[]{"method", "entity", "file"};
                    (void)mutate("session.grant", "\"scope\":\"" + json_escape(chosen_scope) + "\",\"kind\":\"" + keys[grant_kind] +
                        "\",\"target\":\"" + json_escape(grant_kind ? grant_target.data() : "") + "\"", "Access granted");
                }
                note_item("agent:grant");
            }
            ImGui::SeparatorText("Active grants");
            if (session_object) {
                const auto revoke = [&](const std::string& scope) {
                    (void)mutate("session.revoke", "\"scope\":\"" + json_escape(scope) + "\"", "Access revoked");
                };
                if (const auto* grants = field(*session_object, "grants"); grants && grants->array())
                    for (const auto& grant : *grants->array()) if (grant.string()) {
                        ImGui::PushID(grant.string()->c_str());
                        ImGui::TextUnformatted(grant.string()->c_str()); ImGui::SameLine();
                        if (ImGui::SmallButton("Revoke")) revoke(*grant.string());
                        ImGui::PopID();
                    }
                if (const auto* grants = field(*session_object, "scoped_grants"); grants && grants->array()) {
                    int row_id = 0;
                    for (const auto& grant : *grants->array()) if (const auto* entry = grant.object()) {
                        ImGui::PushID(++row_id);
                        const auto scope = string_or(*entry, "scope", "");
                        ImGui::TextWrapped("%s: %s %s", scope.c_str(), string_or(*entry, "kind", "").c_str(), string_or(*entry, "target", "").c_str());
                        if (ImGui::SmallButton("Revoke method")) revoke(scope);
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndTabItem();
        }
        const bool actions_tab = ImGui::BeginTabItem("Activity");
        note_item("agent:actions-tab");
        if (actions_tab) {
            ImGui::InputText("Audit file", audit_filename.data(), audit_filename.size());
            if (ImGui::Button("Export audit"))
                (void)mutate("session.export_audit", "\"filename\":\"" + json_escape(audit_filename.data()) + "\"", "Audit exported under .relay/audits");
            note_item("agent:export");
            ImGui::TextDisabled("Latest 256 decisions. Exports preserve existing files.");
            ImGui::SeparatorText("Tool activity");
            if (object) if (const auto* results = field(*object, "results"); results && results->array()) {
                for (std::size_t index = 0; index < results->array()->size(); ++index) if (const auto* entry = (*results->array())[index].object()) {
                    const bool succeeded = boolean_or(*entry, "succeeded", false);
                    ImGui::PushID(static_cast<int>(index));
                    ImGui::PushStyleColor(ImGuiCol_Text, editor_color(succeeded ? colors.success : colors.danger));
                    const auto label = std::string(succeeded ? "Succeeded  /  " : "Failed  /  ") + string_or(*entry, "scope", "");
                    const bool details = ImGui::CollapsingHeader(label.c_str());
                    ImGui::PopStyleColor();
                    if (details) { ImGui::PushFont(fonts.monospace); ImGui::TextWrapped("%s", string_or(*entry, "summary", "").c_str()); ImGui::PopFont(); }
                    ImGui::PopID();
                }
            }
            if (is_openai && openai && ImGui::CollapsingHeader("Connection diagnostics")) {
                if (const auto* transport = field(*openai, "transport"); transport && transport->object())
                    ImGui::TextWrapped("%s", json_stringify(*transport).c_str());
                if (const auto* entries = field(*openai, "diagnostics"); entries && entries->array())
                    for (const auto& entry : *entries->array()) ImGui::TextWrapped("%s", json_stringify(entry).c_str());
            }
            ImGui::SeparatorText("Session audit");
            if (const auto* audit_object = agent_audit.object()) {
                if (const auto* entries = field(*audit_object, "entries"); entries && entries->array())
                    for (auto it = entries->array()->rbegin(); it != entries->array()->rend(); ++it) if (const auto* entry = it->object()) {
                        const char* state = !boolean_or(*entry, "allowed", false) ? "Denied" : boolean_or(*entry, "succeeded", false) ? "Succeeded" : "Failed";
                        ImGui::TextWrapped("%.0f  %s  %s", number_or(*entry, "sequence", 0), state, string_or(*entry, "scope", "").c_str());
                    }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void set_status(std::string message, const bool error) {
        status_message = std::move(message);
        status_is_error = error;
    }

    static std::string entity_field(const std::string_view handle) {
        return "\"entity\":\"" + std::string(handle) + '"';
    }

    // Refreshes every panel at once, as needed straight after an edit.
    void refresh() {
        refresh_pending = false;
        refresh_stage = -1;
        for (int stage = 0; stage < refresh_stages; ++stage) refresh_part(stage);
    }

    // Periodic refreshes are spread over consecutive frames so that no single frame pays for
    // every panel read, which showed as a regular hitch in the viewport.
    void advance_refresh() {
        if (refresh_stage < 0) refresh_stage = 0;
        refresh_part(refresh_stage);
        if (++refresh_stage == refresh_stages) refresh_stage = -1;
    }

    void refresh_part(const int stage) {
        if (stage == 0) {
            seconds_since_refresh = 0.0;
            if (auto status = call("runtime.status")) runtime_status = std::move(*status);
            if (auto project = call("project.status")) project_status = std::move(*project);
            if (auto clipboard = call("scene.clipboard"); clipboard && clipboard->object())
                clipboard_ready = number_or(*clipboard->object(), "entities", 0) > 0;
            if (auto list = call_if_changed("scene.list", scene_list_reply)) {
                scene_list = std::move(*list);
                rebuild_index();
            }
            animator_playing = false;
            if (const auto* entity = selection.empty() ? nullptr : find_entity(selection)) {
                if (const auto* animator = component(*entity, "animator"))
                    animator_playing = boolean_or(*animator, "playing", false);
            }
            if (panel_open[6])
                for (const auto& target : animation_targets())
                    if (const auto* entity = find_entity(target))
                        if (const auto* animator = component(*entity, "animator"))
                            animator_playing |= boolean_or(*animator, "playing", false);
            if (auto logs = call("logs.read", "\"after\":" + std::to_string(last_log_sequence))) {
                append_logs(*logs);
            }
            refresh_scripts();
            refresh_game_input();
            if (auto history = call("scene.history")) {
                const auto* object = history->object();
                const auto collect = [&](const std::string_view key, std::vector<std::string>& target) {
                    target.clear();
                    if (object == nullptr) return;
                    const auto* value = field(*object, key);
                    if (value == nullptr || value->array() == nullptr) return;
                    for (const auto& item : *value->array()) {
                        if (const auto* text = item.string()) target.push_back(*text);
                    }
                };
                collect("undo", undo_labels);
                collect("redo", redo_labels);
                if (object != nullptr) {
                    if (const auto* state = field(*object, "state"); state && state->object())
                        scene_revision =
                            static_cast<std::uint64_t>(number_or(*state->object(), "revision", 0.0));
                }
            }
            return;
        }
        if (stage == 1) {
            // The overlay is fetched only while it can be drawn.
            if (collider_wireframes_enabled && panel_open[5] && camera_enabled &&
                !(runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game")) {
                if (auto boxes = call_if_changed("physics.debug_boxes", collider_boxes_reply))
                    collider_boxes = std::move(*boxes);
            } else {
                collider_boxes_reply.clear();
                collider_boxes = JsonValue{};
            }
            if (panel_open[5] && camera_enabled &&
                !(runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game")) {
                if (auto shapes = call_if_changed("audio.debug_shapes", audio_shapes_reply))
                    audio_shapes = std::move(*shapes);
            } else {
                audio_shapes_reply.clear();
                audio_shapes = JsonValue{};
            }
            return;
        }
        if (stage == 2) {
            if (!panel_open[8]) return;
            if (auto result = call_if_changed("session.review", agent_review_reply))
                agent_review = std::move(*result);
            if (auto result = call_if_changed("session.audit", agent_audit_reply))
                agent_audit = std::move(*result);
            if (auto result = call("chat.status")) chat_status = std::move(*result);
            return;
        }
        // Assets are refreshed on a cadence rather than only after a UI-driven import, because an
        // agent sharing this runtime can import a model at any time and the human's mesh and
        // material lists must reflect that.
        const bool periodic = seconds_since_assets >= refresh_interval_seconds;
        if (periodic) seconds_since_assets = 0.0;
        if (!periodic && !assets_pending) return;
        refresh_assets();
        if (auto projects = call("project.list"); projects && projects->object()) {
            project_files.clear();
            if (const auto* list = field(*projects->object(), "projects"); list && list->array())
                for (const auto& file : *list->array())
                    if (file.string()) project_files.push_back(*file.string());
        }
        refresh_asset_listing();
        refresh_templates();
        refresh_audio_files();
        refresh_sky_files();
        refresh_shader_files();
        assets_pending = false;
    }

    void rebuild_index() {
        entities.clear();
        children.clear();
        roots.clear();
        entity_index.clear();
        const auto* object = scene_list.object();
        if (object == nullptr) return;
        const auto* list = field(*object, "entities");
        if (list == nullptr || list->array() == nullptr) return;
        for (const auto& value : *list->array()) {
            if (const auto* entity = value.object()) entities.push_back(entity);
        }
        for (std::size_t index = 0; index < entities.size(); ++index) {
            entity_index.emplace(string_or(*entities[index], "entity"), index);
            const auto parent = string_or(*entities[index], "parent");
            if (parent.empty())
                roots.push_back(index);
            else
                children[parent].push_back(index);
        }
        // A destroyed selection must not keep driving the inspector.
        const auto previous = selection;
        selections.prune([&](const auto& handle) { return find_entity(handle) != nullptr; });
        if (previous != selection) drafts.clear();
    }

    [[nodiscard]] const JsonValue::Object* find_entity(const std::string_view handle) const {
        const auto found = entity_index.find(handle);
        return found == entity_index.end() ? nullptr : entities[found->second];
    }

    void refresh_assets() {
        auto assets = call("render.assets");
        if (!assets) return;
        mesh_names.clear();
        material_names.clear();
        morph_defaults.clear();
        model_clips.clear();
        const auto* object = assets->object();
        if (object == nullptr) return;
        const auto collect = [&](const std::string_view key, std::vector<std::string>& target) {
            const auto* value = field(*object, key);
            if (value == nullptr || value->array() == nullptr) return;
            for (const auto& item : *value->array()) {
                if (const auto* text = item.string())
                    target.push_back(*text);
                else if (const auto* record = item.object()) {
                    auto name = string_or(*record, "name");
                    if (!name.empty()) target.push_back(std::move(name));
                }
            }
        };
        collect("meshes", mesh_names);
        collect("materials", material_names);
        registry_items.clear();
        for (const auto* key : {"meshes", "materials"})
            if (const auto* list = field(*object, key); list && list->array())
                for (const auto& item : *list->array())
                    if (const auto* record = item.object()) {
                        RegistryItem entry{string_or(*record, "label"), string_or(*record, "source"), std::nullopt};
                        if (const auto* color = field(*record, "color"); color && color->array() && color->array()->size() >= 3U) {
                            std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
                            for (std::size_t index = 0; index < std::min<std::size_t>(4U, color->array()->size()); ++index)
                                if (const auto value = (*color->array())[index].number()) rgba[index] = static_cast<float>(*value);
                            entry.color = rgba;
                        }
                        registry_items[string_or(*record, "name")] = std::move(entry);
                    }
        if (const auto* meshes = field(*object, "meshes"); meshes && meshes->array())
            for (const auto& mesh : *meshes->array())
                if (const auto* record = mesh.object())
                    if (const auto* defaults = field(*record, "morph_default_weights");
                        defaults && defaults->array()) {
                        auto& target = morph_defaults[string_or(*record, "name")];
                        for (const auto& value : *defaults->array())
                            if (value.number()) target.push_back(*value.number());
                    }
        if (const auto* models = field(*object, "models"); models && models->array())
            for (const auto& model : *models->array()) {
                const auto* record = model.object();
                if (record == nullptr) continue;
                auto& target = model_clips[string_or(*record, "name")];
                const auto* clips = field(*record, "clips");
                if (clips == nullptr || clips->array() == nullptr) continue;
                for (const auto& clip : *clips->array())
                    if (const auto* entry = clip.object())
                        target.push_back(ClipInfo{string_or(*entry, "name"),
                                                  number_or(*entry, "duration_seconds", 0.0)});
            }
    }

    [[nodiscard]] std::span<const ClipInfo> clips_for(const std::string_view model) const {
        const auto found = model_clips.find(model);
        return found == model_clips.end() ? std::span<const ClipInfo>{} : found->second;
    }

    void append_logs(const JsonValue& logs) {
        const auto* object = logs.object();
        if (object == nullptr) return;
        const auto* entries = field(*object, "entries");
        if (entries == nullptr || entries->array() == nullptr) return;
        for (const auto& value : *entries->array()) {
            const auto* entry = value.object();
            if (entry == nullptr) continue;
            const auto sequence = static_cast<std::uint64_t>(number_or(*entry, "sequence", 0.0));
            last_log_sequence = std::max(last_log_sequence, sequence);
            log_lines.push_back('[' + string_or(*entry, "level", "info") + "] " +
                                string_or(*entry, "message"));
            if (log_lines.size() > maximum_log_lines) log_lines.pop_front();
        }
    }

    void select(const std::string& handle) {
        if (selection != handle) drafts.clear();
        selections.assign(handle);
    }

    void click_selection(const std::string& handle, bool tree = false) {
        const auto& io = ImGui::GetIO();
        selections.click(handle, io.KeyCtrl, tree && io.KeyShift, visible_rows);
        drafts.clear();
    }

    std::string selection_fields() const {
        std::string result = "\"entities\":[";
        for (std::size_t i = 0; i < selections.handles.size(); ++i) {
            if (i) result += ',';
            result += '"' + selections.handles[i] + '"';
        }
        return result + "]";
    }

    void update_view() {
        view.position = navigation_camera.position();
        view.target = navigation_camera.target;
        view.camera = Camera{};
        view.camera.near_plane = 0.05;
        view.camera.far_plane = 5000.0;
    }

    void draw_collider_wireframes() {
        if (!collider_wireframes_enabled || !camera_enabled || !viewport_visible ||
            !viewport_draw_list || (runtime_status.object() &&
            string_or(*runtime_status.object(), "mode") == "game")) return;
        const auto* result = collider_boxes.object();
        const auto* values = result ? field(*result, "boxes") : nullptr;
        const auto* boxes = values ? values->array() : nullptr;
        if (!boxes) return;
        const double width = viewport_max.x - viewport_min.x;
        const double height = viewport_max.y - viewport_min.y;
        if (width <= 0.0 || height <= 0.0) return;
        const auto matrix = editor_view(view.position, view.target);
        const double focal = 1.0 / std::tan(view.camera.field_of_view_y_degrees *
                                            3.14159265358979323846 / 360.0);
        const double near = view.camera.near_plane;
        const auto read_vector = [](const JsonValue* value, Vec3& output) {
            const auto* array = value ? value->array() : nullptr;
            if (!array || array->size() != 3) return false;
            const auto* x = (*array)[0].number();
            const auto* y = (*array)[1].number();
            const auto* z = (*array)[2].number();
            if (!x || !y || !z) return false;
            output = {*x, *y, *z};
            return true;
        };
        const auto camera_point = [&](const Vec3 point) {
            return Vec3{
                matrix[0] * point.x + matrix[4] * point.y + matrix[8] * point.z + matrix[12],
                matrix[1] * point.x + matrix[5] * point.y + matrix[9] * point.z + matrix[13],
                matrix[2] * point.x + matrix[6] * point.y + matrix[10] * point.z + matrix[14]};
        };
        const auto project = [&](const Vec3 point) {
            const double depth = -point.z;
            return ImVec2{
                static_cast<float>(viewport_min.x + width * 0.5 +
                                   point.x * focal * height * 0.5 / depth),
                static_cast<float>(viewport_min.y + height * 0.5 -
                                   point.y * focal * height * 0.5 / depth)};
        };
        const auto add = [](const Vec3 a, const Vec3 b) {
            return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
        };
        const auto scaled = [](const Vec3 a, const double s) { return Vec3{a.x * s, a.y * s, a.z * s}; };
        viewport_draw_list->PushClipRect(viewport_min, viewport_max, true);
        for (const auto& value : *boxes) {
            const auto* box = value.object();
            if (!box) continue;
            Vec3 center{};
            std::array<Vec3, 3> edges{};
            const auto* edge_value = field(*box, "edges");
            const auto* edge_array = edge_value ? edge_value->array() : nullptr;
            if (!read_vector(field(*box, "center"), center) || !edge_array ||
                edge_array->size() != 3 || !read_vector(&(*edge_array)[0], edges[0]) ||
                !read_vector(&(*edge_array)[1], edges[1]) ||
                !read_vector(&(*edge_array)[2], edges[2])) continue;
            const auto selected = string_or(*box, "entity") == selection;
            const auto enabled = boolean_or(*box, "enabled", true);
            const ImU32 color = selected ? IM_COL32(255, 194, 67, 255) :
                                  enabled ? IM_COL32(75, 224, 174, 190) :
                                            IM_COL32(147, 156, 165, 110);
            const float thickness = selected ? 2.0F : 1.4F;
            const auto line = [&](const Vec3 from, const Vec3 to) {
                auto a = camera_point(from), b = camera_point(to);
                if (a.z > -near && b.z > -near) return;
                if (a.z > -near || b.z > -near) {
                    const double fraction = (-near - a.z) / (b.z - a.z);
                    const Vec3 clipped{a.x + fraction * (b.x - a.x),
                                       a.y + fraction * (b.y - a.y), -near};
                    if (a.z > -near) a = clipped; else b = clipped;
                }
                const auto first = project(a), last = project(b);
                if (!std::isfinite(first.x) || !std::isfinite(first.y) ||
                    !std::isfinite(last.x) || !std::isfinite(last.y)) return;
                viewport_draw_list->AddLine(first, last, color, thickness);
            };
            // Circle or arc of `radius` around `origin` in the plane spanned by unit u and v.
            const auto arc = [&](const Vec3 origin, const Vec3 u, const Vec3 v, const double radius,
                                 const double start, const double sweep) {
                constexpr int segments = 32;
                const int count = std::max(4, static_cast<int>(segments * sweep /
                                                               (2.0 * 3.14159265358979323846)));
                Vec3 previous{};
                for (int step = 0; step <= count; ++step) {
                    const double angle = start + sweep * step / count;
                    const auto point = add(origin, add(scaled(u, radius * std::cos(angle)),
                                                       scaled(v, radius * std::sin(angle))));
                    if (step) line(previous, point);
                    previous = point;
                }
            };
            const auto type = string_or(*box, "type", "box");
            const auto radius = number_or(*box, "radius", 0.0);
            constexpr double tau = 2.0 * 3.14159265358979323846;
            if (type == "sphere" && radius > 0.0) {
                arc(center, {1, 0, 0}, {0, 1, 0}, radius, 0.0, tau);
                arc(center, {0, 1, 0}, {0, 0, 1}, radius, 0.0, tau);
                arc(center, {0, 0, 1}, {1, 0, 0}, radius, 0.0, tau);
                continue;
            }
            Vec3 axis{};
            if (type == "capsule" && radius > 0.0 && read_vector(field(*box, "axis"), axis)) {
                const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
                const Vec3 up = length > 0.0 ? scaled(axis, 1.0 / length) : Vec3{0, 1, 0};
                const Vec3 seed = std::abs(up.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
                const Vec3 side_raw{up.y * seed.z - up.z * seed.y, up.z * seed.x - up.x * seed.z,
                                    up.x * seed.y - up.y * seed.x};
                const double side_length = std::sqrt(side_raw.x * side_raw.x +
                                                      side_raw.y * side_raw.y +
                                                      side_raw.z * side_raw.z);
                const Vec3 side = scaled(side_raw, 1.0 / side_length);
                const Vec3 depth{up.y * side.z - up.z * side.y, up.z * side.x - up.x * side.z,
                                 up.x * side.y - up.y * side.x};
                const auto top = add(center, axis), bottom = add(center, scaled(axis, -1.0));
                arc(top, side, depth, radius, 0.0, tau);
                arc(bottom, side, depth, radius, 0.0, tau);
                for (const auto& offset : {side, scaled(side, -1.0), depth, scaled(depth, -1.0)})
                    line(add(top, scaled(offset, radius)), add(bottom, scaled(offset, radius)));
                arc(top, side, up, radius, 0.0, tau / 2.0);
                arc(top, depth, up, radius, 0.0, tau / 2.0);
                arc(bottom, side, up, radius, tau / 2.0, tau / 2.0);
                arc(bottom, depth, up, radius, tau / 2.0, tau / 2.0);
                continue;
            }
            const auto* line_value = field(*box, "lines");
            const auto* lines = line_value ? line_value->array() : nullptr;
            bool drew_lines = false;
            if (lines && (type == "convex" || type == "mesh")) {
                for (std::size_t index = 0; index + 1U < lines->size(); index += 2U) {
                    Vec3 a{}, b{};
                    if (!read_vector(&(*lines)[index], a) || !read_vector(&(*lines)[index + 1U], b))
                        continue;
                    line(a, b);
                    drew_lines = true;
                }
                // A partial outline still shows the full extent through its bounds.
                if (drew_lines && !boolean_or(*box, "lines_truncated", false)) continue;
            }
            std::array<Vec3, 8> corners{};
            for (unsigned corner = 0; corner < 8; ++corner) {
                Vec3 point = center;
                for (unsigned index = 0; index < 3; ++index) {
                    const double sign = (corner & (1U << index)) ? 1.0 : -1.0;
                    point = add(point, scaled(edges[index], sign));
                }
                corners[corner] = point;
            }
            for (unsigned corner = 0; corner < 8; ++corner)
                for (unsigned index = 0; index < 3; ++index) {
                    const unsigned other = corner ^ (1U << index);
                    if (corner < other) line(corners[corner], corners[other]);
                }
        }
        // Joints: a cross at the anchor, the hinge or slider axis, and a line to the partner.
        const auto* joint_values = field(*result, "joints");
        if (joint_values && joint_values->array())
            for (const auto& value : *joint_values->array()) {
                const auto* joint = value.object();
                Vec3 anchor{}, axis{}, partner{};
                if (!joint || !read_vector(field(*joint, "anchor"), anchor) ||
                    !read_vector(field(*joint, "axis"), axis)) continue;
                const auto selected = string_or(*joint, "entity") == selection;
                const auto enabled = boolean_or(*joint, "enabled", true);
                const ImU32 color = selected ? IM_COL32(255, 194, 67, 255) :
                                      enabled ? IM_COL32(176, 140, 255, 220) :
                                                IM_COL32(147, 156, 165, 110);
                const float thickness = selected ? 2.0F : 1.4F;
                const auto segment = [&](const Vec3 from, const Vec3 to) {
                    auto a = camera_point(from), b = camera_point(to);
                    if (a.z > -near && b.z > -near) return;
                    if (a.z > -near || b.z > -near) {
                        const double fraction = (-near - a.z) / (b.z - a.z);
                        const Vec3 clipped{a.x + fraction * (b.x - a.x),
                                           a.y + fraction * (b.y - a.y), -near};
                        if (a.z > -near) a = clipped; else b = clipped;
                    }
                    const auto first = project(a), last = project(b);
                    if (std::isfinite(first.x) && std::isfinite(first.y) &&
                        std::isfinite(last.x) && std::isfinite(last.y))
                        viewport_draw_list->AddLine(first, last, color, thickness);
                };
                constexpr double cross = 0.08;
                for (const Vec3 direction : {Vec3{cross, 0, 0}, Vec3{0, cross, 0}, Vec3{0, 0, cross}})
                    segment(add(anchor, scaled(direction, -1.0)), add(anchor, direction));
                const auto type = string_or(*joint, "type");
                if (type == "hinge" || type == "slider")
                    segment(add(anchor, scaled(axis, -0.4)), add(anchor, scaled(axis, 0.4)));
                if (read_vector(field(*joint, "partner"), partner)) segment(anchor, partner);
            }
        viewport_draw_list->PopClipRect();
    }

    // Draws world-space lines over the viewport from the editor camera, clipped at its near plane.
    struct ViewportPen {
        EditorMatrix eye;
        double focal{}, near{}, width{}, height{};
        ImVec2 origin;
        ImDrawList* list{};

        Vec3 to_eye(const Vec3 point) const {
            return {eye[0] * point.x + eye[4] * point.y + eye[8] * point.z + eye[12],
                    eye[1] * point.x + eye[5] * point.y + eye[9] * point.z + eye[13],
                    eye[2] * point.x + eye[6] * point.y + eye[10] * point.z + eye[14]};
        }
        ImVec2 project(const Vec3 point) const {
            const double depth = -point.z;
            return {static_cast<float>(origin.x + width * 0.5 + point.x * focal * height * 0.5 / depth),
                    static_cast<float>(origin.y + height * 0.5 - point.y * focal * height * 0.5 / depth)};
        }
        void line(const Vec3 from, const Vec3 to, const ImU32 color, const float thickness) const {
            auto a = to_eye(from), b = to_eye(to);
            if (a.z > -near && b.z > -near) return;
            if (a.z > -near || b.z > -near) {
                const double fraction = (-near - a.z) / (b.z - a.z);
                const Vec3 clipped{a.x + fraction * (b.x - a.x), a.y + fraction * (b.y - a.y), -near};
                if (a.z > -near) a = clipped; else b = clipped;
            }
            const auto first = project(a), last = project(b);
            if (std::isfinite(first.x) && std::isfinite(first.y) && std::isfinite(last.x) &&
                std::isfinite(last.y))
                list->AddLine(first, last, color, thickness);
        }
        // A circle of `radius` about `center` in the plane of unit vectors u and v.
        void circle(const Vec3 center, const Vec3 u, const Vec3 v, const double radius,
                    const ImU32 color, const float thickness) const {
            constexpr int segments = 48;
            Vec3 previous{};
            for (int step = 0; step <= segments; ++step) {
                const double angle = 2.0 * 3.14159265358979323846 * step / segments;
                const Vec3 point{center.x + radius * (u.x * std::cos(angle) + v.x * std::sin(angle)),
                                 center.y + radius * (u.y * std::cos(angle) + v.y * std::sin(angle)),
                                 center.z + radius * (u.z * std::cos(angle) + v.z * std::sin(angle))};
                if (step) line(previous, point, color, thickness);
                previous = point;
            }
        }
        void sphere(const Vec3 center, const double radius, const ImU32 color, const float thickness) const {
            circle(center, {1, 0, 0}, {0, 1, 0}, radius, color, thickness);
            circle(center, {0, 1, 0}, {0, 0, 1}, radius, color, thickness);
            circle(center, {0, 0, 1}, {1, 0, 0}, radius, color, thickness);
        }
        // A box from its centre and three half-edge vectors.
        void box(const Vec3 center, const std::array<Vec3, 3>& edges, const ImU32 color,
                 const float thickness) const {
            std::array<Vec3, 8> corners{};
            for (unsigned corner = 0; corner < 8U; ++corner) {
                Vec3 point = center;
                for (unsigned axis = 0; axis < 3U; ++axis) {
                    const double sign = corner & (1U << axis) ? 1.0 : -1.0;
                    point = {point.x + sign * edges[axis].x, point.y + sign * edges[axis].y,
                             point.z + sign * edges[axis].z};
                }
                corners[corner] = point;
            }
            for (unsigned corner = 0; corner < 8U; ++corner)
                for (unsigned axis = 0; axis < 3U; ++axis)
                    if (const unsigned other = corner ^ (1U << axis); corner < other)
                        line(corners[corner], corners[other], color, thickness);
        }
    };

    [[nodiscard]] std::optional<ViewportPen> viewport_pen() const {
        if (!camera_enabled || !viewport_visible || !viewport_draw_list) return std::nullopt;
        const double width = viewport_max.x - viewport_min.x;
        const double height = viewport_max.y - viewport_min.y;
        if (width <= 0.0 || height <= 0.0) return std::nullopt;
        return ViewportPen{editor_view(view.position, view.target),
                           1.0 / std::tan(view.camera.field_of_view_y_degrees *
                                          3.14159265358979323846 / 360.0),
                           view.camera.near_plane, width, height, viewport_min, viewport_draw_list};
    }

    // Reverb zones (every one faintly, the selected one bright, with its fade margin) and the
    // selected spatial source's minimum and maximum distance.
    void draw_audio_shapes() {
        if (runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game") return;
        const auto pen = viewport_pen();
        const auto* result = audio_shapes.object();
        if (!pen || !result) return;
        const auto vector = [](const JsonValue* value, Vec3& output) {
            const auto* array = value ? value->array() : nullptr;
            if (!array || array->size() != 3U) return false;
            for (const auto& item : *array)
                if (!item.number()) return false;
            output = {*(*array)[0].number(), *(*array)[1].number(), *(*array)[2].number()};
            return true;
        };
        viewport_draw_list->PushClipRect(viewport_min, viewport_max, true);
        if (const auto* zones = field(*result, "zones"); zones && zones->array())
            for (const auto& value : *zones->array()) {
                const auto* zone = value.object();
                Vec3 center{};
                if (!zone || !vector(field(*zone, "center"), center)) continue;
                const bool selected = string_or(*zone, "entity") == selection;
                const ImU32 inner = selected ? IM_COL32(186, 140, 255, 255) : IM_COL32(160, 130, 230, 110);
                const ImU32 outer = selected ? IM_COL32(186, 140, 255, 140) : IM_COL32(160, 130, 230, 45);
                const float thickness = selected ? 2.0F : 1.2F;
                const double fade = number_or(*zone, "fade", 0.0);
                if (string_or(*zone, "shape") == "sphere") {
                    const double radius = number_or(*zone, "radius", 0.0);
                    pen->sphere(center, radius, inner, thickness);
                    if (fade > 0.0) pen->sphere(center, radius + fade, outer, 1.0F);
                    continue;
                }
                const auto* edge_values = field(*zone, "edges");
                std::array<Vec3, 3> edges{};
                if (!edge_values || !edge_values->array() || edge_values->array()->size() != 3U ||
                    !vector(&(*edge_values->array())[0], edges[0]) ||
                    !vector(&(*edge_values->array())[1], edges[1]) ||
                    !vector(&(*edge_values->array())[2], edges[2]))
                    continue;
                pen->box(center, edges, inner, thickness);
                if (fade <= 0.0) continue;
                // The fade margin lies `fade` metres outside each face.
                std::array<Vec3, 3> grown = edges;
                for (auto& edge : grown) {
                    const double length = std::sqrt(edge.x * edge.x + edge.y * edge.y + edge.z * edge.z);
                    if (length <= 0.0) continue;
                    const double scale = (length + fade) / length;
                    edge = {edge.x * scale, edge.y * scale, edge.z * scale};
                }
                pen->box(center, grown, outer, 1.0F);
            }
        if (const auto* sources = field(*result, "sources"); sources && sources->array())
            for (const auto& value : *sources->array()) {
                const auto* source = value.object();
                Vec3 center{};
                if (!source || string_or(*source, "entity") != selection ||
                    !vector(field(*source, "center"), center))
                    continue;
                pen->sphere(center, number_or(*source, "min_distance", 1.0), IM_COL32(255, 204, 92, 230), 1.6F);
                pen->sphere(center, number_or(*source, "max_distance", 50.0), IM_COL32(255, 204, 92, 90), 1.0F);
            }
        viewport_draw_list->PopClipRect();
    }

    void draw_scene_nodes() {
        node_markers.clear();
        if (!camera_enabled || !viewport_visible || !viewport_draw_list ||
            (runtime_status.object() &&
             string_or(*runtime_status.object(), "mode") == "game")) return;
        const double width = viewport_max.x - viewport_min.x;
        const double height = viewport_max.y - viewport_min.y;
        if (width <= 0.0 || height <= 0.0) return;
        const auto eye = editor_view(view.position, view.target);
        const double focal = 1.0 / std::tan(view.camera.field_of_view_y_degrees *
                                            3.14159265358979323846 / 360.0);
        const double near = view.camera.near_plane;
        const auto eye_point = [&](Vec3 point) {
            return Vec3{eye[0] * point.x + eye[4] * point.y + eye[8] * point.z + eye[12],
                        eye[1] * point.x + eye[5] * point.y + eye[9] * point.z + eye[13],
                        eye[2] * point.x + eye[6] * point.y + eye[10] * point.z + eye[14]};
        };
        const auto project = [&](Vec3 point) -> std::optional<ImVec2> {
            if (point.z > -near + 1e-7 * near) return std::nullopt;
            const double depth = -point.z;
            const double x = viewport_min.x + width * 0.5 +
                             point.x * focal * height * 0.5 / depth;
            const double y = viewport_min.y + height * 0.5 -
                             point.y * focal * height * 0.5 / depth;
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e7 ||
                std::abs(y) > 1e7) return std::nullopt;
            return ImVec2{static_cast<float>(x), static_cast<float>(y)};
        };
        const auto line_world = [&](Vec3 first, Vec3 last, ImU32 color, float thickness) {
            auto a = eye_point(first), b = eye_point(last);
            if (a.z >= -near && b.z >= -near) return;
            if (a.z >= -near || b.z >= -near) {
                const double fraction = (-near - a.z) / (b.z - a.z);
                const Vec3 clipped{a.x + fraction * (b.x - a.x),
                                   a.y + fraction * (b.y - a.y), -near};
                if (a.z >= -near) a = clipped; else b = clipped;
            }
            const auto screen_a = project(a), screen_b = project(b);
            if (screen_a && screen_b)
                viewport_draw_list->AddLine(*screen_a, *screen_b, color, thickness);
        };
        viewport_draw_list->PushClipRect(viewport_min, viewport_max, true);
        std::size_t drawn = 0;
        for (const auto* entity : entities) {
            if (drawn >= 2048U) break;
            const auto* camera = component(*entity, "camera");
            const auto* light = component(*entity, "light");
            if (!camera && !light) continue;
            ++drawn;
            const auto handle = string_or(*entity, "entity");
            const auto name = string_or(*entity, "name", handle);
            const auto world = visual_world_of(handle);
            const Vec3 origin{world[12], world[13], world[14]};
            const bool selected = handle == selection;
            if (camera && selected && camera_wireframes_enabled) {
                Camera settings;
                settings.field_of_view_y_degrees = number_or(*camera, "field_of_view_y_degrees", 60);
                settings.near_plane = number_or(*camera, "near_plane", 0.1);
                settings.far_plane = number_or(*camera, "far_plane", 1000);
                settings.orthographic_height = number_or(*camera, "orthographic_height", 0);
                const auto corners = editor_camera_guide(settings, width / height, world);
                constexpr ImU32 color = IM_COL32(98, 204, 255, 215);
                for (unsigned plane = 0; plane < 2; ++plane) {
                    const unsigned offset = plane * 4U;
                    for (unsigned edge = 0; edge < 4; ++edge) {
                        if (edge < (edge ^ 1U))
                            line_world(corners[offset + edge], corners[offset + (edge ^ 1U)], color, 1.5F);
                        if (edge < (edge ^ 2U))
                            line_world(corners[offset + edge], corners[offset + (edge ^ 2U)], color, 1.5F);
                    }
                }
                for (unsigned corner = 0; corner < 4; ++corner)
                    line_world(corners[corner], corners[corner + 4U], color, 1.5F);
            }
            if (!node_icons_enabled) continue;
            const auto camera_space = eye_point(origin);
            const auto center = project(camera_space);
            if (!center || center->x < viewport_min.x - 20 || center->x > viewport_max.x + 20 ||
                center->y < viewport_min.y - 20 || center->y > viewport_max.y + 20) continue;
            const auto draw_icon = [&](ImVec2 at, bool camera_icon, int light_type) {
                const float radius = camera_icon ? 12.0F : 11.0F;
                const ImU32 color = camera_icon ? IM_COL32(98, 204, 255, 255) :
                                    light_type == 0 ? IM_COL32(255, 219, 112, 255) :
                                    light_type == 2 ? IM_COL32(255, 161, 91, 255) :
                                                      IM_COL32(255, 235, 130, 255);
                if (camera_icon) {
                    viewport_draw_list->AddRectFilled({at.x - radius, at.y - radius},
                        {at.x + radius, at.y + radius}, IM_COL32(19, 25, 34, 255), 4.0F);
                    viewport_draw_list->AddRect({at.x - radius, at.y - radius},
                        {at.x + radius, at.y + radius},
                        selected ? IM_COL32(255, 194, 67, 255) : color, 4.0F, 0,
                        selected ? 2.0F : 1.2F);
                    viewport_draw_list->AddRectFilled({at.x - 8, at.y - 5},
                        {at.x + 1, at.y + 5}, color, 2.0F);
                    viewport_draw_list->AddQuadFilled(
                        {at.x + 1, at.y - 4}, {at.x + 7, at.y - 2},
                        {at.x + 7, at.y + 2}, {at.x + 1, at.y + 4}, color);
                } else {
                    viewport_draw_list->AddCircleFilled(at, radius, IM_COL32(19, 25, 34, 225), 16);
                    viewport_draw_list->AddCircle(at, radius,
                        selected ? IM_COL32(255, 194, 67, 255) : color,
                        16, selected ? 2.0F : 1.2F);
                }
                if (!camera_icon && light_type == 0) {
                    viewport_draw_list->AddCircleFilled(ImVec2(at.x - 4, at.y - 4), 2.2F, color, 8);
                    for (float offset : {-4.0F, 0.0F, 4.0F})
                        viewport_draw_list->AddLine(ImVec2(at.x - 1, at.y + offset),
                                                    ImVec2(at.x + 6, at.y + offset + 3), color, 1.4F);
                } else if (!camera_icon && light_type == 2) {
                    viewport_draw_list->AddTriangle(ImVec2(at.x - 6, at.y),
                        ImVec2(at.x + 5, at.y - 5), ImVec2(at.x + 5, at.y + 5), color, 1.5F);
                    viewport_draw_list->AddCircleFilled(ImVec2(at.x - 6, at.y), 1.7F, color, 8);
                } else if (!camera_icon) {
                    viewport_draw_list->AddCircleFilled(at, 3.0F, color, 12);
                    for (unsigned ray = 0; ray < 8; ++ray) {
                        const float angle = static_cast<float>(ray) * 0.78539816F;
                        const float x = std::cos(angle), y = std::sin(angle);
                        viewport_draw_list->AddLine(ImVec2(at.x + x * 5, at.y + y * 5),
                                                    ImVec2(at.x + x * 8, at.y + y * 8), color, 1.3F);
                    }
                }
                const char* kind = camera_icon ? "Camera" : light_type == 0 ? "Directional light" :
                                   light_type == 2 ? "Spot light" : "Point light";
                node_markers.push_back({handle, std::string(kind) + ": " + name, at,
                                        static_cast<float>(-camera_space.z), radius});
                if (headless) headless_items["node:" + handle + (camera_icon ? ":camera" : ":light")] =
                    {at.x - radius, at.y - radius, at.x + radius, at.y + radius};
            };
            if (camera) draw_icon(*center, true, 0);
            if (light) {
                const int type = static_cast<int>(number_or(*light, "type", 1));
                draw_icon({center->x + (camera ? 23.0F : 0.0F), center->y}, false, type);
            }
        }
        viewport_draw_list->PopClipRect();
        if (viewport_hovered)
            if (const auto* marker = marker_at(ImGui::GetIO().MousePos))
                ImGui::SetTooltip("%s", marker->label.c_str());
    }

    [[nodiscard]] bool pointer_in_viewport() const {
        const auto pointer = ImGui::GetIO().MousePos;
        return pointer.x >= viewport_min.x && pointer.x < viewport_max.x &&
               pointer.y >= viewport_min.y && pointer.y < viewport_max.y;
    }

    void capture_pointer(bool capture, bool restore = true) {
        // Recorded even without a window, so headless tests can see what the editor asked for.
        capture_requested = capture;
        if (capture == mouse_captured || !sdl_window) return;
        if (capture) mouse_anchor = ImGui::GetIO().MousePos;
        if (!SDL_SetWindowRelativeMouseMode(sdl_window, capture)) return;
        mouse_captured = capture;
        relative_delta = {};
        if (!capture && restore) SDL_WarpMouseInWindow(sdl_window, mouse_anchor.x, mouse_anchor.y);
    }

    // Godot-style perspective navigation: MMB orbit, Shift+MMB pan, RMB freelook.
    void update_camera_input() {
        const auto& io = ImGui::GetIO();
        // While the game has input the pointer is the game's: editor navigation must not release
        // the lock the game asked for, which it otherwise does every frame it is not flying.
        if (game_input_focus) {
            freelook_latched = false;
            navigating = false;
            relative_delta = {};
            capture_pointer(game_lock_mouse, false);
            return;
        }
        if (!camera_enabled || io.WantTextInput) {
            freelook_latched = false;
            navigating = false;
            capture_pointer(false);
            return;
        }
        const bool inside =
            viewport_visible && pointer_in_viewport() && (viewport_hovered || mouse_captured);
        if (inside && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
            freelook_latched = !freelook_latched;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) freelook_latched = false;
        const bool freelook =
            inside && (freelook_latched || ImGui::IsMouseDown(ImGuiMouseButton_Right));
        const bool middle = inside && ImGui::IsMouseDown(ImGuiMouseButton_Middle);
        navigating = freelook || middle;
        const bool already_captured = mouse_captured;
        const auto motion = relative_delta;
        relative_delta = {};
        capture_pointer(freelook && !gizmo_active);
        if (!inside || gizmo_active) return;

        const auto delta = mouse_captured ? (already_captured ? motion : ImVec2{}) : io.MouseDelta;
        if (freelook) {
            navigation_camera.turn(delta.x, delta.y, true);
            const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyDown(key) ? 1.0 : 0.0; };
            navigation_camera.fly(pressed(ImGuiKey_W) - pressed(ImGuiKey_S),
                                  pressed(ImGuiKey_D) - pressed(ImGuiKey_A),
                                  pressed(ImGuiKey_E) - pressed(ImGuiKey_Q), io.DeltaTime,
                                  io.KeyShift ? 3.0
                                  : io.KeyAlt ? 0.25
                                              : 1.0);
        } else if (middle) {
            if (io.KeyShift)
                navigation_camera.pan(delta.x, delta.y, viewport_max.y - viewport_min.y,
                                      view.camera.field_of_view_y_degrees);
            else
                navigation_camera.turn(delta.x, delta.y, false);
        }
        if (io.MouseWheel != 0.0F) navigation_camera.wheel(io.MouseWheel, freelook);
    }

    void update_selection_input() {
        const auto& io = ImGui::GetIO();
        if (!camera_enabled || !viewport_hovered || !pointer_in_viewport() || navigating) return;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !gizmo_active &&
            (!ImGuizmo::IsOver() || marker_at(io.MousePos))) {
            const auto drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0F);
            if (std::abs(drag.x) < 3.0F && std::abs(drag.y) < 3.0F) pick_at(io.MousePos);
        }
    }

    void focus_selection() {
        if (selection.empty()) return;
        const auto bounds = call("scene.bounds", entity_field(selection));
        if (!bounds) return;
        const auto* object = bounds->object();
        if (object == nullptr) return;
        auto minimum = editor_vector(*object, "minimum", {{0.0, 0.0, 0.0}});
        auto maximum = editor_vector(*object, "maximum", {{0.0, 0.0, 0.0}});
        const auto* geometry = field(*object, "has_geometry");
        bool has_geometry = geometry && geometry->boolean() && *geometry->boolean();
        for (const auto& handle : selections.handles) {
            if (handle == selection) continue;
            const auto other = call("scene.bounds", entity_field(handle));
            if (!other || !other->object()) continue;
            const auto lo = editor_vector(*other->object(), "minimum", {{0, 0, 0}});
            const auto hi = editor_vector(*other->object(), "maximum", {{0, 0, 0}});
            for (std::size_t axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], lo[axis]);
                maximum[axis] = std::max(maximum[axis], hi[axis]);
            }
            has_geometry |= boolean_or(*other->object(), "has_geometry", false);
        }
        const auto width = viewport_max.x - viewport_min.x;
        const auto height = viewport_max.y - viewport_min.y;
        navigation_camera.frame(
            {minimum[0], minimum[1], minimum[2]}, {maximum[0], maximum[1], maximum[2]},
            has_geometry, view.camera.field_of_view_y_degrees, height > 0 ? width / height : 1.0);
        freelook_latched = false;
        set_status("Framed " + selection, false);
    }

    // Selects whatever the pointer is over by asking the engine to intersect a world-space ray.
    // The ray is built here and sent with the request, so the engine stores no viewpoint and an
    // agent can pick with its own ray without an editor running.
    [[nodiscard]] const NodeMarker* marker_at(ImVec2 pointer) const {
        const NodeMarker* nearest = nullptr;
        for (const auto& marker : node_markers) {
            const float dx = pointer.x - marker.screen.x;
            const float dy = pointer.y - marker.screen.y;
            if (dx * dx + dy * dy > marker.radius * marker.radius) continue;
            if (!nearest || marker.depth < nearest->depth) nearest = &marker;
        }
        return nearest;
    }

    void pick_at(const ImVec2 pointer) {
        if (const auto* marker = marker_at(pointer)) {
            click_selection(marker->handle);
            return;
        }
        const auto width = viewport_max.x - viewport_min.x;
        const auto height = viewport_max.y - viewport_min.y;
        if (width <= 0.0F || height <= 0.0F) return;
        const double horizontal =
            2.0 * static_cast<double>(pointer.x - viewport_min.x) / static_cast<double>(width) -
            1.0;
        const double vertical = 1.0 - 2.0 * static_cast<double>(pointer.y - viewport_min.y) /
                                          static_cast<double>(height);
        const auto aspect = static_cast<double>(width) / static_cast<double>(height);
        const auto direction =
            editor_screen_ray(view.position, view.target, view.camera.field_of_view_y_degrees,
                              aspect, horizontal, vertical);

        const auto pick = call("scene.pick", "\"origin_x\":" + number_text(view.position.x) +
                                                 ",\"origin_y\":" + number_text(view.position.y) +
                                                 ",\"origin_z\":" + number_text(view.position.z) +
                                                 ",\"direction_x\":" + number_text(direction.x) +
                                                 ",\"direction_y\":" + number_text(direction.y) +
                                                 ",\"direction_z\":" + number_text(direction.z));
        if (!pick) return;
        const auto* object = pick->object();
        if (object == nullptr) return;
        const auto entity = string_or(*object, "entity");
        if (entity.empty()) {
            if (!ImGui::GetIO().KeyCtrl) select({});
            set_status("Nothing under the pointer", false);
            return;
        }
        click_selection(entity);
    }

    // Local transform of one entity straight from the cached scene listing.
    [[nodiscard]] EditorMatrix local_of(const std::string_view handle) const {
        const auto* record = find_entity(handle);
        if (record == nullptr) return editor_identity();
        const auto* transform = component(*record, "transform");
        if (transform == nullptr) return editor_identity();
        const auto position = editor_vector(*transform, "position", {{0.0, 0.0, 0.0}});
        const auto rotation = editor_vector(*transform, "rotation_degrees", {{0.0, 0.0, 0.0}});
        const auto scale = editor_vector(*transform, "scale", {{1.0, 1.0, 1.0}});
        return editor_compose({position[0], position[1], position[2]},
                              {rotation[0], rotation[1], rotation[2]},
                              {scale[0], scale[1], scale[2]});
    }

    [[nodiscard]] EditorMatrix visual_local_of(const std::string_view handle) const {
        const auto* record = find_entity(handle);
        if (!record) return editor_identity();
        const auto* transform = component(*record, "transform");
        if (!transform) return editor_identity();
        const auto position = editor_vector(*transform, "position", {{0, 0, 0}});
        const auto rotation = editor_vector(*transform, "rotation_degrees", {{0, 0, 0}});
        const auto scale = editor_vector(*transform, "scale", {{1, 1, 1}});
        Transform pose{{position[0], position[1], position[2]},
                       {rotation[0], rotation[1], rotation[2]},
                       {scale[0], scale[1], scale[2]}};
        if (const auto* track = component(*record, "transform_animation")) {
            const auto* keys_value = field(*track, "keys");
            const auto* keys = keys_value ? keys_value->array() : nullptr;
            if (keys && !keys->empty()) {
                TransformAnimation animation;
                animation.time_seconds = number_or(*track, "time_seconds", 0);
                animation.duration_seconds = number_or(*track, "duration_seconds", 1);
                for (const auto& value : *keys) {
                    const auto* key = value.object();
                    if (!key) continue;
                    const auto key_position = editor_vector(*key, "position", {{0, 0, 0}});
                    const auto key_rotation = editor_vector(*key, "rotation_degrees", {{0, 0, 0}});
                    const auto key_scale = editor_vector(*key, "scale", {{1, 1, 1}});
                    animation.keys.push_back({number_or(*key, "time_seconds", 0),
                        {{key_position[0], key_position[1], key_position[2]},
                         {key_rotation[0], key_rotation[1], key_rotation[2]},
                         {key_scale[0], key_scale[1], key_scale[2]}}});
                }
                pose = sample_transform_animation(animation, pose);
            }
        }
        return editor_compose(pose.position, pose.rotation_degrees, pose.scale);
    }

    [[nodiscard]] EditorMatrix visual_world_of(std::string_view handle) const {
        std::vector<std::string> chain;
        while (!handle.empty() && chain.size() < 256U) {
            const auto* record = find_entity(handle);
            if (!record) return editor_identity();
            chain.emplace_back(handle);
            handle = string_or(*record, "parent");
        }
        auto world = editor_identity();
        for (auto item = chain.rbegin(); item != chain.rend(); ++item)
            world = editor_multiply(world, visual_local_of(*item));
        return world;
    }

    // World transform of an entity's parent chain, composed from the cached tree. Imported nodes
    // driven by animation are not accounted for here: the gizmo edits the stored transform, which
    // is what scene.set_transform writes, not the animated pose layered on top of it.
    [[nodiscard]] EditorMatrix parent_world_of(const std::string_view handle) const {
        const auto* record = find_entity(handle);
        if (record == nullptr) return editor_identity();
        auto parent = string_or(*record, "parent");
        if (parent.empty()) return editor_identity();
        std::vector<std::string> chain;
        while (!parent.empty() && chain.size() < 256U) {
            chain.push_back(parent);
            const auto* ancestor = find_entity(parent);
            if (ancestor == nullptr) break;
            parent = string_or(*ancestor, "parent");
        }
        auto world = editor_identity();
        for (auto item = chain.rbegin(); item != chain.rend(); ++item) {
            world = editor_multiply(world, local_of(*item));
        }
        return world;
    }

    void draw_gizmo() {
        const auto width = viewport_max.x - viewport_min.x;
        const auto height = viewport_max.y - viewport_min.y;
        if ((runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game") ||
            selection.empty() || !camera_enabled || !viewport_visible || width <= 0.0F ||
            height <= 0.0F) {
            gizmo_active = false;
            return;
        }
        if (find_entity(selection) == nullptr) {
            gizmo_active = false;
            return;
        }

        ImGuizmo::SetOrthographic(false);
        // Keep the scene and gizmo in the viewport window's draw order, so floating
        // panels above it occlude both and own their pointer input.
        ImGuizmo::SetDrawlist(viewport_draw_list);
        ImGuizmo::SetRect(viewport_min.x, viewport_min.y, width, height);

        const auto aspect = static_cast<double>(width) / static_cast<double>(height);
        const auto projection = editor_projection(view.camera.field_of_view_y_degrees, aspect,
                                                  view.camera.near_plane, view.camera.far_plane);
        // ImGuizmo needs a rigid camera view for stable screen-space sizing. Manipulate the
        // world matrix, then convert back through the parent inverse; never put parent scale in
        // view.
        const auto parent = parent_world_of(selection);
        const auto inverse_parent = editor_inverse_affine(parent);
        if (!inverse_parent) {
            gizmo_active = false;
            return;
        }
        const auto camera_view = editor_view(view.position, view.target);
        EditorMatrix world = editor_multiply(parent, local_of(selection));
        const auto before_world = world;
        const double depth = -(camera_view[2] * world[12] + camera_view[6] * world[13] +
                               camera_view[10] * world[14] + camera_view[14]);
        // A gizmo at/behind the eye or in the near plane cannot have a finite useful screen size.
        if (depth <= view.camera.near_plane * 2.0) {
            gizmo_active = false;
            return;
        }
        auto* draw_list = viewport_draw_list;
        draw_list->PushClipRect(viewport_min, viewport_max, true);
        ImGuizmo::Enable(!navigating);
        ImGuizmo::SetGizmoSizeClipSpace(0.12F);
        const bool used = ImGuizmo::Manipulate(camera_view.data(), projection.data(),
                                               gizmo_operation, gizmo_mode, world.data());
        draw_list->PopClipRect();
        const bool using_gizmo = ImGuizmo::IsUsing();
        if (using_gizmo && !gizmo_active) gizmo_gesture = ++gesture_serial;
        if (used && using_gizmo) {
            Vec3 translation{};
            Vec3 rotation{};
            Vec3 scale{};
            // Decomposed with Relay's own Euler order, not the gizmo library's, so the values
            // written back are exactly the ones the renderer will use.
            editor_decompose(editor_multiply(*inverse_parent, world), translation, rotation, scale);
            // Every update after the first continues the same gesture, so the whole drag collapses
            // into one undo entry instead of one per frame.
            std::string fields = entity_field(selection);
            if (gizmo_operation == ImGuizmo::TRANSLATE)
                fields += vector_fields({translation.x, translation.y, translation.z},
                                        {"px", "py", "pz"});
            else if (gizmo_operation == ImGuizmo::ROTATE)
                fields += vector_fields({rotation.x, rotation.y, rotation.z}, {"rx", "ry", "rz"});
            else if (gizmo_operation == ImGuizmo::SCALE)
                fields += vector_fields({scale.x, scale.y, scale.z}, {"sx", "sy", "sz"});
            fields += ",\"gesture\":" + std::to_string(gizmo_gesture);
            if (selections.handles.size() == 1) mutate("scene.set_transform", fields, "Gizmo transform");
            else if (const auto inverse = editor_inverse_affine(before_world)) {
                const auto delta = editor_multiply(world, *inverse);
                std::string group = selection_fields() + ",\"delta\":[";
                for (std::size_t i = 0; i < delta.size(); ++i) {
                    if (i) group += ',';
                    group += number_text(delta[i]);
                }
                group += "],\"gesture\":" + std::to_string(gizmo_gesture);
                mutate("scene.transform_many", group, "Transformed selection");
            }
        }
        gizmo_active = using_gizmo;
    }

    // Rename helpers shared by the hierarchy and the asset browser.
    // Panel visibility and View menu toggles persist with the dock layout.
    void bind_preferences() {
        for (std::size_t index = 0; index < panel_names.size(); ++index)
            layout.bind(std::string("panel.") + panel_names[index], &panel_open[index]);
        layout.bind("view.ground_grid", &grid_enabled);
        layout.bind("view.collider_wireframes", &collider_wireframes_enabled);
        layout.bind("view.node_icons", &node_icons_enabled);
        layout.bind("view.camera_wireframes", &camera_wireframes_enabled);
    }

    void begin_rename(const RenameKind kind, const std::string& target, const std::string& name) {
        inline_rename = {};
        inline_rename.kind = kind;
        inline_rename.target = target;
        const auto length = std::min(name.size(), inline_rename.buffer.size() - 1U);
        std::copy_n(name.begin(), length, inline_rename.buffer.begin());
        inline_rename.buffer[length] = '\0';
        slow_click = {};
    }

    [[nodiscard]] bool renaming(const RenameKind kind, const std::string& target) const {
        return inline_rename.kind == kind && inline_rename.target == target;
    }

    enum class RenameResult { editing, commit, cancel };
    // Draws the rename field at the cursor. Enter or clicking away commits; Escape cancels.
    RenameResult rename_field(const std::string& item_name, const float width) {
        if (inline_rename.frames++ == 0) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(std::max(width, 40.0F * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x * 0.5F, 0.0F));
        const bool entered = ImGui::InputText("##inline_rename", inline_rename.buffer.data(),
                                              inline_rename.buffer.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue |
                                                  ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleVar();
        note_item(item_name);
        if (entered) return RenameResult::commit;
        if (ImGui::IsItemActive()) {
            inline_rename.activated = true;
            return RenameResult::editing;
        }
        if (inline_rename.activated)
            return ImGui::IsKeyPressed(ImGuiKey_Escape, false) ? RenameResult::cancel
                                                               : RenameResult::commit;
        // Focus lands a frame after the request; give up if it never arrives.
        return inline_rename.frames > 3 ? RenameResult::cancel : RenameResult::editing;
    }

    void note_slow_click(const RenameKind kind, const std::string& target, const bool eligible) {
        if (eligible && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            slow_click = {kind, target, ImGui::GetTime()};
        else
            slow_click = {};
    }

    void update_slow_click() {
        if (slow_click.kind == RenameKind::none) return;
        const auto& io = ImGui::GetIO();
        if (ImGui::GetTime() - slow_click.time < static_cast<double>(io.MouseDoubleClickTime) ||
            ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
        const auto pending = slow_click;
        slow_click = {};
        const float threshold = io.MouseDragThreshold;
        if (io.MouseDragMaxDistanceSqr[0] > threshold * threshold) return;
        if (pending.kind == RenameKind::entity && selection == pending.target &&
            selections.handles.size() == 1U) {
            if (const auto* entity = find_entity(pending.target))
                begin_rename(RenameKind::entity, pending.target, string_or(*entity, "name"));
        } else if (pending.kind == RenameKind::asset && asset_selection == pending.target) {
            if (const auto* entry = asset_entry(pending.target); entry && !entry->locked)
                begin_rename(RenameKind::asset, pending.target, entry->name);
        }
    }

    void refresh_templates() {
        node_templates.clear();
        const auto listed = call("templates.list", {}, false);
        const auto* object = listed ? listed->object() : nullptr;
        const auto* list = object ? field(*object, "templates") : nullptr;
        if (!list || !list->array()) return;
        const auto strings = [](const JsonValue::Object& entry, const char* key) {
            std::vector<std::string> values;
            if (const auto* items = field(entry, key); items && items->array())
                for (const auto& value : *items->array())
                    if (value.string()) values.push_back(*value.string());
            return values;
        };
        for (const auto& item : *list->array())
            if (const auto* entry = item.object())
                node_templates.push_back({string_or(*entry, "id"), string_or(*entry, "name"),
                                          string_or(*entry, "type"), strings(*entry, "components"),
                                          strings(*entry, "behaviours")});
    }

    // Creates a node from a template under `parent`, optionally at a world position, and selects it.
    void instantiate_template(const std::string& id, const std::string& parent = {},
                              std::optional<Vec3> position = {}) {
        std::string fields = "\"template\":\"" + json_escape(id) + '"';
        if (!parent.empty()) fields += ",\"parent\":\"" + parent + '"';
        const auto created = call("templates.instantiate", fields);
        if (!created || !created->object()) return;
        const auto handle = string_or(*created->object(), "entity");
        if (position)
            (void)call("scene.set_transform", entity_field(handle) + ",\"px\":" +
                                                  number_text(position->x) + ",\"py\":" +
                                                  number_text(position->y) + ",\"pz\":" +
                                                  number_text(position->z));
        const auto found = std::find_if(node_templates.begin(), node_templates.end(),
                                        [&](const TemplateEntry& entry) { return entry.id == id; });
        set_status("Created " + (found != node_templates.end() ? found->name : id), false);
        refresh_pending = true;
        refresh();
        select(handle);
    }

    // Opens the Add Node window; with a parent, the new node starts as its child.
    void open_add_node(const std::string& parent) {
        node_parent = parent;
        node_as_child = !parent.empty();
        node_query.fill('\0');
        node_name.fill('\0');
        if (!node_type_catalog.object())
            if (auto types = call("nodes.types")) node_type_catalog = std::move(*types);
        if (!component_catalog.object())
            if (auto catalog = call("component.types")) component_catalog = std::move(*catalog);
        refresh_templates();
        open_node_window = true;
    }

    // Creates a node of a built-in type, or a copy of a saved template, and selects it.
    void create_node_from(const std::string& choice, const std::string& parent,
                          const std::string& name) {
        if (choice.starts_with("project:")) {
            std::string fields = "\"template\":\"" + json_escape(choice) + '"';
            if (!parent.empty()) fields += ",\"parent\":\"" + parent + '"';
            if (!name.empty()) fields += ",\"name\":\"" + json_escape(name) + '"';
            const auto created = call("templates.instantiate", fields);
            if (!created || !created->object()) return;
            set_status("Created " + choice.substr(8), false);
            refresh_pending = true;
            refresh();
            select(string_or(*created->object(), "entity"));
            return;
        }
        std::string fields = "\"type\":\"" + json_escape(choice) + '"';
        if (!parent.empty()) fields += ",\"parent\":\"" + parent + '"';
        if (!name.empty()) fields += ",\"name\":\"" + json_escape(name) + '"';
        const auto created = call("scene.create", fields);
        if (!created || !created->object()) return;
        set_status("Created " + (name.empty() ? choice : name), false);
        refresh_pending = true;
        refresh();
        select(string_or(*created->object(), "entity"));
    }

    void begin_save_template(const std::string& handle, const std::string& name) {
        template_save_entity = handle;
        template_name.fill('\0');
        std::string safe;
        for (const char character : name)
            if (std::isalnum(static_cast<unsigned char>(character)) || character == ' ' ||
                character == '-' || character == '_')
                safe += character;
        std::copy_n(safe.begin(), std::min<std::size_t>(safe.size(), 64U), template_name.begin());
        template_replace = false;
        open_template_dialog = true;
    }

    // Accepts a dragged template file and returns its template id when dropped.
    [[nodiscard]] std::optional<std::string> accept_template_drop() {
        const auto* payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->IsDataType("relay.asset")) return std::nullopt;
        const std::string path(static_cast<const char*>(payload->Data));
        const auto* entry = asset_entry(path);
        if (!entry || entry->kind != "template" || !path.starts_with("templates/") ||
            path.find('/', 10U) != std::string::npos)
            return std::nullopt;
        if (!ImGui::AcceptDragDropPayload("relay.asset")) return std::nullopt;
        constexpr std::string_view suffix = ".relay-template.json";
        return "project:" + entry->name.substr(0, entry->name.size() - suffix.size());
    }

    void create_entity(const std::string& parent) {
        std::string fields = "\"name\":\"Entity\"";
        if (!parent.empty()) fields += ",\"parent\":\"" + parent + '"';
        const auto created = call("scene.create", fields);
        if (!created || !created->object()) return;
        set_status("Entity created", false);
        refresh_pending = true;
        refresh();
        const auto handle = string_or(*created->object(), "entity");
        if (handle.empty()) return;
        select(handle);
        begin_rename(RenameKind::entity, handle, "Entity");
    }

    [[nodiscard]] static bool sound_file(const std::string_view path) {
        const auto dot = path.rfind('.');
        std::string extension(dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1U));
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == "wav" || extension == "flac" || extension == "mp3" || extension == "ogg";
    }

    // Accepts a dragged sound file inside a drag and drop target. Returns its path when dropped.
    [[nodiscard]] std::optional<std::string> accept_sound_drop() {
        const auto* payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->IsDataType("relay.asset")) return std::nullopt;
        std::string path(static_cast<const char*>(payload->Data));
        if (!sound_file(path) || !ImGui::AcceptDragDropPayload("relay.asset")) return std::nullopt;
        return path;
    }

    // A sound dropped into the viewport becomes an Audio Source node a metre above the point.
    void create_sound_node(const std::string& clip, const Vec3 at) {
        const auto name = base_name(clip);
        const auto created = call("scene.create", "\"name\":\"" + json_escape(name.substr(0, name.rfind('.'))) +
                                                      "\",\"type\":\"AudioSource\"");
        if (!created || !created->object()) return;
        const auto handle = string_or(*created->object(), "entity");
        (void)call("scene.set_transform", entity_field(handle) + ",\"px\":" + number_text(at.x) +
                                              ",\"py\":" + number_text(at.y + 1.0) +
                                              ",\"pz\":" + number_text(at.z));
        (void)call("scene.set_audio_source", entity_field(handle) + ",\"clip\":\"" + json_escape(clip) + '"');
        set_status("Placed " + name, false);
        refresh_pending = true;
        refresh();
        select(handle);
    }

    // Plays a sound file flat, as a preview, from the Assets panel.
    void preview_sound(const std::string& clip) {
        if (call("audio.play_clip", "\"clip\":\"" + json_escape(clip) + '"'))
            set_status("Previewing " + base_name(clip), false);
    }

    // Accepts a dragged importable asset. Returns its path when dropped.
    [[nodiscard]] std::optional<std::string> accept_model_drop() {
        const auto* payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->IsDataType("relay.asset")) return std::nullopt;
        const std::string path(static_cast<const char*>(payload->Data));
        const auto* entry = asset_entry(path);
        if (!entry || !entry->importable) return std::nullopt;
        if (!ImGui::AcceptDragDropPayload("relay.asset")) return std::nullopt;
        return path;
    }

    void draw_tree_node(const std::size_t index) {
        const auto* entity = entities[index];
        const auto handle = string_or(*entity, "entity");
        const auto name = string_or(*entity, "name", "Entity");
        const auto found = children.find(handle);
        const bool has_children = found != children.end() && !found->second.empty();
        const bool editing = renaming(RenameKind::entity, handle);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        drawing_rows.push_back(handle);

        if (selections.contains(handle)) flags |= ImGuiTreeNodeFlags_Selected;

        if (has_children && hierarchy_open_all)
            ImGui::SetNextItemOpen(*hierarchy_open_all, ImGuiCond_Always);
        else if (has_children && hierarchy_reveal.contains(handle))
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        // Hierarchy highlights meet edge-to-edge; other controls keep normal spacing.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        const bool open = ImGui::TreeNodeEx(handle.c_str(), flags, "%s", editing ? "" : name.c_str());
        ImGui::PopStyleVar();
        note_item("entity:" + handle);
        if (has_children && open) hierarchy_rows_open = true;
        if (hierarchy_scroll_to == handle) {
            ImGui::SetScrollHereY(0.5F);
            hierarchy_scroll_to.clear();
        }
        const auto row_min = ImGui::GetItemRectMin();
        const auto row_max = ImGui::GetItemRectMax();
        // The node type is derived from its components; plain nodes stay unlabelled.
        if (const auto type = string_or(*entity, "type", "Node"); !editing && type != "Node") {
            const auto size = ImGui::CalcTextSize(type.c_str());
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(row_max.x - size.x - 6.0F * ui_scale,
                       row_min.y + (row_max.y - row_min.y - size.y) * 0.5F),
                ImGui::GetColorU32(editor_color(editor_palette().text_faint)), type.c_str());
        }
        // Rows select when the button is released without a drag, so a node can be dragged onto a
        // field of the node the Inspector shows without selecting it first.
        if (!editing && ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            hierarchy_pressed = handle;
            hierarchy_press_double = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        }
        if (!editing && hierarchy_pressed == handle && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            const auto& io = ImGui::GetIO();
            hierarchy_pressed.clear();
            const bool dragged = io.MouseDragMaxDistanceSqr[0] > io.MouseDragThreshold * io.MouseDragThreshold;
            if (!dragged && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                const bool sole = selection == handle && selections.handles.size() == 1U;
                click_selection(handle, true);
                note_slow_click(RenameKind::entity, handle,
                                sole && !io.KeyCtrl && !io.KeyShift && !hierarchy_press_double);
            }
        }

        // Dragging one row onto another reparents it. The drop target rejects its own subtree
        // implicitly: scene.set_parent refuses cycles, and the failure surfaces as a status
        // message. Dropping a model file imports it as a child.
        if (!editing && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            ImGui::SetDragDropPayload("relay.entity", handle.c_str(), handle.size() + 1U);
            ImGui::Text("Reparent %s", name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("relay.entity")) {
                const std::string dragged(static_cast<const char*>(payload->Data));
                if (dragged != handle) {
                    mutate("scene.set_parent",
                           entity_field(dragged) + ",\"parent\":\"" + handle + '"', "Reparented");
                }
            }
            if (const auto model = accept_model_drop()) import_model(*model, "scene", handle);
            if (const auto dropped = accept_template_drop()) instantiate_template(*dropped, handle);
            ImGui::EndDragDropTarget();
        }

        if (!editing && ImGui::BeginPopupContextItem()) {
            if (!selections.contains(handle)) select(handle);
            if (ImGui::MenuItem("Add Child Node...")) open_add_node(handle);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
            if (ImGui::MenuItem("Save as template...")) begin_save_template(handle, name);
            if (ImGui::MenuItem("Move to root")) {
                mutate("scene.set_parent", entity_field(handle) + ",\"parent\":null", "Reparented");
            }
            if (ImGui::MenuItem("Rename", "F2")) begin_rename(RenameKind::entity, handle, name);
            if (ImGui::MenuItem("Destroy", "Del")) {
                mutate("scene.destroy", entity_field(handle), "Entity destroyed");
            }
            ImGui::EndPopup();
        }
        if (editing) {
            // The field replaces the label on the row itself, after the arrow.
            const float x = row_min.x + ImGui::GetTreeNodeToLabelSpacing();
            ImGui::SameLine(0.0F, 0.0F);
            ImGui::SetCursorScreenPos(ImVec2(x, row_min.y));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
            const auto result = rename_field("rename:entity", row_max.x - x);
            ImGui::PopStyleVar();
            if (result == RenameResult::commit) {
                const std::string renamed = inline_rename.buffer.data();
                if (!renamed.empty() && renamed != name)
                    mutate("scene.rename",
                           entity_field(handle) + ",\"name\":\"" + json_escape(renamed) + '"',
                           "Entity renamed");
            }
            if (result != RenameResult::editing) inline_rename = {};
        }
        if (open && has_children) {
            for (const auto child : found->second)
                draw_tree_node(child);
            ImGui::TreePop();
        }
    }

    void selectable_chat_text(const std::string& text, int block) {
        if (text.empty()) return;
        WrappedInput wrapped;
        wrapped.raw = text; wrapped.width = std::max(1.0F, ImGui::GetContentRegionAvail().x - 2);
        wrapped.wrap();
        std::vector<char> buffer(wrapped.display.begin(), wrapped.display.end()); buffer.push_back('\0');
        const auto lines = 1 + std::count(wrapped.display.begin(), wrapped.display.end(), '\n');
        ImGui::PushID(block);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0);
        ImGui::InputTextMultiline("##HistoryText", buffer.data(), buffer.size(),
            ImVec2(-1, static_cast<float>(lines) * ImGui::GetTextLineHeight() + 2),
            ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll);
        note_item("agent:history-text");
        // Copy the original text, excluding newlines inserted only for visual wrapping.
        if (ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
            if (const auto* state = ImGui::GetInputTextState(ImGui::GetItemID())) {
                const auto begin = wrapped.raw_position(std::min(state->GetSelectionStart(), state->GetSelectionEnd()));
                const auto end = wrapped.raw_position(std::max(state->GetSelectionStart(), state->GetSelectionEnd()));
                if (end > begin) ImGui::SetClipboardText(text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin)).c_str());
            }
        }
        ImGui::PopStyleVar(3); ImGui::PopStyleColor(); ImGui::PopID();
    }

    [[nodiscard]] bool hierarchy_search_active() const {
        return hierarchy_query[0] != '\0' || !hierarchy_type_filters.empty();
    }

    // Whether a derived node type is `wanted` or one of its subtypes.
    [[nodiscard]] bool type_within(std::string type, const std::string& wanted) const {
        for (int depth = 0; !type.empty() && depth < 32; ++depth) {
            if (type == wanted) return true;
            const auto* info = node_type_info(type);
            type = info ? string_or(*info, "parent") : std::string{};
        }
        return false;
    }

    // Ancestor names from the top, such as "Environment / Props"; empty for top-level nodes.
    // Where a node sits in the hierarchy, as "Parent / Child / Node", for tooltips.
    [[nodiscard]] std::string node_path(const std::string& handle) const {
        const auto* node = find_entity(handle);
        if (!node) return {};
        const auto parents = entity_parent_path(*node);
        return parents.empty() ? string_or(*node, "name") : parents + " / " + string_or(*node, "name");
    }

    [[nodiscard]] std::string entity_parent_path(const JsonValue::Object& entity) const {
        std::vector<std::string> names;
        for (auto parent = string_or(entity, "parent"); !parent.empty() && names.size() < 64;) {
            const auto* node = find_entity(parent);
            if (!node) break;
            names.insert(names.begin(), string_or(*node, "name"));
            parent = string_or(*node, "parent");
        }
        std::string path;
        for (const auto& name : names) path += (path.empty() ? "" : " / ") + name;
        return path;
    }

    // Leaves the search and shows `handle` in the tree, opening its ancestors.
    void reveal_entity(const std::string& handle) {
        hierarchy_query.fill('\0');
        hierarchy_type_filters.clear();
        const auto* entity = find_entity(handle);
        for (auto parent = entity ? string_or(*entity, "parent") : std::string{}; !parent.empty();) {
            hierarchy_reveal.insert(parent);
            const auto* node = find_entity(parent);
            parent = node ? string_or(*node, "parent") : std::string{};
        }
        hierarchy_scroll_to = handle;
        select(handle);
    }

    void ensure_node_type_catalog() {
        if (!node_type_catalog.object())
            if (auto types = call("nodes.types", {}, false)) node_type_catalog = std::move(*types);
    }

    // A square toolbar button drawn as a funnel, tinted while filters are active.
    bool filter_button(const char* id, const bool active) {
        const float size = ImGui::GetFrameHeight();
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, editor_palette().accent_soft);
        const bool clicked = ImGui::Button(id, ImVec2(size, size));
        if (active) ImGui::PopStyleColor();
        auto* list = ImGui::GetWindowDrawList();
        const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        const float width = high.x - low.x;
        const float cx = low.x + width * 0.5F, top = low.y + width * 0.28F;
        const float neck = low.y + width * 0.55F, bottom = low.y + width * 0.74F;
        const ImU32 color = active ? editor_palette().accent : editor_palette().text_dim;
        const ImVec2 funnel[]{{low.x + width * 0.24F, top}, {high.x - width * 0.24F, top},
                              {cx + width * 0.07F, neck}, {cx + width * 0.07F, bottom},
                              {cx - width * 0.07F, bottom + width * 0.05F}, {cx - width * 0.07F, neck}};
        list->AddConvexPolyFilled(funnel, 6, color);
        return clicked;
    }

    // A square toolbar button with two chevrons: pointing apart to expand every row, or together
    // to collapse them when `collapse` is set.
    bool expand_collapse_button(const char* id, const bool collapse) {
        const float size = ImGui::GetFrameHeight();
        const bool clicked = ImGui::Button(id, ImVec2(size, size));
        auto* list = ImGui::GetWindowDrawList();
        const auto low = ImGui::GetItemRectMin();
        const float width = ImGui::GetItemRectSize().x;
        const float cx = low.x + width * 0.5F, half = width * 0.2F, rise = width * 0.1F;
        const ImU32 color = ImGui::IsItemHovered() ? editor_palette().text : editor_palette().text_dim;
        const float thickness = std::max(1.0F, 1.5F * ui_scale);
        for (const float centre : {low.y + width * 0.33F, low.y + width * 0.67F}) {
            const bool upper = centre < low.y + width * 0.5F;
            // Expanding: the upper chevron points up and the lower down; collapsing, the reverse.
            const float tip = (upper != collapse) ? -rise : rise;
            const ImVec2 chevron[]{{cx - half, centre - tip}, {cx, centre + tip}, {cx + half, centre - tip}};
            list->AddPolyline(chevron, 3, color, ImDrawFlags_None, thickness);
        }
        return clicked;
    }

    void draw_hierarchy_search_bar() {
        const auto& style = ImGui::GetStyle();
        const float button = ImGui::GetFrameHeight();
        if (hierarchy_search_focus) {
            ImGui::SetKeyboardFocusHere();
            hierarchy_search_focus = false;
        }
        ImGui::SetNextItemWidth(-(button * 2.0F + style.ItemSpacing.x * 2.0F));
        ImGui::InputTextWithHint("##hierarchy_search", "Search nodes", hierarchy_query.data(),
                                 hierarchy_query.size());
        note_item("hierarchy:search");
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            hierarchy_query.fill('\0');
        ImGui::SameLine();
        ImGui::BeginDisabled(hierarchy_search_active());
        if (expand_collapse_button("##hierarchy_expand", hierarchy_any_open))
            hierarchy_open_all = !hierarchy_any_open;
        ImGui::EndDisabled();
        note_item("hierarchy:expand_all");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(hierarchy_any_open ? "Collapse all" : "Expand all");
        ImGui::SameLine();
        const bool filtering = !hierarchy_type_filters.empty();
        if (filter_button("##hierarchy_filter", filtering)) {
            ensure_node_type_catalog();
            ImGui::OpenPopup("##hierarchy_filters");
        }
        note_item("hierarchy:filter");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(filtering ? "Filter by node type (%zu active)" : "Filter by node type",
                              hierarchy_type_filters.size());
        const auto* catalog = node_type_catalog.object();
        const auto* types = catalog ? field(*catalog, "types") : nullptr;
        if (ImGui::BeginPopup("##hierarchy_filters")) {
            // Types are listed as their tree; choosing a category includes its subtypes.
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            if (types && types->array())
                for (const auto& value : *types->array()) {
                    const auto* type = value.object();
                    if (!type) continue;
                    const auto id = string_or(*type, "id");
                    int depth = 0;
                    for (auto parent = string_or(*type, "parent"); !parent.empty() && depth < 16; ++depth) {
                        const auto* info = node_type_info(parent);
                        parent = info ? string_or(*info, "parent") : std::string{};
                    }
                    const auto label = std::string(static_cast<std::size_t>(depth) * 3U, ' ') +
                                       string_or(*type, "name");
                    bool enabled = hierarchy_type_filters.contains(id);
                    if (ImGui::MenuItem(label.c_str(), nullptr, &enabled)) {
                        if (enabled) hierarchy_type_filters.insert(id);
                        else hierarchy_type_filters.erase(id);
                    }
                    note_item("hierarchy:filter:" + id);
                }
            ImGui::PopItemFlag();
            ImGui::Separator();
            if (ImGui::MenuItem("Clear filters", nullptr, false, !hierarchy_type_filters.empty()))
                hierarchy_type_filters.clear();
            ImGui::EndPopup();
        }
        // Active filters as removable chips.
        bool first = true;
        for (const auto& id : std::vector<std::string>(hierarchy_type_filters.begin(),
                                                       hierarchy_type_filters.end())) {
            const auto* info = node_type_info(id);
            const std::string chip = (info ? string_or(*info, "name") : id) + "  x##type_chip_" + id;
            const float width = ImGui::CalcTextSize(chip.c_str(), nullptr, true).x +
                                style.FramePadding.x * 2.0F;
            if (!first && ImGui::GetContentRegionAvail().x > width + style.ItemSpacing.x)
                ImGui::SameLine();
            first = false;
            if (ImGui::SmallButton(chip.c_str())) hierarchy_type_filters.erase(id);
        }
    }

    // One search result: the node's name with its ancestors, and its type on the right.
    void draw_hierarchy_result(const JsonValue::Object& entity) {
        const auto handle = string_or(entity, "entity");
        const auto name = string_or(entity, "name", "Entity");
        drawing_rows.push_back(handle);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (selections.contains(handle)) flags |= ImGuiTreeNodeFlags_Selected;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        ImGui::TreeNodeEx(("result:" + handle).c_str(), flags, "%s", name.c_str());
        ImGui::PopStyleVar();
        note_item("hierarchy:result:" + handle);
        const auto row_min = ImGui::GetItemRectMin(), row_max = ImGui::GetItemRectMax();
        if (ImGui::IsItemClicked()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) reveal_entity(handle);
            else click_selection(handle, true);
        }
        if (ImGui::BeginPopupContextItem()) {
            if (!selections.contains(handle)) select(handle);
            if (ImGui::MenuItem("Show in hierarchy")) reveal_entity(handle);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
            if (ImGui::MenuItem("Destroy", "Del")) mutate("scene.destroy", entity_field(handle), "Entity destroyed");
            ImGui::EndPopup();
        }
        auto* list = ImGui::GetWindowDrawList();
        const float text_y = row_min.y + (row_max.y - row_min.y - ImGui::GetTextLineHeight()) * 0.5F;
        const auto path = entity_parent_path(entity);
        const float name_end = row_min.x + ImGui::GetTreeNodeToLabelSpacing() +
                               ImGui::CalcTextSize(name.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
        const auto type = string_or(entity, "type", "Node");
        const auto type_size = ImGui::CalcTextSize(type.c_str());
        const float type_x = row_max.x - type_size.x - 6.0F * ui_scale;
        const auto faint = ImGui::GetColorU32(editor_color(editor_palette().text_faint));
        list->PushClipRect(ImVec2(name_end, row_min.y), ImVec2(type_x - 4.0F * ui_scale, row_max.y), true);
        list->AddText(ImVec2(name_end, text_y), faint, path.empty() ? "top level" : path.c_str());
        list->PopClipRect();
        if (type != "Node") list->AddText(ImVec2(type_x, text_y), faint, type.c_str());
    }

    void draw_hierarchy() {
        drawing_rows.clear();
        hierarchy_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (inline_rename.kind == RenameKind::entity && !find_entity(inline_rename.target))
            inline_rename = {};
        draw_hierarchy_search_bar();
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (begin_region("##tree", ImVec2(0.0F, -footer))) {
            if (hierarchy_search_active()) {
                const auto query = lowercase(hierarchy_query.data());
                std::size_t matches = 0;
                for (const auto* entity : entities) {
                    if (!query.empty() && lowercase(string_or(*entity, "name")).find(query) == std::string::npos)
                        continue;
                    const auto type = string_or(*entity, "type", "Node");
                    if (!hierarchy_type_filters.empty() &&
                        std::none_of(hierarchy_type_filters.begin(), hierarchy_type_filters.end(),
                                     [&](const std::string& wanted) { return type_within(type, wanted); }))
                        continue;
                    draw_hierarchy_result(*entity);
                    ++matches;
                }
                if (matches == 0)
                    ImGui::TextColored(editor_color(editor_palette().text_faint), "No matching nodes.");
            } else {
                hierarchy_rows_open = false;
                for (const auto root : roots)
                    draw_tree_node(root);
                hierarchy_any_open = hierarchy_rows_open;
                hierarchy_open_all.reset();
                hierarchy_reveal.clear();
            }
            // The space below the rows clears the selection, accepts drops at the root, and
            // offers entity creation.
            const auto available = ImGui::GetContentRegionAvail();
            ImGui::Dummy(ImVec2(std::max(available.x, 1.0F),
                                std::max(available.y, ImGui::GetFrameHeight())));
            note_item("hierarchy:empty");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl) select({});
            if (ImGui::BeginDragDropTarget()) {
                if (const auto* payload = ImGui::AcceptDragDropPayload("relay.entity")) {
                    const std::string dragged(static_cast<const char*>(payload->Data));
                    mutate("scene.set_parent", entity_field(dragged) + ",\"parent\":null",
                           "Reparented");
                }
                if (const auto model = accept_model_drop()) import_model(*model);
                if (const auto dropped = accept_template_drop()) instantiate_template(*dropped);
                ImGui::EndDragDropTarget();
            }
            if (ImGui::BeginPopupContextItem("##hierarchy_empty")) {
                if (ImGui::MenuItem("Add Node...")) open_add_node({});
                if (ImGui::MenuItem("Add Child of Selection...", nullptr, false, !selection.empty()))
                    open_add_node(selection);
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        visible_rows = drawing_rows;
        ImGui::Separator();
        if (ImGui::Button("+ Add Node", ImVec2(ImGui::GetContentRegionAvail().x, 0.0F)))
            open_add_node(selection);
        note_item("hierarchy:add_node");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(selection.empty() ? "Create a node" : "Create a node; it can go under the selection");
    }

    void draw_transform_section(const JsonValue::Object& entity) {
        if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return;
        const auto* transform = component(entity, "transform");
        if (!transform) return;
        constexpr std::array<const char*, 3> labels{"Position", "Rotation", "Scale"};
        constexpr std::array<const char*, 3> keys{"position", "rotation_degrees", "scale"};
        constexpr std::array<std::array<const char*, 3>, 3> wire{
            {{"px", "py", "pz"}, {"rx", "ry", "rz"}, {"sx", "sy", "sz"}}};
        std::string fields = entity_field(selection);
        bool committed = false;
        for (std::size_t row = 0; row < 3; ++row) {
            const auto defaults =
                row == 2 ? std::array<double, 3>{1, 1, 1} : std::array<double, 3>{0, 0, 0};
            auto values = editor_vector(*transform, keys[row], defaults);
            const auto mask =
                drag_vector3(labels[row], values, row == 1 ? 0.5F : 0.01F, 72.0F * ui_scale);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!(mask & (1U << axis))) continue;
                fields += ",\"" + std::string(wire[row][axis]) + "\":" + number_text(values[axis]);
                committed = true;
            }
        }
        if (committed) mutate("scene.set_transform", fields, "Transform updated");
    }

    void draw_camera_section(const JsonValue::Object& entity) {
        const auto* camera = component(entity, "camera");
        if (camera == nullptr || !component_header("Camera", "camera")) return;
        auto field_of_view = number_or(*camera, "field_of_view_y_degrees", 60.0);
        auto near_plane = number_or(*camera, "near_plane", 0.1);
        auto far_plane = number_or(*camera, "far_plane", 1000.0);
        const auto orthographic_height = number_or(*camera, "orthographic_height", 0.0);
        const bool active = boolean_or(*camera, "active", false);

        ImGui::Text("Projection: %s", orthographic_height > 0.0 ? "orthographic" : "perspective");
        std::string fields = entity_field(selection);
        bool commit = false;
        const auto scalar = [&](const char* label, const char* wire, double& value, float speed) {
            if (drag_scalar(label, value, speed)) {
                fields += ",\"" + std::string(wire) + "\":" + number_text(value);
                commit = true;
            }
        };
        scalar("FOV Y", "field_of_view_y_degrees", field_of_view, 0.25F);
        scalar("Near", "near_plane", near_plane, 0.01F);
        scalar("Far", "far_plane", far_plane, 1.0F);
        auto height = orthographic_height;
        scalar("Ortho height", "orthographic_height", height, 0.05F);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Zero selects perspective; positive values select orthographic");
        auto exposure = number_or(*camera, "exposure_ev", 0.0);
        if (drag_scalar("Exposure (EV)", exposure, 0.05F)) {
            fields += ",\"exposure_ev\":" + number_text(std::clamp(exposure, -16.0, 16.0));
            commit = true;
        }
        if (commit) mutate("scene.set_camera", fields, "Camera updated");
        if (!active && ImGui::Button("Make active")) {
            mutate("scene.set_camera", entity_field(selection) + ",\"active\":true",
                   "Camera activated");
        } else if (active) {
            ImGui::TextDisabled("Active camera");
        }
    }

    void draw_renderer_section(const JsonValue::Object& entity) {
        const auto* renderer = component(entity, "mesh_renderer");
        if (renderer == nullptr || !component_header("Mesh renderer", "mesh_renderer")) return;
        const auto mesh = renderer != nullptr ? string_or(*renderer, "mesh") : std::string{};
        const auto material =
            renderer != nullptr ? string_or(*renderer, "material") : std::string{};

        const auto handle = selection;
        const auto set_mesh = [this, handle, material](const std::string& chosen) {
            mutate("scene.set_renderer",
                   entity_field(handle) + ",\"enabled\":true,\"mesh\":\"" + json_escape(chosen) +
                       "\",\"material\":\"" + json_escape(material.empty() ? "builtin.orange" : material) + '"',
                   "Renderer updated");
        };
        asset_field(
            "Mesh", mesh, "inspector:renderer:mesh",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:renderer:mesh";
                pick.title = "Choose a mesh";
                pick.current = mesh;
                add_registry_choices(pick, mesh_names, true);
                pick.choose = set_mesh;
                return pick;
            }, "None", AssetIcon::mesh);
        const auto set_material = [this, handle, mesh](const std::string& chosen) {
            mutate("scene.set_renderer",
                   entity_field(handle) + ",\"enabled\":true,\"mesh\":\"" +
                       json_escape(mesh.empty() ? "builtin.triangle" : mesh) + "\",\"material\":\"" +
                       json_escape(chosen) + '"',
                   "Renderer updated");
        };
        // Built-in and imported materials, and the project's surface materials.
        asset_field(
            "Material", material, "inspector:renderer:material",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:renderer:material";
                pick.title = "Choose a material";
                pick.current = material;
                pick.kinds = {"material"};
                pick.accepts = [this](const std::string& path) { return material_facts_of(path).type == "surface"; };
                pick.what = "a surface material";
                add_registry_choices(pick, material_names, false);
                pick.actions.emplace_back("New material", [this, set_material] {
                    (void)call("assets.create_folder", "\"path\":\"materials\"", false);
                    if (const auto created = create_material("materials", false)) set_material(*created);
                });
                pick.choose = set_material;
                return pick;
            },
            "None", AssetIcon::material);
        if (material.ends_with(".relay-material")) {
            draw_shader_material_fields(material, false);
            draw_object_parameters(*renderer, material);
        }
    }

    // Values of the material's parameters for this object alone: shown as the material's until
    // changed here (or by a script during the game), and undoable like other scene edits.
    void draw_object_parameters(const JsonValue::Object& renderer, const std::string& material) {
        const auto* summary = shader_material_summary(material);
        const auto* parameters = summary ? field(*summary, "parameters") : nullptr;
        if (!parameters || !parameters->array()) return;
        std::map<std::string, std::vector<double>> own;
        if (const auto* values = field(renderer, "parameters"); values && values->object())
            for (const auto& [name, list] : *values->object())
                if (list.array()) {
                    auto& numbers = own[name];
                    for (const auto& item : *list.array())
                        if (item.number()) numbers.push_back(*item.number());
                }
        std::vector<const JsonValue::Object*> numeric;
        for (const auto& item : *parameters->array())
            if (const auto* parameter = item.object(); parameter && string_or(*parameter, "type") != "sampler2D")
                numeric.push_back(parameter);
        if (numeric.empty()) return;
        ImGui::PushID("object_parameters");
        if (!own.empty()) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const std::string title = own.empty() ? "This object only" : "This object only (" + std::to_string(own.size()) + ")";
        const bool open = ImGui::TreeNodeEx("##object", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", title.c_str());
        note_item("inspector:object:parameters");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Values for this object alone. Other objects using %s keep the material's values. "
                              "Scripts change the same values with set_material_parameter.",
                              base_name(material).c_str());
        if (open) {
            const auto entity_fields = entity_field(selection);
            for (const auto* parameter : numeric) {
                const auto name = string_or(*parameter, "name");
                const auto found = own.find(name);
                const bool overridden = found != own.end();
                // The material's values are dimmed; this object's own are in full color.
                if (!overridden) ImGui::PushStyleColor(ImGuiCol_Text, editor_color(editor_palette().text_dim));
                draw_material_parameter(
                    *parameter,
                    [&](const std::string& fields) {
                        mutate("scene.set_renderer_parameter", entity_fields + fields, "Object value set");
                    },
                    overridden ? std::function<void()>([&] {
                        mutate("scene.set_renderer_parameter",
                               entity_fields + ",\"name\":\"" + json_escape(name) + "\",\"clear\":true",
                               "Object value cleared");
                    })
                               : std::function<void()>{},
                    overridden ? &found->second : nullptr, "inspector:object:", "Back to the material's value");
                if (!overridden) ImGui::PopStyleColor();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void draw_animator_section(const JsonValue::Object& entity) {
        const auto* animator = component(entity, "animator");
        if (animator == nullptr) return;
        if (!component_header("Model animation", "animator", 0, false)) return;

        ImGui::Text("Model: %s", string_or(*animator, "model", "<unknown>").c_str());
        auto clip = static_cast<int>(number_or(*animator, "clip", 0.0));
        auto time_seconds = number_or(*animator, "time_seconds", 0.0);
        auto speed = number_or(*animator, "speed", 1.0);
        const bool playing = boolean_or(*animator, "playing", false);
        const bool loop = boolean_or(*animator, "loop", false);

        if (ImGui::Button(playing ? "Pause clip" : "Play clip")) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"playing\":" + (playing ? "false" : "true"),
                   playing ? "Animation paused" : "Animation playing");
        }
        ImGui::SameLine();
        if (ImGui::Button(loop ? "Loop: on" : "Loop: off")) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"loop\":" + (loop ? "false" : "true"),
                   "Loop updated");
        }
        ImGui::SameLine();
        // Restarting is the commonest thing to want after watching a clip once, and it is a seek
        // rather than a new kind of operation.
        if (ImGui::Button("Restart")) {
            mutate("scene.set_animation", entity_field(selection) + ",\"time_seconds\":0",
                   "Animation restarted");
        }

        const auto clips = clips_for(string_or(*animator, "model"));
        const auto clip_label = [&](const std::size_t index) {
            const auto name = index < clips.size() ? clips[index].name : std::string{};
            return name.empty() ? "Clip " + std::to_string(index) : name;
        };
        if (clips.empty()) {
            // The model's clip list has not arrived yet, so fall back to the raw index rather than
            // showing an empty list that cannot be used.
            ImGui::SetNextItemWidth(120.0F * ui_scale);
            auto& clip_draft = drafts[ImGui::GetID("Clip")];
            clip = static_cast<int>(clip_draft.begin(clip));
            ImGui::InputInt("Clip", &clip);
            clip_draft.value = clip;
            const bool clip_commit = ImGui::IsItemDeactivatedAfterEdit();
            clip_draft.finish(ImGui::IsItemActive());
            if (clip_commit && clip >= 0 && clip <= 255) {
                mutate("scene.set_animation",
                       entity_field(selection) + ",\"clip\":" + std::to_string(clip),
                       "Clip selected");
            }
        } else if (inspector_begin_combo("Clip", clip_label(static_cast<std::size_t>(clip)).c_str())) {
            for (std::size_t index = 0; index < clips.size(); ++index) {
                if (ImGui::Selectable(clip_label(index).c_str(),
                                      index == static_cast<std::size_t>(clip))) {
                    mutate("scene.set_animation",
                           entity_field(selection) + ",\"clip\":" + std::to_string(index),
                           "Clip selected");
                }
            }
            ImGui::EndCombo();
        }

        const double duration = static_cast<std::size_t>(clip) < clips.size()
                                    ? clips[static_cast<std::size_t>(clip)].duration_seconds
                                    : 0.0;
        // A slider bounded by the clip is what makes scrubbing usable; an unknown or zero-length
        // clip keeps the open-ended drag so the field never becomes unusable.
        const bool seeked = duration > 0.0
                                ? slider_scalar("Time", time_seconds, 0.0, duration, "%.2f s")
                                : drag_scalar("Time", time_seconds, 0.01F);
        if (seeked && time_seconds >= 0.0) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"time_seconds\":" + number_text(time_seconds) +
                       (duration > 0.0 ? ",\"gesture\":" + std::to_string(animation_gesture)
                                       : std::string{}),
                   "Animation seeked");
        }
        if (duration > 0.0) {
            ImGui::TextColored(editor_color(editor_palette().text_faint), "Length %.3f s", duration);
        }
        if (drag_scalar("Speed", speed, 0.01F, "%.2fx")) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"speed\":" + number_text(speed), "Speed updated");
        }
    }

    void draw_keyframes_section(const JsonValue::Object& entity) {
        const auto* animation = component(entity, "transform_animation");
        if (!animation || !component_header("Transform keyframes", "keyframes")) return;
        const auto entity_request = entity_field(selection);
        const auto playing = boolean_or(*animation, "playing", false);
        const auto loop = boolean_or(*animation, "loop", true);
        if (ImGui::Button(playing ? "Pause keys" : "Play keys"))
            mutate("scene.keyframes.playback", entity_request +
                   ",\"playing\":" + (playing ? "false" : "true"), "Key playback updated");
        ImGui::SameLine();
        if (ImGui::Button(loop ? "Loop keys: on" : "Loop keys: off"))
            mutate("scene.keyframes.playback", entity_request +
                   ",\"loop\":" + (loop ? "false" : "true"), "Key loop updated");
        auto duration = number_or(*animation, "duration_seconds", 1.0);
        auto time = number_or(*animation, "time_seconds", 0.0);
        if (slider_scalar("Key time", time, 0.0, duration, "%.2f s"))
            mutate("scene.keyframes.playback", entity_request +
                   ",\"playing\":false,\"time_seconds\":" + number_text(time),
                   "Key time updated");
        if (drag_scalar("Key duration", duration, 0.05F) && duration > 0.0)
            mutate("scene.keyframes.playback", entity_request +
                   ",\"duration_seconds\":" + number_text(duration),
                   "Key duration updated");
        if (ImGui::Button("Add key at time"))
            mutate("scene.keyframe.set", entity_request +
                   ",\"time_seconds\":" + number_text(time), "Transform key added");
        const auto* keys = field(*animation, "keys");
        if (!keys || !keys->array()) return;
        for (std::size_t index = 0; index < keys->array()->size(); ++index) {
            const auto* key = (*keys->array())[index].object();
            if (!key) continue;
            const auto key_time = number_or(*key, "time_seconds", 0.0);
            ImGui::PushID(static_cast<int>(index));
            const auto label = "Key " + number_text(key_time) + " s";
            if (ImGui::TreeNode(label.c_str())) {
                if (ImGui::Button("Use current transform")) {
                    auto fields = entity_request + ",\"time_seconds\":" + number_text(key_time);
                    if (const auto* transform = component(entity, "transform")) {
                        for (const auto& [name, prefix] : {
                                 std::pair{"position", "p"},
                                 std::pair{"rotation_degrees", "r"},
                                 std::pair{"scale", "s"}}) {
                            const auto* vector = field(*transform, name);
                            if (!vector || !vector->object()) continue;
                            for (const char axis : {'x', 'y', 'z'}) {
                                const auto wire = std::string(prefix) + axis;
                                fields += ",\"" + wire + "\":" +
                                          number_text(number_or(*vector->object(),
                                                                std::string(1, axis).c_str(), 0.0));
                            }
                        }
                    }
                    mutate("scene.keyframe.set", fields, "Transform key updated");
                }
                ImGui::SameLine();
                if (ImGui::Button("Delete key"))
                    mutate("scene.keyframe.delete", entity_request +
                           ",\"time_seconds\":" + number_text(key_time),
                           "Transform key deleted");
                for (const auto& [name, field_name, prefix] : {
                         std::tuple{"Position", "position", "p"},
                         std::tuple{"Rotation", "rotation_degrees", "r"},
                         std::tuple{"Scale", "scale", "s"}}) {
                    const auto* vector = field(*key, field_name);
                    if (!vector || !vector->object()) continue;
                    std::array<double, 3> values{
                        number_or(*vector->object(), "x", 0.0),
                        number_or(*vector->object(), "y", 0.0),
                        number_or(*vector->object(), "z", 0.0)};
                    for (std::size_t axis = 0; axis < 3U; ++axis) {
                        ImGui::PushID(static_cast<int>(axis));
                        const auto axis_label = std::string(name) + " " + "XYZ"[axis];
                        if (drag_scalar(axis_label.c_str(), values[axis], 0.05F)) {
                            const auto wire = std::string(prefix) + "xyz"[axis];
                            mutate("scene.keyframe.set", entity_request +
                                   ",\"time_seconds\":" + number_text(key_time) +
                                   ",\"" + wire + "\":" + number_text(values[axis]),
                                   "Transform key updated");
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }

    void draw_light_section(const JsonValue::Object& entity) {
        const auto* light = component(entity, "light");
        if (light == nullptr || !component_header("Light", "light")) return;

        const auto type = static_cast<unsigned>(number_or(*light, "type", 1));
        static constexpr std::array<const char*, 3> type_names{"directional", "point", "spot"};
        int type_index = static_cast<int>(std::min(type, 2U));
        inspector_field_label("Type");
        if (ImGui::Combo("##light_type", &type_index, "directional\0point\0spot\0")) {
            mutate("scene.set_light",
                   entity_field(selection) + ",\"type\":\"" +
                       type_names[static_cast<std::size_t>(type_index)] + '"',
                   "Light type changed");
        }

        auto color = editor_vector(*light, "color", {1, 1, 1});
        if (const auto mask = drag_vector3("Color", color, 0.01F, 84.0F * ui_scale)) {
            mutate("scene.set_light",
                   entity_field(selection) + vector_fields(color, {"red", "green", "blue"}, mask),
                   "Light color updated");
        }

        auto intensity = number_or(*light, "intensity", 1.0);
        if (drag_scalar("Intensity", intensity, 0.05F) && intensity >= 0.0) {
            mutate("scene.set_light",
                   entity_field(selection) + ",\"intensity\":" + number_text(intensity),
                   "Light intensity updated");
        }
        note_item("inspector:light:intensity");

        auto range = number_or(*light, "range", 0.0);
        if (drag_scalar("Range", range, 0.1F) && range >= 0.0) {
            mutate("scene.set_light", entity_field(selection) + ",\"range\":" + number_text(range),
                   "Light range updated");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(0 = infinite)");

        if (type_index == 2) {
            // Cone angles are radians on the wire and stay clamped to the schema's quarter turn.
            auto inner = number_or(*light, "inner_cone", 0.0);
            auto outer = number_or(*light, "outer_cone", 0.7853981633974483);
            const bool inner_done = drag_scalar("Inner cone", inner, 0.005F);
            const bool outer_done = drag_scalar("Outer cone", outer, 0.005F);
            if (inner_done || outer_done) {
                inner = std::clamp(inner, 0.0, 1.5707963267948966);
                outer = std::clamp(outer, 0.0001, 1.5707963267948966);
                if (inner > outer) inner = outer;
                mutate("scene.set_light",
                       entity_field(selection) + ",\"inner_cone\":" + number_text(inner) +
                           ",\"outer_cone\":" + number_text(outer),
                       "Light cone updated");
            }
        }

        if (type_index != 0) {
            auto attenuation = editor_vector(*light, "attenuation", {0, 0, 1});
            if (const auto mask =
                    drag_vector3("Attenuation", attenuation, 0.01F, 84.0F * ui_scale)) {
                for (auto& value : attenuation)
                    value = std::max(value, 0.0);
                mutate("scene.set_light",
                       entity_field(selection) +
                           vector_fields(attenuation, {"constant", "linear", "quadratic"}, mask),
                       "Light attenuation updated");
            }
        }
    }

    void draw_collider_section(const JsonValue::Object& entity) {
        const auto* collider = component(entity, "collider");
        if (!collider || !component_header("Collider", "collider")) return;
        const auto entity_request = entity_field(selection);
        constexpr std::array<const char*, 5> shape_names{"Box", "Sphere", "Capsule", "Convex hull",
                                                         "Triangle mesh"};
        constexpr std::array<const char*, 5> shape_values{"box", "sphere", "capsule", "convex",
                                                          "mesh"};
        auto shape_index = std::clamp(static_cast<int>(number_or(*collider, "type", 0)), 0, 4);
        if (inspector_begin_combo("Shape", shape_names[static_cast<std::size_t>(shape_index)])) {
            for (int index = 0; index < 5; ++index) {
                if (ImGui::Selectable(shape_names[static_cast<std::size_t>(index)],
                                      index == shape_index)) {
                    shape_index = index;
                    mutate("scene.set_collider", entity_request + ",\"type\":\"" +
                           shape_values[static_cast<std::size_t>(index)] + "\"", "Collider shape updated");
                }
            }
            ImGui::EndCombo();
        }
        auto enabled = boolean_or(*collider, "enabled", true);
        if (inspector_checkbox("Enabled##collider", &enabled))
            mutate("scene.set_collider", entity_request +
                   ",\"enabled\":" + (enabled ? "true" : "false"), "Collider updated");
        auto center = editor_vector(*collider, "center", {0, 0, 0});
        if (const auto mask = drag_vector3("Center", center, 0.05F, 84.0F * ui_scale))
            mutate("scene.set_collider", entity_request +
                   vector_fields(center, {"center_x", "center_y", "center_z"}, mask),
                   "Collider center updated");
        if (shape_index == 0) {
            auto extents = editor_vector(*collider, "half_extents", {0.5, 0.5, 0.5});
            if (const auto mask = drag_vector3("Half extents", extents, 0.05F, 84.0F * ui_scale)) {
                for (auto& value : extents) value = std::max(value, 0.01);
                mutate("scene.set_collider", entity_request +
                       vector_fields(extents, {"half_x", "half_y", "half_z"}, mask),
                       "Collider size updated");
            }
        } else if (shape_index >= 3) {
            const auto mesh = string_or(*collider, "mesh");
            const auto* renderer = component(entity, "mesh_renderer");
            const auto renderer_mesh = renderer ? string_or(*renderer, "mesh") : std::string{};
            const auto set_mesh = [this, entity_request](const std::string& chosen) {
                mutate("scene.set_collider", entity_request + ",\"mesh\":\"" + json_escape(chosen) + '"',
                       "Collider mesh updated");
            };
            asset_field(
                "Collision mesh", mesh, "inspector:collider:mesh",
                [=, this] {
                    AssetPick pick;
                    pick.key = "inspector:collider:mesh";
                    pick.title = "Choose a collision mesh";
                    pick.current = mesh;
                    pick.choices.push_back({"", "Use the renderer's mesh", AssetIcon::mesh, "", "", std::nullopt});
                    add_registry_choices(pick, mesh_names, true);
                    pick.choose = set_mesh;
                    return pick;
                }, "Renderer mesh", AssetIcon::mesh);
            const auto& source = mesh.empty() ? renderer_mesh : mesh;
            if (source.empty())
                ImGui::TextDisabled("Add a mesh renderer or choose a collision mesh.");
            else if (std::find(mesh_names.begin(), mesh_names.end(), source) == mesh_names.end())
                ImGui::TextDisabled("Mesh %s is not loaded; the collider is inactive.",
                                    source.c_str());
            else if (shape_index == 4)
                ImGui::TextDisabled("Dynamic bodies use this mesh's convex hull.");
        } else {
            auto radius = number_or(*collider, "radius", 0.5);
            if (drag_scalar("Radius", radius, 0.05F))
                mutate("scene.set_collider", entity_request + ",\"radius\":" +
                       number_text(std::max(radius, 0.01)), "Collider radius updated");
            if (shape_index == 2) {
                auto half_height = number_or(*collider, "half_height", 0.5);
                if (drag_scalar("Cylinder half height", half_height, 0.05F))
                    mutate("scene.set_collider", entity_request + ",\"half_height\":" +
                           number_text(std::max(half_height, 0.01)), "Collider height updated");
            }
        }
        auto layer = static_cast<std::uint32_t>(number_or(*collider, "layer", 1));
        inspector_field_label("Layer bits");
        if (ImGui::InputScalar("##layer_bits", ImGuiDataType_U32, &layer) && layer != 0U)
            mutate("scene.set_collider", entity_request + ",\"layer\":" + std::to_string(layer),
                   "Collider layer updated");
        auto collision_mask = static_cast<std::uint32_t>(number_or(*collider, "mask", 0xffffffffU));
        inspector_field_label("Mask bits");
        if (ImGui::InputScalar("##mask_bits", ImGuiDataType_U32, &collision_mask))
            mutate("scene.set_collider", entity_request + ",\"mask\":" +
                   std::to_string(collision_mask), "Collider mask updated");
    }

    void draw_physics_body_section(const JsonValue::Object& entity) {
        const auto* body = component(entity, "physics_body");
        if (!body || !component_header("Physics body", "physics_body")) return;
        const auto body_request = entity_field(selection);
        const auto type = static_cast<int>(number_or(*body, "type", 1));
        int selected_type = std::clamp(type, 0, 1);
        inspector_field_label("Body type");
        if (ImGui::Combo("##body_type", &selected_type, "Static\0Dynamic\0"))
            mutate("scene.set_physics_body", body_request +
                   (selected_type == 0 ? ",\"type\":\"static\"" : ",\"type\":\"dynamic\""),
                   "Physics body type updated");
        if (selected_type == 1) {
            auto mass = number_or(*body, "mass", 1.0);
            if (drag_scalar("Mass", mass, 0.05F) && mass > 0.0)
                mutate("scene.set_physics_body", body_request + ",\"mass\":" + number_text(mass),
                       "Physics body mass updated");
            auto gravity = number_or(*body, "gravity_scale", 1.0);
            if (drag_scalar("Gravity scale", gravity, 0.05F) && gravity >= 0.0)
                mutate("scene.set_physics_body", body_request + ",\"gravity_scale\":" +
                       number_text(gravity), "Physics body gravity updated");
            bool locked = boolean_or(*body, "lock_rotation", false);
            if (inspector_checkbox("Lock rotation", &locked))
                mutate("scene.set_physics_body",
                       body_request + ",\"lock_rotation\":" + (locked ? "true" : "false"),
                       locked ? "Rotation locked" : "Rotation unlocked");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Collisions move the body but never turn it, as for characters");
        }
        auto restitution = number_or(*body, "restitution", 0.0);
        if (drag_scalar("Bounciness", restitution, 0.01F) &&
            restitution >= 0.0 && restitution <= 1.0)
            mutate("scene.set_physics_body", body_request + ",\"restitution\":" +
                   number_text(restitution), "Physics body bounciness updated");
        auto friction = number_or(*body, "friction", 0.2);
        if (drag_scalar("Friction", friction, 0.01F) &&
            friction >= 0.0 && friction <= 10.0)
            mutate("scene.set_physics_body", body_request + ",\"friction\":" +
                   number_text(friction), "Physics body friction updated");
        if (selected_type == 1) {
            auto linear = number_or(*body, "linear_damping", 0.05);
            if (drag_scalar("Linear damping", linear, 0.01F) &&
                linear >= 0.0 && linear <= 100.0)
                mutate("scene.set_physics_body", body_request + ",\"linear_damping\":" +
                       number_text(linear), "Physics body linear damping updated");
            auto angular = number_or(*body, "angular_damping", 0.05);
            if (drag_scalar("Angular damping", angular, 0.01F) &&
                angular >= 0.0 && angular <= 100.0)
                mutate("scene.set_physics_body", body_request + ",\"angular_damping\":" +
                       number_text(angular), "Physics body angular damping updated");
        }
    }

    void draw_audio_source_section(const JsonValue::Object& entity) {
        const auto* source = component(entity, "audio_source");
        if (!source || !component_header("Audio source", "audio_source")) return;
        const auto& palette = editor_palette();
        const auto request_fields = entity_field(selection);
        const auto set = [&](const std::string& fields, const char* label) {
            mutate("scene.set_audio_source", request_fields + fields, label);
        };
        const auto clip = string_or(*source, "clip");
        const auto pick_clip = [this, request_fields](const std::string& chosen) {
            mutate("scene.set_audio_source", request_fields + ",\"clip\":\"" + json_escape(chosen) + '"',
                   chosen.empty() ? "Clip cleared" : "Clip set");
        };
        // Sound files dragged from the Assets panel drop onto the field too.
        asset_field(
            "Clip", clip, "inspector:audio:clip",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:audio:clip";
                pick.title = "Choose a sound";
                pick.current = clip;
                pick.kinds = {"audio"};
                pick.choices.push_back({"", "None", AssetIcon::audio, "", "", std::nullopt});
                pick.choose = pick_clip;
                return pick;
            },
            "None", AssetIcon::audio);

        const auto* voice = audio_voice(selection);
        if (!clip.empty()) {
            const auto* info = audio_clip_summary(clip);
            if (info) {
                draw_waveform(*info, voice ? number_or(*voice, "position_seconds", -1.0) : -1.0);
                ImGui::TextColored(editor_color(palette.text_faint), "%.2f s  ·  %s  ·  %d Hz",
                                   number_or(*info, "duration_seconds", 0.0),
                                   number_or(*info, "channels", 1.0) > 1.0 ? "stereo" : "mono",
                                   static_cast<int>(number_or(*info, "sample_rate", 0.0)));
            } else {
                ImGui::TextColored(editor_color(palette.warning), "This file cannot be played");
            }
        }
        const bool playing = voice != nullptr;
        ImGui::BeginDisabled(clip.empty());
        if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(80.0F * ui_scale, 0.0F))) {
            if (playing)
                (void)call("audio.stop", request_fields);
            else if (call("audio.play", request_fields))
                set_status("Previewing " + clip, false);
            refresh_audio_status();
        }
        ImGui::EndDisabled();
        note_item("inspector:audio:play");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(clip.empty() ? "Choose a clip first"
                                           : "In the editor the source previews flat; Run Game "
                                             "plays it from its position");

        const auto bus = string_or(*source, "bus", "Master");
        const auto buses = audio_bus_names();
        const bool known_bus = std::find(buses.begin(), buses.end(), bus) != buses.end();
        if (inspector_begin_combo("Bus", bus.c_str())) {
            for (const auto& name : buses) {
                if (ImGui::Selectable(name.c_str(), name == bus))
                    set(",\"bus\":\"" + json_escape(name) + '"', "Bus changed");
                note_item("inspector:audio:bus:" + name);
            }
            ImGui::EndCombo();
        }
        note_item("inspector:audio:bus");
        if (!known_bus && !buses.empty())
            ImGui::TextColored(editor_color(palette.warning),
                               "No bus is called %s; this source plays into Master", bus.c_str());

        const auto scalar = [&](const char* label, const char* wire, double value, float speed,
                                const char* format, double minimum, double maximum) {
            if (drag_scalar(label, value, speed, format))
                set(",\"" + std::string(wire) + "\":" + number_text(std::clamp(value, minimum, maximum)),
                    "Audio source updated");
        };
        scalar("Volume", "volume_db", number_or(*source, "volume_db", 0.0), 0.1F, "%.1f dB",
               minimum_audio_volume_db, maximum_audio_volume_db);
        scalar("Pitch", "pitch", number_or(*source, "pitch", 1.0), 0.005F, "%.3fx", 0.1, 4.0);
        const auto flag = [&](const char* label, const char* wire, const char* tooltip) {
            bool value = boolean_or(*source, wire, false);
            inspector_field_label(label);
            if (editor_checkbox((std::string("##") + wire).c_str(), &value))
                set(",\"" + std::string(wire) + "\":" + (value ? "true" : "false"),
                    "Audio source updated");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        };
        flag("Loop", "loop", "Start again from the beginning when the clip ends");
        flag("Play on start", "play_on_start",
             "Play when Run Game starts, or when this node is spawned during the game");
        flag("Spatial", "spatial",
             "Positioned in the world: quieter with distance and panned around the listener. Off "
             "plays flat, like music or interface sounds.");
        if (boolean_or(*source, "spatial", true)) {
            const auto minimum = number_or(*source, "min_distance", 1.0);
            const auto maximum = number_or(*source, "max_distance", 50.0);
            scalar("Min distance", "min_distance", minimum, 0.05F, "%.2f m", 0.01, maximum);
            scalar("Max distance", "max_distance", maximum, 0.25F, "%.1f m", minimum, 1e6);
            static constexpr std::array<const char*, 3> rolloffs{"inverse", "inverse_square", "linear"};
            static constexpr std::array<const char*, 3> rolloff_labels{"Inverse (natural)",
                                                                       "Inverse square (steep)",
                                                                       "Linear"};
            const auto rolloff = string_or(*source, "rolloff", "inverse");
            std::size_t current = 0;
            for (std::size_t index = 0; index < rolloffs.size(); ++index)
                if (rolloff == rolloffs[index]) current = index;
            if (inspector_begin_combo("Rolloff", rolloff_labels[current])) {
                for (std::size_t index = 0; index < rolloffs.size(); ++index)
                    if (ImGui::Selectable(rolloff_labels[index], index == current))
                        set(std::string(",\"rolloff\":\"") + rolloffs[index] + '"', "Rolloff changed");
                ImGui::EndCombo();
            }
            scalar("Doppler", "doppler", number_or(*source, "doppler", 1.0), 0.01F, "%.2f", 0.0, 5.0);
            flag("Occlusion", "occlusion",
                 "Quieter and duller when colliders stand between it and the listener");
            scalar("Reverb send", "reverb_send", number_or(*source, "reverb_send", 1.0), 0.01F,
                   "%.2f", 0.0, 1.0);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much reaches the reverb of the zone the listener is in");
        } else {
            scalar("Pan", "pan", number_or(*source, "pan", 0.0), 0.01F, "%.2f", -1.0, 1.0);
        }
    }

    void draw_music_player_section(const JsonValue::Object& entity) {
        const auto* player = component(entity, "music_player");
        if (!player || !component_header("Music player", "music_player")) return;
        const auto& palette = editor_palette();
        const auto request_fields = entity_field(selection);
        const auto set = [&](const std::string& fields, const char* label) {
            mutate("scene.set_music_player", request_fields + fields, label);
        };
        std::vector<std::string> tracks;
        if (const auto* list = field(*player, "tracks"); list && list->array())
            for (const auto& item : *list->array())
                if (item.string()) tracks.push_back(*item.string());
        const auto set_tracks = [this, request_fields](const std::vector<std::string>& changed, const char* label) {
            std::string fields = ",\"tracks\":[";
            for (std::size_t index = 0; index < changed.size(); ++index)
                fields += std::string(index ? "," : "") + '"' + json_escape(changed[index]) + '"';
            mutate("scene.set_music_player", request_fields + fields + "]", label);
        };
        // What plays now, with controls, while the game runs.
        int playing = -1;
        if (const auto* status = audio_status.object())
            if (const auto* music = field(*status, "music"); music && music->array())
                for (const auto& item : *music->array())
                    if (const auto* entry = item.object(); entry && string_or(*entry, "entity") == selection)
                        playing = static_cast<int>(number_or(*entry, "track", -1.0));
        const bool game = runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game";
        ImGui::TextColored(editor_color(palette.text_dim), "Playlist");
        std::optional<std::vector<std::string>> changed;
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            ImGui::PushID(static_cast<int>(index));
            const bool current = static_cast<int>(index) == playing;
            if (current) ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.success));
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s%zu", current ? "> " : "  ", index + 1U);
            ImGui::SameLine();
            {
                const float icon = ImGui::GetTextLineHeight();
                const auto at = ImGui::GetCursorScreenPos();
                draw_asset_type_icon(ImGui::GetWindowDrawList(),
                                     ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y), icon, AssetIcon::audio, 0.8F);
                ImGui::Dummy(ImVec2(icon, ImGui::GetFrameHeight()));
                ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
            }
            ImGui::TextUnformatted(asset_display_name(tracks[index]).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tracks[index].c_str());
            if (current) ImGui::PopStyleColor();
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - 88.0F * ui_scale));
            if (game && ImGui::SmallButton("Play"))
                (void)call("audio.music", request_fields + ",\"action\":\"play\",\"track\":" + std::to_string(index));
            if (game) ImGui::SameLine();
            ImGui::BeginDisabled(index == 0U);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                changed = tracks;
                std::swap((*changed)[index], (*changed)[index - 1U]);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) {
                changed = tracks;
                changed->erase(changed->begin() + static_cast<std::ptrdiff_t>(index));
            }
            note_item("inspector:music:remove:" + std::to_string(index));
            ImGui::PopID();
        }
        if (changed) set_tracks(*changed, "Playlist changed");
        if (tracks.size() < maximum_music_tracks) {
            const auto add_track = [tracks, set_tracks](const std::string& file) {
                auto added = tracks;
                added.push_back(file);
                set_tracks(added, "Track added");
            };
            asset_add_button(
                "+ Add track...", "inspector:music:add",
                [=, this] {
                    AssetPick pick;
                    pick.key = "inspector:music:add";
                    pick.title = "Add a track";
                    pick.kinds = {"audio"};
                    pick.choose = add_track;
                    return pick;
                });
        }
        if (game) {
            if (ImGui::Button("Next")) (void)call("audio.music", request_fields + ",\"action\":\"next\"");
            note_item("inspector:music:next");
            ImGui::SameLine();
            if (ImGui::Button("Stop")) (void)call("audio.music", request_fields + ",\"action\":\"stop\"");
        } else {
            ImGui::TextColored(editor_color(palette.text_faint), "Plays during Run Game");
        }
        const auto bus = string_or(*player, "bus", "Music");
        if (inspector_begin_combo("Bus", bus.c_str())) {
            for (const auto& name : audio_bus_names())
                if (ImGui::Selectable(name.c_str(), name == bus))
                    set(",\"bus\":\"" + json_escape(name) + '"', "Bus changed");
            ImGui::EndCombo();
        }
        const auto scalar = [&](const char* label, const char* wire, double value, float speed,
                                const char* format, double minimum, double maximum) {
            if (drag_scalar(label, value, speed, format))
                set(",\"" + std::string(wire) + "\":" + number_text(std::clamp(value, minimum, maximum)),
                    "Music player updated");
        };
        scalar("Volume", "volume_db", number_or(*player, "volume_db", 0.0), 0.1F, "%.1f dB",
               minimum_audio_volume_db, maximum_audio_volume_db);
        scalar("Crossfade", "crossfade_seconds", number_or(*player, "crossfade_seconds", 2.0), 0.02F,
               "%.2f s", 0.0, 30.0);
        const auto flag = [&](const char* label, const char* wire, const bool fallback, const char* tip) {
            bool value = boolean_or(*player, wire, fallback);
            inspector_field_label(label);
            if (editor_checkbox((std::string("##") + wire).c_str(), &value))
                set(",\"" + std::string(wire) + "\":" + (value ? "true" : "false"), "Music player updated");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        flag("Play on start", "play_on_start", true, "Start the first track when Run Game starts");
        flag("Shuffle", "shuffle", false, "Play the tracks in a random order, a new one each time round");
        flag("Loop playlist", "loop_playlist", true, "Start again after the last track");
        ImGui::SeparatorText("Timing");
        scalar("BPM", "bpm", number_or(*player, "bpm", 120.0), 0.1F, "%.1f", 20.0, 400.0);
        auto beats = number_or(*player, "beats_per_bar", 4.0);
        if (drag_scalar("Beats per bar", beats, 0.05F, "%.0f"))
            set(",\"beats_per_bar\":" + std::to_string(static_cast<int>(std::clamp(std::round(beats), 1.0, 16.0))),
                "Music player updated");
        scalar("First beat", "first_beat_seconds", number_or(*player, "first_beat_seconds", 0.0), 0.005F,
               "%.3f s", 0.0, 60.0);
        static constexpr std::array<std::pair<const char*, const char*>, 4> syncs{{
            {"immediate", "Immediately"}, {"beat", "Next beat"}, {"bar", "Next bar"},
            {"track_end", "End of track"}}};
        const auto sync = string_or(*player, "sync", "bar");
        const char* sync_label = "Next bar";
        for (const auto& [id, label] : syncs)
            if (sync == id) sync_label = label;
        if (inspector_begin_combo("Changes land", sync_label)) {
            for (const auto& [id, label] : syncs)
                if (ImGui::Selectable(label, sync == id))
                    set(std::string(",\"sync\":\"") + id + '"', "Music sync changed");
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When a change asked for by a script or Next waits to happen");
    }

    void draw_sky_section(const JsonValue::Object& entity) {
        const auto* sky = component(entity, "sky");
        if (!sky || !component_header("Sky", "sky")) return;
        const auto& palette = editor_palette();
        const auto request_fields = entity_field(selection);
        const auto set = [&](const std::string& fields, const char* label) {
            mutate("scene.set_sky", request_fields + fields, label);
        };
        const auto color_field = [](const char* wire, const std::array<double, 3>& color) {
            return ",\"" + std::string(wire) + "\":[" + number_text(color[0]) + ',' +
                   number_text(color[1]) + ',' + number_text(color[2]) + ']';
        };
        const auto scalar = [&](const char* label, const char* wire, double value, float speed,
                                const char* format, double minimum, double maximum) {
            if (drag_scalar(label, value, speed, format))
                set(",\"" + std::string(wire) + "\":" + number_text(std::clamp(value, minimum, maximum)),
                    "Sky updated");
        };
        const auto dim_text = [&](const char* text) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            ImGui::TextWrapped("%s", text);
            ImGui::PopStyleColor();
        };
        std::size_t skies = 0;
        bool used = false;
        for (const auto* other : entities)
            if (component(*other, "sky")) {
                if (skies++ == 0U) used = string_or(*other, "entity") == selection;
            }
        if (skies > 1U)
            ImGui::TextColored(editor_color(palette.warning),
                               used ? "%zu nodes have a sky; this one is used."
                                    : "%zu nodes have a sky; only the first is used, not this one.",
                               skies);

        ImGui::SeparatorText("Skybox");
        const auto material = string_or(*sky, "material");
        const auto pick_material = [this, request_fields](const std::string& chosen) {
            mutate("scene.set_sky", request_fields + ",\"material\":\"" + json_escape(chosen) + '"',
                   chosen.empty() ? "Sky gradient chosen" : "Sky material chosen");
        };
        // A color gradient, or a sky material showing a panorama image. Sky materials dragged
        // from the Assets panel drop onto the field.
        const auto handle = selection;
        asset_field(
            "Skybox", material, "inspector:sky:material",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:sky:material";
                pick.title = "Choose a sky material";
                pick.current = material;
                pick.kinds = {"material"};
                pick.accepts = [this](const std::string& path) { return material_facts_of(path).type == "sky"; };
                pick.what = "a sky material";
                pick.choices.push_back({"", "Gradient", AssetIcon::sky_material, "", "", std::nullopt});
                pick.actions.emplace_back("New sky material", [this, handle] { create_sky_material({}, handle); });
                pick.choose = pick_material;
                return pick;
            },
            "Gradient", AssetIcon::sky_material);
        if (material.empty()) {
            const Sky defaults;
            auto horizon = editor_vector(*sky, "horizon_color", {defaults.horizon_color.x,
                                                                 defaults.horizon_color.y,
                                                                 defaults.horizon_color.z});
            auto zenith = editor_vector(*sky, "zenith_color", {defaults.zenith_color.x,
                                                               defaults.zenith_color.y,
                                                               defaults.zenith_color.z});
            if (color_edit("Horizon", horizon))
                set(color_field("horizon_color", horizon), "Sky horizon color changed");
            note_item("inspector:sky:horizon");
            if (color_edit("Zenith", zenith))
                set(color_field("zenith_color", zenith), "Sky zenith color changed");
            note_item("inspector:sky:zenith");
            draw_color_ramp(horizon, zenith,
                            "From the horizon (and everything below it) to straight up");
        } else {
            draw_sky_material_fields(material);
        }
        scalar("Intensity", "intensity", number_or(*sky, "intensity", Sky{}.intensity), 0.01F, "%.2f",
               0.0, 100.0);
        note_item("inspector:sky:intensity");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How bright the visible sky is");
        scalar("Ambient light", "ambient_intensity", number_or(*sky, "ambient_intensity", 0.25),
               0.005F, "%.2f", 0.0, 10.0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How strongly the sky lights the scene. At 1 surfaces receive as much "
                              "light as the visible sky gives; lower leaves the sun in charge.");

        ImGui::SeparatorText("Sun");
        const auto* light = component(entity, "light");
        if (light && number_or(*light, "type", 1.0) == 0.0) {
            dim_text("The directional light below is the sun, drawn in the sky where its light "
                     "comes from. Rotate this node to move it.");
        } else if (light) {
            ImGui::TextColored(editor_color(palette.warning),
                               "This node's light is not directional, so it is not a sun.");
            if (ImGui::Button("Make it the sun"))
                mutate("scene.set_light", request_fields + ",\"type\":\"directional\"",
                       "Light turned into the sun");
            note_item("inspector:sky:make_sun");
        } else {
            dim_text("No sun. Add one to light the scene from the sky's direction.");
            if (ImGui::Button("Add sun") && mutate("component.add", request_fields + ",\"component\":\"light\"",
                                                   "Sun added"))
                (void)call("scene.set_light",
                           request_fields + ",\"type\":\"directional\",\"intensity\":" +
                               number_text(default_sun_intensity) +
                               ",\"red\":" + number_text(default_sun_color.x) +
                               ",\"green\":" + number_text(default_sun_color.y) +
                               ",\"blue\":" + number_text(default_sun_color.z));
            note_item("inspector:sky:add_sun");
        }

        ImGui::SeparatorText("Fog");
        bool fog = boolean_or(*sky, "fog", true);
        inspector_field_label("Fog");
        if (editor_checkbox("##fog", &fog))
            set(std::string(",\"fog\":") + (fog ? "true" : "false"), fog ? "Fog on" : "Fog off");
        note_item("inspector:sky:fog");
        if (fog) {
            const auto start = number_or(*sky, "fog_start", 30.0);
            const auto end = number_or(*sky, "fog_end", 250.0);
            scalar("Start", "fog_start", start, 0.1F, "%.1f m", 0.0, end - 0.01);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distance from the camera where fog begins");
            scalar("End", "fog_end", end, 0.25F, "%.1f m", start + 0.01, 1e6);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Distance from the camera where everything is fog");
            auto start_color = editor_vector(*sky, "fog_start_color", {0.62, 0.74, 0.88});
            auto end_color = editor_vector(*sky, "fog_end_color", {0.62, 0.74, 0.88});
            if (color_edit("Start color", start_color))
                set(color_field("fog_start_color", start_color), "Fog color changed");
            note_item("inspector:sky:fog_start_color");
            if (color_edit("End color", end_color))
                set(color_field("fog_end_color", end_color), "Fog color changed");
            note_item("inspector:sky:fog_end_color");
            draw_color_ramp(start_color, end_color, "The fog's color from its start to its end");
        }
    }

    // The sky material's own settings, saved to its file as they change. Not undoable: they
    // belong to the file, which other skies and scenes may share.
    void draw_sky_material_fields(const std::string& path) {
        const auto& palette = editor_palette();
        const auto* material = sky_material_summary(path);
        if (!material) {
            ImGui::TextColored(editor_color(palette.warning),
                               "%s cannot be read; the gradient shows instead", path.c_str());
            return;
        }
        const auto save = [this, path](const std::string& fields) {
            // File edits are not undoable scene changes, so drags leave no gesture behind.
            pending_gesture = 0U;
            if (call("assets.set_sky_material", "\"path\":\"" + json_escape(path) + '"' + fields))
                sky_material_info.erase(path);
        };
        const auto panorama = string_or(*material, "panorama");
        const auto set_panorama = [save](const std::string& file) { save(",\"panorama\":\"" + json_escape(file) + '"'); };
        // An equirectangular (2:1) panorama, such as an exported photo sphere.
        asset_field(
            "Panorama", panorama, "inspector:sky:panorama",
            [=, this] {
                AssetPick pick;
                pick.key = "inspector:sky:panorama";
                pick.title = "Choose a panorama image (2:1)";
                pick.current = panorama;
                pick.kinds = {"image"};
                pick.accepts = [](const std::string& file) { return panorama_file(file); };
                pick.what = "a PNG or JPEG image";
                pick.choose = set_panorama;
                return pick;
            },
            "None", AssetIcon::image);
        if (const auto* image = field(*material, "image"); image && image->object()) {
            const auto width = number_or(*image->object(), "width", 0.0);
            const auto height = number_or(*image->object(), "height", 0.0);
            ImGui::TextColored(editor_color(palette.text_faint), "%d x %d", static_cast<int>(width),
                               static_cast<int>(height));
            if (height > 0.0 && std::abs(width / height - 2.0) > 0.02) {
                ImGui::SameLine();
                ImGui::TextColored(editor_color(palette.warning), "(not 2:1, so it will look stretched)");
            }
        } else if (!panorama.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.warning));
            ImGui::TextWrapped("%s; the gradient shows instead",
                               string_or(*material, "error", "the image cannot be shown").c_str());
            ImGui::PopStyleColor();
        }
        auto tint = editor_vector(*material, "tint", {1, 1, 1});
        if (const auto* list = field(*material, "tint"); list && list->array() && list->array()->size() == 3U)
            for (std::size_t channel = 0; channel < 3U; ++channel)
                if (const auto* value = (*list->array())[channel].number()) tint[channel] = *value;
        if (color_edit("Tint", tint))
            save(",\"tint\":[" + number_text(tint[0]) + ',' + number_text(tint[1]) + ',' +
                 number_text(tint[2]) + ']');
        note_item("inspector:sky:tint");
        auto intensity = number_or(*material, "intensity", 1.0);
        if (drag_scalar("Brightness", intensity, 0.01F, "%.2f"))
            save(",\"intensity\":" + number_text(std::clamp(intensity, 0.0, 100.0)));
        auto rotation = number_or(*material, "rotation_degrees", 0.0);
        if (drag_scalar("Rotation", rotation, 0.5F, "%.1f deg"))
            save(",\"rotation_degrees\":" + number_text(std::clamp(rotation, -360.0, 360.0)));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turns the panorama about the vertical axis");
    }

    void draw_reverb_zone_section(const JsonValue::Object& entity) {
        const auto* zone = component(entity, "reverb_zone");
        if (!zone || !component_header("Reverb zone", "reverb_zone")) return;
        const auto& palette = editor_palette();
        const auto request_fields = entity_field(selection);
        const auto set = [&](const std::string& fields, const char* label) {
            mutate("scene.set_reverb_zone", request_fields + fields, label);
        };
        const auto shape = string_or(*zone, "shape", "box");
        if (inspector_begin_combo("Shape", shape == "sphere" ? "Sphere" : "Box")) {
            if (ImGui::Selectable("Box", shape == "box")) set(",\"shape\":\"box\"", "Zone shape changed");
            if (ImGui::Selectable("Sphere", shape == "sphere"))
                set(",\"shape\":\"sphere\"", "Zone shape changed");
            ImGui::EndCombo();
        }
        note_item("inspector:reverb:shape");
        const auto scalar = [&](const char* label, const char* wire, double value, float speed,
                                const char* format, double minimum, double maximum) {
            if (drag_scalar(label, value, speed, format))
                set(",\"" + std::string(wire) + "\":" + number_text(std::clamp(value, minimum, maximum)),
                    "Reverb zone updated");
        };
        if (shape == "sphere") {
            scalar("Radius", "radius", number_or(*zone, "radius", 5.0), 0.05F, "%.2f m", 0.01, 1e5);
        } else {
            auto half = editor_vector(*zone, "half_extents", {5, 3, 5});
            if (const auto mask = drag_vector3("Half size", half, 0.05F, 108.0F * ui_scale)) {
                for (auto& value : half) value = std::clamp(value, 0.01, 1e5);
                set(vector_fields(half, {"half_x", "half_y", "half_z"}, mask), "Reverb zone resized");
            }
        }
        scalar("Fade", "fade", number_or(*zone, "fade", 2.0), 0.05F, "%.2f m", 0.0, 1e4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How far outside the shape the reverb fades away");
        static constexpr std::array<std::pair<const char*, const char*>, 9> presets{{
            {"room", "Room"}, {"small_room", "Small room"}, {"bathroom", "Bathroom"},
            {"hall", "Hall"}, {"cathedral", "Cathedral"}, {"cave", "Cave"}, {"arena", "Arena"},
            {"forest", "Forest"}, {"custom", "Custom"}}};
        const auto preset = string_or(*zone, "preset", "room");
        const char* preset_label = "Custom";
        for (const auto& [id, label] : presets)
            if (preset == id) preset_label = label;
        if (inspector_begin_combo("Preset", preset_label)) {
            for (const auto& [id, label] : presets) {
                if (ImGui::Selectable(label, preset == id))
                    set(std::string(",\"preset\":\"") + id + '"', "Reverb preset chosen");
                note_item(std::string("inspector:reverb:preset:") + id);
            }
            ImGui::EndCombo();
        }
        note_item("inspector:reverb:preset");
        scalar("Room size", "room_size", number_or(*zone, "room_size", 0.5), 0.005F, "%.2f", 0.0, 1.0);
        scalar("Damping", "damping", number_or(*zone, "damping", 0.5), 0.005F, "%.2f", 0.0, 1.0);
        scalar("Level", "wet_db", number_or(*zone, "wet_db", -8.0), 0.1F, "%.1f dB",
               minimum_audio_volume_db, 6.0);
        scalar("Pre-delay", "pre_delay_ms", number_or(*zone, "pre_delay_ms", 8.0), 0.5F, "%.0f ms", 0.0, 250.0);
        // While the game runs, how much of this zone the listener hears.
        if (const auto* status = audio_status.object())
            if (const auto* environment = field(*status, "environment"); environment && environment->object())
                if (const auto* zones = field(*environment->object(), "zones"); zones && zones->array()) {
                    double weight = 0.0;
                    for (const auto& item : *zones->array())
                        if (const auto* entry = item.object(); entry && string_or(*entry, "entity") == selection)
                            weight = number_or(*entry, "weight", 0.0);
                    if (boolean_or(*status, "game", false))
                        ImGui::TextColored(editor_color(weight > 0.0 ? palette.success : palette.text_faint),
                                           weight > 0.0 ? "The listener hears %.0f%% of this zone"
                                                        : "The listener is outside this zone",
                                           weight * 100.0);
                }
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("Spatial sounds take on this reverb while the listener is inside the "
                           "shape, fading out over the fade distance around it.");
        ImGui::PopStyleColor();
    }

    // Min/max peak pairs from audio.clip, with a playhead while the source plays.
    void draw_waveform(const JsonValue::Object& info, const double position_seconds) {
        const auto* peaks = field(info, "peaks");
        if (!peaks || !peaks->array() || peaks->array()->size() < 2U) return;
        const auto& values = *peaks->array();
        const auto& palette = editor_palette();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = 36.0F * ui_scale;
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, height));
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), palette.input,
                            3.0F * ui_scale);
        const auto buckets = values.size() / 2U;
        const float middle = origin.y + height * 0.5F;
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const auto low = values[bucket * 2U].number() ? *values[bucket * 2U].number() : 0.0;
            const auto high = values[bucket * 2U + 1U].number() ? *values[bucket * 2U + 1U].number() : 0.0;
            const float x = origin.x + (static_cast<float>(bucket) + 0.5F) * width / static_cast<float>(buckets);
            draw->AddLine(ImVec2(x, middle - static_cast<float>(high) * height * 0.48F),
                          ImVec2(x, middle - static_cast<float>(low) * height * 0.48F + 1.0F),
                          palette.accent, std::max(1.0F, width / static_cast<float>(buckets) - 1.0F));
        }
        const auto duration = number_or(info, "duration_seconds", 0.0);
        if (position_seconds >= 0.0 && duration > 0.0) {
            const float x = origin.x + static_cast<float>(std::fmod(position_seconds, duration) / duration) * width;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), palette.text, 1.5F * ui_scale);
        }
    }

    void draw_audio_listener_section() {
        if (!component_header("Audio listener", "audio_listener")) return;
        const auto& palette = editor_palette();
        std::size_t listeners = 0;
        for (const auto* other : entities)
            if (component(*other, "audio_listener")) ++listeners;
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("During Run Game, spatial sounds are heard from this node's position and "
                           "facing. Put it on the player's camera.");
        ImGui::PopStyleColor();
        if (listeners > 1U)
            ImGui::TextColored(editor_color(palette.warning),
                               "%zu nodes have listeners; the first in the hierarchy is used.",
                               listeners);
    }

    void draw_joint_section(const JsonValue::Object& entity) {
        const auto* joint = component(entity, "joint");
        if (!joint || !component_header("Joint", "joint")) return;
        const auto joint_request = entity_field(selection);
        const auto set = [&](const std::string& fields, const char* label) {
            mutate("scene.set_joint", joint_request + fields, label);
        };
        constexpr std::array<const char*, 5> type_names{"Fixed", "Point (ball and socket)", "Hinge",
                                                        "Slider", "Distance (rope or spring)"};
        constexpr std::array<const char*, 5> type_values{"fixed", "point", "hinge", "slider",
                                                         "distance"};
        const auto type_text = string_or(*joint, "type", "hinge");
        std::size_t type = 2;
        for (std::size_t index = 0; index < type_values.size(); ++index)
            if (type_text == type_values[index]) type = index;
        const bool hinge = type == 2, slider = type == 3, distance = type == 4;
        const bool open = inspector_begin_combo("Joint type", type_names[type]);
        note_item("joint:type");
        if (open) {
            for (std::size_t index = 0; index < type_names.size(); ++index) {
                if (ImGui::Selectable(type_names[index], index == type))
                    set(std::string{",\"type\":\""} + type_values[index] + '"', "Joint type updated");
                note_item(std::string{"joint:type:"} + type_values[index]);
            }
            ImGui::EndCombo();
        }
        bool enabled = boolean_or(*joint, "enabled", true);
        if (inspector_checkbox("Enabled##joint", &enabled))
            set(std::string{",\"enabled\":"} + (enabled ? "true" : "false"), "Joint updated");

        // The partner: any other node with a physics body or collider, or the world.
        const auto connected = string_or(*joint, "connected");
        const auto* partner = connected.empty() ? nullptr : find_entity(connected);
        // The body this node is joined to, or a fixed point in the world.
        const auto connect = [this, joint_request](const std::string& handle) {
            mutate("scene.set_joint", joint_request + ",\"connected\":\"" + handle + '"',
                   handle.empty() ? "Joint connected to the world" : "Joint connected");
        };
        const auto self = selection;
        asset_field(
            "Connected to", connected, "joint:connected",
            [=, this] {
                AssetPick pick;
                pick.key = "joint:connected";
                pick.title = "Choose the body to join to";
                pick.what = "a node with a collider or physics body";
                pick.current = connected;
                pick.choices.push_back({"", "World", AssetIcon::node, "", "", std::nullopt});
                for (const auto* candidate : entities) {
                    const auto handle = string_or(*candidate, "entity");
                    if (handle == self || (!component(*candidate, "physics_body") && !component(*candidate, "collider")))
                        continue;
                    pick.choices.push_back({handle, string_or(*candidate, "name"), AssetIcon::node, "@scene",
                                            node_path(handle), std::nullopt});
                }
                pick.choose = connect;
                return pick;
            }, "World", AssetIcon::node,
            partner ? std::optional<Reference>(Reference{string_or(*partner, "name"), AssetIcon::node, node_path(connected)})
                    : std::nullopt);

        auto anchor = editor_vector(*joint, "anchor", {0, 0, 0});
        if (const auto mask = drag_vector3("Anchor", anchor, 0.05F, 84.0F * ui_scale))
            set(vector_fields(anchor, {"anchor_x", "anchor_y", "anchor_z"}, mask), "Joint anchor updated");
        if (hinge || slider) {
            auto axis = editor_vector(*joint, "axis", {0, 1, 0});
            if (const auto mask = drag_vector3("Axis", axis, 0.05F, 84.0F * ui_scale);
                mask && (axis[0] != 0.0 || axis[1] != 0.0 || axis[2] != 0.0))
                set(vector_fields(axis, {"axis_x", "axis_y", "axis_z"}, mask), "Joint axis updated");
        }
        if (distance) {
            auto far = editor_vector(*joint, "connected_anchor", {0, 0, 0});
            if (const auto mask = drag_vector3(partner ? "Other anchor" : "World point", far, 0.05F,
                                               84.0F * ui_scale))
                set(vector_fields(far, {"connected_anchor_x", "connected_anchor_y",
                                        "connected_anchor_z"}, mask),
                    "Joint anchor updated");
        }
        if (hinge || slider || distance) {
            bool limits = boolean_or(*joint, "limits", false);
            const char* limits_label = hinge ? "Limit angle" : slider ? "Limit travel" : "Limit length";
            if (inspector_checkbox(limits_label, &limits))
                set(std::string{",\"limits\":"} + (limits ? "true" : "false"), "Joint limits updated");
            if (distance && !limits)
                ImGui::TextDisabled("Keeps the length it has when the game starts.");
            if (limits) {
                // Clamped to what the joint type accepts, so a drag never produces a refusal.
                auto minimum = number_or(*joint, "limit_min", 0.0);
                auto maximum = number_or(*joint, "limit_max", 0.0);
                const char* unit = hinge ? "%.1f deg" : "%.2f m";
                if (drag_scalar("Minimum", minimum, hinge ? 1.0F : 0.05F, unit)) {
                    minimum = hinge ? std::clamp(minimum, -180.0, 0.0)
                            : slider ? std::min(minimum, 0.0)
                                     : std::clamp(minimum, 0.0, maximum);
                    set(",\"limit_min\":" + number_text(minimum), "Joint limits updated");
                }
                if (drag_scalar("Maximum", maximum, hinge ? 1.0F : 0.05F, unit)) {
                    maximum = hinge ? std::clamp(maximum, 0.0, 180.0)
                            : slider ? std::max(maximum, 0.0)
                                     : std::max(maximum, minimum);
                    set(",\"limit_max\":" + number_text(maximum), "Joint limits updated");
                }
            }
        }
        if (hinge || slider) {
            bool motor = boolean_or(*joint, "motor", false);
            if (inspector_checkbox("Motor", &motor))
                set(std::string{",\"motor\":"} + (motor ? "true" : "false"), "Joint motor updated");
            if (motor) {
                auto speed = number_or(*joint, "motor_speed", 90.0);
                if (drag_scalar("Speed", speed, hinge ? 1.0F : 0.05F, hinge ? "%.1f deg/s" : "%.2f m/s"))
                    set(",\"motor_speed\":" + number_text(speed), "Joint motor updated");
                auto force = number_or(*joint, "motor_force", 1000.0);
                if (drag_scalar(hinge ? "Max torque" : "Max force", force, 10.0F,
                                hinge ? "%.0f N m" : "%.0f N"))
                    set(",\"motor_force\":" + number_text(std::max(force, 0.0)), "Joint motor updated");
            }
        }
        if (distance) {
            auto frequency = number_or(*joint, "spring_frequency", 0.0);
            if (drag_scalar("Spring frequency", frequency, 0.05F, "%.2f Hz"))
                set(",\"spring_frequency\":" + number_text(std::clamp(frequency, 0.0, 1000.0)),
                    "Joint spring updated");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("0 keeps the length rigid");
            auto damping = number_or(*joint, "spring_damping", 0.5);
            if (drag_scalar("Spring damping", damping, 0.01F))
                set(",\"spring_damping\":" + number_text(std::clamp(damping, 0.0, 100.0)),
                    "Joint spring updated");
        }
        if (partner) {
            bool collide = boolean_or(*joint, "collide_connected", false);
            if (inspector_checkbox("Collide with connected", &collide))
                set(std::string{",\"collide_connected\":"} + (collide ? "true" : "false"),
                    "Joint updated");
        }
        // Say plainly why a joint would do nothing.
        const auto dynamic = [&](const JsonValue::Object* node) {
            const auto* body = node ? component(*node, "physics_body") : nullptr;
            return body && number_or(*body, "type", 1) == 1;
        };
        const auto warning = editor_color(editor_palette().warning);
        if (!component(entity, "physics_body") && !component(entity, "collider"))
            ImGui::TextColored(warning, "Add a physics body or collider to this node.");
        else if (!dynamic(&entity) && !dynamic(partner))
            ImGui::TextColored(warning, "One of the joined bodies must be dynamic.");
        if (!enabled)
            ImGui::TextDisabled("Removing the connected node disables its joints.");
    }

    std::vector<std::string> script_behaviours() const {
        std::vector<std::string> names;
        const auto* status = scripts();
        const auto* list = status ? field(*status, "behaviours") : nullptr;
        if (list && list->array())
            for (const auto& item : *list->array())
                if (const auto* info = item.object()) names.push_back(string_or(*info, "name"));
        return names;
    }

    const JsonValue::Object* behaviour_info(const std::string& name) const {
        const auto* status = scripts();
        const auto* list = status ? field(*status, "behaviours") : nullptr;
        if (list && list->array())
            for (const auto& item : *list->array())
                if (const auto* info = item.object(); info && string_or(*info, "name") == name)
                    return info;
        return nullptr;
    }

    // A removable component's header carries a close button and a Remove context item; closing
    // it removes the component through the protocol like any other edit.
    bool component_header(const char* label, const std::string& id, const std::size_t index = 0,
                          const bool removable = true) {
        bool keep = true;
        const bool open = ImGui::CollapsingHeader(label, removable ? &keep : nullptr,
                                                  ImGuiTreeNodeFlags_DefaultOpen);
        note_item("inspector:component:" + id + (id == "script" ? ":" + std::to_string(index) : ""));
        if (removable && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Remove component")) keep = false;
            ImGui::EndPopup();
        }
        if (keep) return open;
        mutate("component.remove", entity_field(selection) + ",\"component\":\"" + id + '"' +
                                       (id == "script" ? ",\"index\":" + std::to_string(index) : ""),
               "Component removed");
        return false;
    }

    // The asset kind a file name's extension implies, or empty for plain text.
    static std::string asset_kind_of_file(const std::string_view name) {
        switch (asset_icon_for_file(name)) {
        case AssetIcon::audio: return "audio";
        case AssetIcon::image: return "image";
        case AssetIcon::model: return "model";
        case AssetIcon::material: return "material";
        case AssetIcon::shader: return "shader";
        case AssetIcon::node_template: return "template";
        case AssetIcon::scene: return "scene";
        case AssetIcon::video: return "media";
        default: return {};
        }
    }

    // A text script property holds an asset path when its default or value names one.
    static std::string script_asset_kind(const JsonValue::Object& declared, const JsonValue& value) {
        for (const auto* text : {value.string(), field(declared, "value") ? field(declared, "value")->string() : nullptr})
            if (text && !text->empty())
                if (auto kind = asset_kind_of_file(*text); !kind.empty()) return kind;
        return {};
    }

    void draw_script_property(const std::string& script_request, const JsonValue::Object& declared,
                              const JsonValue::Object* stored) {
        const auto name = string_or(declared, "name");
        const auto type = string_or(declared, "type");
        const auto* value = field(stored ? *stored : declared, "value");
        if (!value) return;
        const auto set = [&](const std::string& json_value) {
            mutate("scene.set_script_property",
                   script_request + ",\"property\":\"" + name + "\"," + json_value, "Property updated");
        };
        ImGui::PushID(name.c_str());
        if (type == "boolean") {
            bool current = value->boolean() && *value->boolean();
            if (inspector_checkbox(name.c_str(), &current))
                set(std::string{"\"boolean\":"} + (current ? "true" : "false"));
        } else if (type == "number") {
            double current = value->number() ? *value->number() : 0.0;
            if (drag_scalar(name.c_str(), current, 0.05F, "%.4g"))
                set("\"number\":" + number_text(current));
        } else if (type == "vector") {
            const auto parsed = [&](std::size_t axis) {
                const auto* items = value->array();
                return items && axis < items->size() && (*items)[axis].number()
                           ? *(*items)[axis].number() : 0.0;
            };
            std::array<double, 3> current{parsed(0), parsed(1), parsed(2)};
            if (drag_vector3(name.c_str(), current, 0.01F, 72.0F * ui_scale))
                set("\"vector\":[" + number_text(current[0]) + "," + number_text(current[1]) +
                    "," + number_text(current[2]) + "]");
        } else if (const auto asset_kind = type == "text" ? script_asset_kind(declared, *value) : std::string{};
                   !asset_kind.empty()) {
            // Text naming a project file (its default or value has an asset's extension) is edited
            // as an asset reference.
            const auto current = value->string() ? *value->string() : std::string{};
            const auto choose = [this, script_request, name](const std::string& file) {
                mutate("scene.set_script_property",
                       script_request + ",\"property\":\"" + name + "\",\"text\":\"" + json_escape(file) + '"',
                       "Property updated");
            };
            const auto key = "inspector:script:" + script_request + ":" + name;
            asset_field(
                name.c_str(), current, key,
                [=, this] {
                    AssetPick pick;
                    pick.key = key;
                    pick.title = "Choose " + name;
                    pick.current = current;
                    pick.kinds = {asset_kind};
                    pick.choices.push_back({"", "None", AssetIcon::other, "", "", std::nullopt});
                    pick.choose = choose;
                    return pick;
                },
                "None", asset_icon_for_kind(asset_kind));
        } else if (type == "text") {
            auto& buffer = script_text_buffers[script_request + name];
            // Show the stored value unless the person is typing in this field.
            if (ImGui::GetActiveID() != ImGui::GetID(("##" + name).c_str())) {
                buffer.fill('\0');
                const auto text = value->string() ? *value->string() : std::string{};
                std::copy_n(text.begin(), std::min(text.size(), buffer.size() - 1U), buffer.begin());
            }
            inspector_field_label(name.c_str());
            ImGui::InputText(("##" + name).c_str(), buffer.data(), buffer.size());
            if (ImGui::IsItemDeactivatedAfterEdit())
                set("\"text\":\"" + json_escape(std::string{buffer.data()}) + '"');
        }
        if (stored) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset"))
                mutate("scene.set_script_property",
                       script_request + ",\"property\":\"" + name + "\",\"reset\":true", "Property reset");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Use the default from the script's code");
        }
        ImGui::PopID();
    }

    void draw_script_sections(const JsonValue::Object& entity) {
        const auto* list = field(entity, "scripts");
        if (!list || !list->array()) return;
        const auto& components = *list->array();
        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto* script = components[index].object();
            if (!script) continue;
            ImGui::PushID(static_cast<int>(index));
            const auto behaviour = string_or(*script, "behaviour");
            const auto label = behaviour + " (Script)###script";
            if (!component_header(label.c_str(), "script", index)) {
                ImGui::PopID();
                continue;
            }
            const auto script_request = entity_field(selection) + ",\"index\":" + std::to_string(index);
            bool enabled = boolean_or(*script, "enabled", true);
            if (inspector_checkbox("Enabled", &enabled))
                mutate("scene.set_script", script_request + ",\"enabled\":" + (enabled ? "true" : "false"),
                       enabled ? "Script enabled" : "Script disabled");
            const auto* info = behaviour_info(behaviour);
            const auto* declared = info ? field(*info, "properties") : nullptr;
            const auto* stored_list = field(*script, "properties");
            if (!info) {
                ImGui::TextDisabled(script_behaviours().empty()
                                        ? "Build the project's scripts to edit its properties."
                                        : "The current scripts do not define this behaviour.");
            } else if (declared && declared->array()) {
                for (const auto& item : *declared->array()) {
                    const auto* property = item.object();
                    if (!property) continue;
                    const JsonValue::Object* stored = nullptr;
                    if (stored_list && stored_list->array())
                        for (const auto& candidate : *stored_list->array())
                            if (const auto* object = candidate.object();
                                object && string_or(*object, "name") == string_or(*property, "name") &&
                                string_or(*object, "type") == string_or(*property, "type"))
                                stored = object;
                    draw_script_property(script_request, *property, stored);
                }
            }
            ImGui::PopID();
        }
    }

    static std::string lowercase(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return text;
    }

    // Maps a component id to the key its data uses in scene JSON.
    static std::string_view component_key(const std::string_view id) {
        return id == "keyframes" ? std::string_view{"transform_animation"} : id;
    }

    void draw_add_component() {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(5, 5, 5, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(8, 8, 8, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(11, 11, 11, 255));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(13, 13, 13, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F * ui_scale);
        if (ImGui::Button("+ Add Component", ImVec2(ImGui::GetContentRegionAvail().x, 0.0F))) {
            component_query.fill('\0');
            component_selected.clear();
            if (auto catalog = call("component.types")) component_catalog = std::move(*catalog);
            open_component_window = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        note_item("inspector:add_component");
    }

    void draw_morph_section(const JsonValue::Object& entity) {
        const auto* renderer = component(entity, "mesh_renderer");
        if (renderer == nullptr) return;
        const auto* weights = field(*renderer, "morph_weights");
        const auto found = morph_defaults.find(string_or(*renderer, "mesh"));
        if (found == morph_defaults.end() || found->second.empty()) return;
        const auto& defaults = found->second;
        const auto count = defaults.size();
        if (!ImGui::CollapsingHeader("Morph targets")) return;
        const auto* overrides = weights ? weights->array() : nullptr;
        if (!overrides || overrides->empty())
            ImGui::TextDisabled("Inherited defaults / animation; edit to override.");
        for (std::size_t index = 0; index < count && index < 64U; ++index) {
            auto weight = overrides && index < overrides->size() && (*overrides)[index].number()
                              ? *(*overrides)[index].number()
                              : defaults[index];
            ImGui::PushID(static_cast<int>(index));
            const bool commit = drag_scalar("weight", weight, 0.01F);
            ImGui::PopID();
            if (commit) {
                mutate("scene.set_morph",
                       entity_field(selection) + ",\"target\":" + std::to_string(index) +
                           ",\"weight\":" + number_text(weight),
                       "Morph weight updated");
            }
        }
        if (ImGui::Button("Reset morphs")) {
            mutate("scene.set_morph", entity_field(selection) + ",\"reset\":true",
                   "Morph weights reset");
        }
    }

    std::vector<std::string> animation_targets() const {
        std::vector<std::string> result;
        auto handles = selections.handles;
        const auto active = std::find(handles.begin(), handles.end(), selection);
        if (active != handles.end()) std::rotate(handles.begin(), active, active + 1);
        for (const auto& handle : handles) {
            const auto* entity = find_entity(handle);
            if (!entity) continue;
            auto root = handle;
            if (!component(*entity, "animator")) {
                const auto* node = component(*entity, "model_node");
                root = node ? string_or(*node, "root") : "";
                entity = find_entity(root);
            }
            if (entity && component(*entity, "animator") &&
                std::find(result.begin(), result.end(), root) == result.end()) result.push_back(root);
        }
        if (result.empty())
            for (const auto* entity : entities)
                if (component(*entity, "animator")) { result.push_back(string_or(*entity, "entity")); break; }
        return result;
    }

    bool timeline_mutate(const std::vector<std::string>& targets, const std::string& fields) {
        std::string request_fields = "\"entities\":[";
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (i) request_fields += ',';
            request_fields += '\"' + targets[i] + '\"';
        }
        return mutate("scene.set_animations", request_fields + "]" + fields, "Animation tracks updated");
    }

    void draw_timeline() {
        const auto targets = animation_targets();
        if (targets.empty()) { ImGui::TextDisabled("Import an animated model to use the timeline."); return; }
        const auto* root = find_entity(targets.front());
        const auto* animator = root ? component(*root, "animator") : nullptr;
        if (!animator) return;
        const auto model = string_or(*animator, "model");
        const auto clips = clips_for(model);
        const auto clip = static_cast<std::size_t>(number_or(*animator, "clip", 0));
        const auto duration = clip < clips.size() ? clips[clip].duration_seconds : 0.0;
        const auto time = number_or(*animator, "time_seconds", 0);
        const bool playing = boolean_or(*animator, "playing", false);
        const bool loop = boolean_or(*animator, "loop", true);
        if (toolbar_button("##timeline_restart", ToolIcon::restart, false, "Return to beginning"))
            timeline_mutate(targets, ",\"time_seconds\":0");
        if (toolbar_button("##timeline_previous", ToolIcon::previous, false, "Previous frame"))
            timeline_mutate(targets, ",\"playing\":false,\"time_seconds\":" + number_text(timeline_step(time, duration, timeline_fps, -1)));
        if (toolbar_button("##timeline_play", playing ? ToolIcon::pause : ToolIcon::play, playing, playing ? "Pause animation" : "Play animation"))
            timeline_mutate(targets, std::string(",\"playing\":") + (playing ? "false" : "true"));
        if (toolbar_button("##timeline_next", ToolIcon::step, false, "Next frame"))
            timeline_mutate(targets, ",\"playing\":false,\"time_seconds\":" + number_text(timeline_step(time, duration, timeline_fps, 1)));
        if (toolbar_button("##timeline_loop", ToolIcon::loop, loop, "Loop animation"))
            timeline_mutate(targets, std::string(",\"loop\":") + (loop ? "false" : "true"));
        if (toolbar_button("##timeline_snap", ToolIcon::snap, timeline_snap, "Snap scrubbing to frames")) timeline_snap = !timeline_snap;
        ImGui::SetNextItemWidth(72.0F * ui_scale);
        ImGui::InputInt("FPS", &timeline_fps, 0, 0);
        timeline_fps = std::clamp(timeline_fps, 1, 240);
        ImGui::SameLine();
        double speed = number_or(*animator, "speed", 1);
        ImGui::SetNextItemWidth(80.0F * ui_scale);
        if (drag_scalar("Speed", speed, 0.01F, "%.2fx"))
            timeline_mutate(targets, ",\"speed\":" + number_text(std::clamp(speed, -100.0, 100.0)));
        ImGui::SetNextItemWidth(220.0F * ui_scale);
        const auto clip_name = clip < clips.size() ? clips[clip].name : "Unavailable clip";
        if (ImGui::BeginCombo("Clip", clip_name.c_str())) {
            for (std::size_t i = 0; i < clips.size(); ++i)
                if (ImGui::Selectable(clips[i].name.c_str(), i == clip))
                    mutate("scene.set_animation", entity_field(targets.front()) + ",\"clip\":" + std::to_string(i) + ",\"time_seconds\":0", "Clip selected");
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Text("%.3f / %.3f s  |  Frame %lld", timeline_scrub_targets.empty() ? time : timeline_preview_time, duration, static_cast<long long>(std::llround(time * timeline_fps)));
        if (duration <= 0) { ImGui::TextDisabled("Clip duration is unavailable."); return; }
        const auto metadata_key = model + ":" + std::to_string(clip);
        if (metadata_key != timeline_metadata_key) {
            if (auto metadata = call("animation.clip", "\"model\":\"" + json_escape(model) + "\",\"clip\":" + std::to_string(clip))) {
                timeline_metadata = std::move(*metadata);
                timeline_metadata_key = metadata_key;
            }
        }
        const auto label_width = std::min(200.0F * ui_scale, ImGui::GetContentRegionAvail().x * 0.35F);
        ImGui::TextDisabled("Time");
        ImGui::SameLine(label_width);
        const auto origin = ImGui::GetCursorScreenPos();
        const auto width = std::max(1.0F, ImGui::GetContentRegionAvail().x);
        const auto ruler_height = 35.0F * ui_scale;
        auto* draw = ImGui::GetWindowDrawList();
        ImGui::InvisibleButton("##time_ruler", ImVec2(width, ruler_height));
        note_item("timeline:ruler");
        if (ImGui::IsItemActivated()) {
            timeline_gesture = ++gesture_serial;
            timeline_scrub_targets = targets;
        }
        if (ImGui::IsItemActive()) {
            const auto next = timeline_time((ImGui::GetIO().MousePos.x - origin.x) / width,
                                            duration, timeline_fps, timeline_snap);
            if (ImGui::IsItemActivated() || next != timeline_preview_time) {
                timeline_preview_time = next;
                timeline_mutate(timeline_scrub_targets, ",\"playing\":false,\"time_seconds\":" + number_text(next) +
                                ",\"gesture\":" + std::to_string(timeline_gesture));
            }
        }
        const auto shown_time = timeline_scrub_targets.empty() ? time : timeline_preview_time;
        if (ImGui::IsItemDeactivated()) timeline_scrub_targets.clear();
        const auto& palette = editor_palette();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + ruler_height), palette.panel);
        const double desired = std::max(1e-9, duration / std::max(1.0, static_cast<double>(width / (90.0F * ui_scale))));
        const double magnitude = std::pow(10.0, std::floor(std::log10(desired)));
        const double ratio = desired / magnitude;
        const double spacing = magnitude * (ratio <= 1 ? 1 : ratio <= 2 ? 2 : ratio <= 5 ? 5 : 10);
        const auto tick_label = [&](const double value) {
            const auto x = origin.x + width * static_cast<float>(value / duration);
            draw->AddLine(ImVec2(x, origin.y + 18 * ui_scale), ImVec2(x, origin.y + ruler_height), palette.border);
            char label[48];
            std::snprintf(label, sizeof(label), "%.2f", value);
            const auto label_width_px = ImGui::CalcTextSize(label).x;
            draw->AddText(ImVec2(std::min(x + 3, origin.x + width - label_width_px), origin.y), palette.text_faint, label);
        };
        for (double tick = 0; tick < duration - spacing * 0.25; tick += spacing) tick_label(tick);
        tick_label(duration);
        const auto playhead = origin.x + width * static_cast<float>(std::clamp(shown_time / duration, 0.0, 1.0));
        draw->AddLine(ImVec2(playhead, origin.y), ImVec2(playhead, origin.y + ruler_height), palette.accent, 2 * ui_scale);
        ImGui::Separator();
        begin_region("##timeline_tracks");
        draw = ImGui::GetWindowDrawList();
        for (const auto* entity : entities) {
            const auto* track = component(*entity, "animator");
            if (!track) continue;
            const auto handle = string_or(*entity, "entity");
            ImGui::PushID(handle.c_str());
            const auto row_origin = ImGui::GetCursorScreenPos();
            const auto label = string_or(*entity, "name") + "  |  " + number_text(number_or(*track, "time_seconds", 0)).substr(0, 6) + " s";
            if (ImGui::Selectable(label.c_str(), std::find(targets.begin(), targets.end(), handle) != targets.end(), 0, ImVec2(label_width - 10 * ui_scale, 0)))
                click_selection(handle);
            const auto track_clips = clips_for(string_or(*track, "model"));
            const auto track_clip = static_cast<std::size_t>(number_or(*track, "clip", 0));
            const auto track_duration = track_clip < track_clips.size() ? track_clips[track_clip].duration_seconds : 0;
            const auto bar_end = origin.x + width * static_cast<float>(std::clamp(track_duration / duration, 0.0, 1.0));
            draw->AddRectFilled(ImVec2(origin.x, row_origin.y + 3 * ui_scale), ImVec2(bar_end, row_origin.y + 15 * ui_scale), palette.accent_soft);
            const auto track_head = origin.x + width * static_cast<float>(std::clamp(number_or(*track, "time_seconds", 0) / duration, 0.0, 1.0));
            draw->AddLine(ImVec2(track_head, row_origin.y), ImVec2(track_head, row_origin.y + ImGui::GetTextLineHeightWithSpacing()), palette.accent, 2 * ui_scale);
            ImGui::PopID();
        }
        if (timeline_metadata.object()) {
            const auto* channels = field(*timeline_metadata.object(), "channels");
            if (channels && channels->array())
                for (const auto& channel : *channels->array()) {
                    if (!channel.object()) continue;
                    const auto row = ImGui::GetCursorScreenPos();
                    const auto channel_name = string_or(*channel.object(), "name");
                    ImGui::PushClipRect(row, ImVec2(origin.x - 8 * ui_scale, row.y + ImGui::GetTextLineHeightWithSpacing()), true);
                    ImGui::TextDisabled("%s", channel_name.c_str());
                    ImGui::PopClipRect();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", channel_name.c_str());
                    const auto* times = field(*channel.object(), "times");
                    if (times && times->array())
                        for (const auto& key : *times->array()) {
                            if (!key.number()) continue;
                            const auto x = origin.x + width * static_cast<float>(std::clamp(*key.number() / duration, 0.0, 1.0));
                            draw->AddCircleFilled(ImVec2(x, row.y + 8 * ui_scale), 2.5F * ui_scale, palette.accent);
                        }
                    draw->AddLine(ImVec2(playhead, row.y), ImVec2(playhead, row.y + ImGui::GetTextLineHeightWithSpacing()), palette.accent);
                    if (times && times->array() && number_or(*channel.object(), "key_count", 0) > static_cast<double>(times->array()->size()))
                        ImGui::TextDisabled("Key markers sampled for display.");
                }
        }
        ImGui::EndChild();
    }

    void draw_inspector() {
        if (selection.empty()) {
            ImGui::Dummy(ImVec2(0.0F, 24.0F * ui_scale));
            ImGui::PushFont(fonts.heading, fonts.heading_size);
            ImGui::TextUnformatted("No selection");
            ImGui::PopFont();
            ImGui::TextColored(editor_color(editor_palette().text_dim),
                               "Select a node in the hierarchy or viewport");
            ImGui::TextColored(editor_color(editor_palette().text_dim),
                               "to edit its components here.");
            return;
        }
        const auto* entity = find_entity(selection);
        if (entity == nullptr) {
            ImGui::TextDisabled("The selected entity no longer exists.");
            return;
        }
        drawing_inspector = true;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(8.0F * ui_scale, 6.0F * ui_scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(9.0F * ui_scale, 5.0F * ui_scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(3, 3, 3, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(6, 6, 6, 255));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(9, 9, 9, 255));
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(8, 8, 8, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(11, 11, 11, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(14, 14, 14, 255));

        ImGui::PushFont(fonts.heading, fonts.heading_size);
        ImGui::TextWrapped("%s", string_or(*entity, "name", "Entity").c_str());
        ImGui::PopFont();
        ImGui::TextColored(editor_color(editor_palette().text_dim), "%s  ·  ID %s",
                           string_or(*entity, "type", "Node").c_str(), selection.c_str());
        ImGui::Separator();
        ImGui::Spacing();

        draw_transform_section(*entity);
        // Each section gets its own ID scope, so fields with the same label in two sections of
        // one node, such as a Sky's and its sun's Intensity, stay separate widgets.
        const auto section = [&](const char* id, const auto& draw) {
            if (!component(*entity, id)) return;
            ImGui::PushID(id);
            draw();
            ImGui::PopID();
        };
        section("camera", [&] { draw_camera_section(*entity); });
        section("mesh_renderer", [&] { draw_renderer_section(*entity); });
        section("animator", [&] { draw_animator_section(*entity); });
        section("transform_animation", [&] { draw_keyframes_section(*entity); });
        section("mesh_renderer", [&] {
            const auto morph = morph_defaults.find(string_or(*component(*entity, "mesh_renderer"), "mesh"));
            if (morph != morph_defaults.end() && !morph->second.empty()) {
                ImGui::PushID("morph");
                draw_morph_section(*entity);
                ImGui::PopID();
            }
        });
        section("sky", [&] { draw_sky_section(*entity); });
        section("post_process", [&] { draw_post_process_section(*entity); });
        section("light", [&] { draw_light_section(*entity); });
        section("collider", [&] { draw_collider_section(*entity); });
        section("physics_body", [&] { draw_physics_body_section(*entity); });
        section("joint", [&] { draw_joint_section(*entity); });
        section("audio_source", [&] { draw_audio_source_section(*entity); });
        section("audio_listener", [&] { draw_audio_listener_section(); });
        section("reverb_zone", [&] { draw_reverb_zone_section(*entity); });
        section("music_player", [&] { draw_music_player_section(*entity); });
        if (const auto* scripts = field(*entity, "scripts"); scripts && scripts->array() &&
            !scripts->array()->empty())
            draw_script_sections(*entity);
        draw_add_component();
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(2);
        drawing_inspector = false;
    }

    // Pulls the scene revision out of wherever a response carries it: `scene.history` nests it
    // under `state`, mutations under `history`, and `scene.save` reports it at the top level.
    static std::optional<std::uint64_t> response_revision(const JsonValue& value) {
        const auto* object = value.object();
        if (object == nullptr) return std::nullopt;
        for (const auto* key : {"history", "state"}) {
            if (const auto* nested = field(*object, key); nested && nested->object())
                object = nested->object();
        }
        const auto* revision = field(*object, "revision");
        if (revision == nullptr || revision->number() == nullptr) return std::nullopt;
        return static_cast<std::uint64_t>(*revision->number());
    }

    void set_scene_filename(const std::string& filename) {
        const auto length = std::min(filename.size(), scene_filename.size() - 1U);
        std::copy_n(filename.begin(), length, scene_filename.begin());
        scene_filename[length] = '\0';
    }

    // Saving and opening both settle the unsaved-work marker, so they share one path instead of
    // each having to remember to update it. The revision comes from the response rather than a
    // later poll, so an edit arriving in between cannot be mistaken for saved work.
    bool open_or_save_scene(const bool opening, const std::string& filename) {
        const auto result = call(opening ? "scene.load" : "scene.save",
                                 "\"filename\":\"" + json_escape(filename) + '\"');
        if (!result) return false;
        if (const auto revision = response_revision(*result)) {
            scene_revision = *revision;
            saved_revision = *revision;
        }
        set_scene_filename(filename);
        scene_has_file = true;
        refresh_pending = true;
        if (opening) {
            assets_pending = true;
            select({});
        }
        if (!opening) {
            if (auto project = call("project.status")) project_status = std::move(*project);
        }
        if (!opening && active_project()) {
            if (!call("project.add_scene", "\"scene_file\":\"" + json_escape(filename) + '\"')) {
                set_status("Scene saved; project membership could not be updated", true);
                return false;
            }
        }
        set_status((opening ? "Opened " : "Saved ") + filename, false);
        return true;
    }

    const JsonValue::Object* active_project() const {
        const auto* object = project_status.object();
        if (!object) return nullptr;
        const auto* project = field(*object, "project");
        return project ? project->object() : nullptr;
    }

    bool open_or_create_project(bool create, const std::string& filename) {
        std::string fields = "\"filename\":\"" + json_escape(filename) + '\"';
        if (create) fields += ",\"name\":\"" + json_escape(project_name.data()) + '\"';
        const auto result = call(create ? "project.create" : "project.open", fields);
        if (!result || !result->object()) return false;
        const auto scene = string_or(*result->object(), "scene_file");
        scene_has_file = !scene.empty();
        if (scene_has_file) set_scene_filename(scene);
        saved_revision = scene_revision;
        select({});
        asset_folders.clear();
        expanded_asset_folders.clear();
        asset_selection.clear();
        inline_rename = {};
        assets_pending = refresh_pending = true;
        if (auto project = call("project.status")) project_status = std::move(*project);
        set_status(create ? "Project created" : "Project opened", false);
        return true;
    }

    void draw_project() {
        const auto* project = active_project();
        if (ImGui::Button("New project")) discarding_action(PendingAction::new_project);
        ImGui::SameLine();
        if (ImGui::Button("Open project")) discarding_action(PendingAction::open_project);
        if (project) {
            ImGui::Separator();
            ImGui::TextUnformatted(string_or(*project, "name").c_str());
            ImGui::TextDisabled("Folder: %s", string_or(*project, "root").c_str());
            if (ImGui::Button("Add existing scene"))
                file_dialog(FileAction::add_project_scene, scene_filename.data());
            ImGui::SameLine();
            if (ImGui::Button("New scene")) discarding_action(PendingAction::new_scene);
            ImGui::TextDisabled("Saving a scene adds it to this project.");
            const auto startup = string_or(*project, "startup_scene");
            if (const auto* files = field(*project, "scenes"); files && files->array())
                for (const auto& entry : *files->array()) {
                    if (!entry.string()) continue;
                    const auto& file = *entry.string();
                    const auto label = file + (file == startup ? " (startup)" : "");
                    const bool selected_scene = ImGui::Selectable(label.c_str(), scene_has_file && file == scene_filename.data());
                    note_item("project:scene:" + file);
                    if (selected_scene) {
                        pending_scene_filename = file;
                        discarding_action(PendingAction::project_scene);
                    }
                    if (ImGui::BeginPopupContextItem()) {
                        const auto fields = "\"scene_file\":\"" + json_escape(file) + '\"';
                        if (ImGui::MenuItem("Set as startup scene"))
                            mutate("project.set_startup", fields, "Startup scene updated");
                        if (ImGui::MenuItem("Remove from project"))
                            mutate("project.remove_scene", fields, "Scene removed from project; file kept");
                        ImGui::EndPopup();
                    }
                }
            ImGui::Separator();
            ImGui::InputText("Package file", package_filename.data(), package_filename.size());
            if (ImGui::Button("Export saved project"))
                (void)mutate("project.package", "\"filename\":\"" +
                             json_escape(package_filename.data()) + "\"",
                             "Project package saved under exports");
            ImGui::TextDisabled("Save scene changes before exporting. Existing packages are kept.");
        } else ImGui::TextDisabled("No project open. Scene files can still be edited.");
        ImGui::Separator();
        ImGui::TextDisabled("Available projects");
        for (const auto& file : project_files)
            if (ImGui::Selectable(file.c_str())) {
                pending_project_filename = file;
                discarding_action(PendingAction::open_project);
            }
    }

    bool save_scene() {
        // Without a file behind it yet, Save has to ask where the scene should go rather than
        // silently claiming whatever name the field happens to be holding.
        if (!scene_has_file) {
            file_dialog(FileAction::save_as, scene_filename.data());
            return false;
        }
        return open_or_save_scene(false, scene_filename.data());
    }

    // Selecting the copy is what makes duplicate useful for laying a scene out: the next drag or
    // inspector edit lands on the new object rather than the one it was made from.
    void group_operation(const char* method, const char* message, bool uses_selection = true) {
        const auto result = call(method, uses_selection ? selection_fields() : std::string{});
        if (!result) return;
        set_status(message, false);
        refresh_pending = true;
        if (std::string_view(method) == "scene.copy" || std::string_view(method) == "scene.cut")
            clipboard_ready = true;
        if (const auto* object = result->object()) {
            if (const auto* copied_roots = field(*object, "roots"); copied_roots && copied_roots->array()) {
                select({});
                for (const auto& root : *copied_roots->array())
                    if (root.string()) selections.click(*root.string(), true, false);
            }
        }
        if (std::string_view(method) == "scene.cut" || std::string_view(method) == "scene.destroy_many")
            select({});
    }

    void duplicate_selection() {
        if (!selection.empty()) group_operation("scene.duplicate_many", "Duplicated selection");
    }

    void new_scene() {
        const auto result = call("scene.clear");
        if (!result) return;
        // An empty scene has nothing to lose, so it starts clean; it is untitled until saved, so
        // the next Save asks for a name instead of overwriting the file that was open before.
        if (const auto revision = response_revision(*result)) {
            scene_revision = *revision;
            saved_revision = *revision;
        }
        scene_has_file = false;
        select({});
        refresh_pending = true;
        assets_pending = true;
        set_status("New scene", false);
    }

    // Anything that throws away the current scene routes through here, so unsaved work cannot be
    // discarded by a single menu click.
    void discarding_action(const PendingAction action) {
        // An external caller may have edited since the last panel refresh. Check the current
        // revision before deciding that it is safe to discard the scene.
        if (!call("scene.history")) return;
        if (!scene_modified()) {
            run_pending_action(action);
            return;
        }
        pending_action = action;
        open_discard_dialog = true;
    }

    void run_pending_action(const PendingAction action) {
        switch (action) {
        case PendingAction::none: break;
        case PendingAction::new_scene: new_scene(); break;
        case PendingAction::open_scene: file_dialog(FileAction::open, scene_filename.data()); break;
        case PendingAction::quit: mutate("runtime.quit", {}, "Closing editor"); break;
        case PendingAction::new_project: file_dialog(FileAction::new_project, "projects/my-project/project.relayproject"); break;
        case PendingAction::open_project: file_dialog(FileAction::open_project, !pending_project_filename.empty() ? pending_project_filename : project_files.empty() ? "projects/my-project/project.relayproject" : project_files.front()); pending_project_filename.clear(); break;
        case PendingAction::project_scene: (void)open_or_save_scene(true, pending_scene_filename); break;
        }
    }

    [[nodiscard]] std::string scene_title() const {
        return std::string(scene_has_file ? scene_filename.data() : "Untitled scene") +
               (scene_modified() ? "*" : "");
    }

    // The window title is where a person tracks which scene they are editing and whether it still
    // needs saving, so it follows the same revision comparison the File menu uses.
    void update_window_title() {
        // The window keeps "Relay Editor" as its trailing name so anything matching on it, the
        // desktop harnesses included, still finds the window once the scene name is in front.
        const auto* project = active_project();
        auto title = (project ? string_or(*project, "name") + " / " : std::string{}) + scene_title() + " - Relay Editor";
        if (sdl_window == nullptr || title == window_title) return;
        window_title = std::move(title);
        SDL_SetWindowTitle(sdl_window, window_title.c_str());
    }

    void file_dialog(FileAction action, const std::string& filename) {
        file_action = action;
        open_file_dialog = true;
        dialog_error.clear();
        const auto length = std::min(filename.size(), action_filename.size() - 1);
        std::copy_n(filename.begin(), length, action_filename.begin());
        action_filename[length] = '\0';
    }

    std::string create_node(const char* name, const char* component = nullptr,
                            const char* fields = "") {
        auto created = call("scene.create", "\"name\":\"" + json_escape(name) + '\"');
        if (!created || !created->object())
            return {};
        const auto handle = string_or(*created->object(), "entity");
        const bool component_ok =
            !component || mutate(component, entity_field(handle) + fields, "Component added");
        refresh_pending = true;
        select(handle);
        if (component_ok)
            set_status(std::string("Created ") + name, false);
        return component_ok ? handle : std::string{};
    }

    static void future_action(const char* label) {
        ImGui::MenuItem(label, "Coming soon", false, false);
    }

    std::string capture_filename(const char* extension) {
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        return "editor-" + std::to_string(stamp) + "-" + std::to_string(++capture_serial) +
               extension;
    }

    void draw_menu_bar() {
        if (!ImGui::BeginMainMenuBar())
            return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New project...")) discarding_action(PendingAction::new_project);
            if (ImGui::MenuItem("Open project...")) discarding_action(PendingAction::open_project);
            if (ImGui::MenuItem("Project browser")) panel_open[7] = true;
            if (ImGui::MenuItem("Close project", nullptr, false, active_project() != nullptr))
                if (mutate("project.close", {}, "Project closed")) project_status = JsonValue{};
            ImGui::Separator();
            if (ImGui::MenuItem("New scene", "Ctrl+N"))
                discarding_action(PendingAction::new_scene);
            if (ImGui::MenuItem("Open scene...", "Ctrl+O"))
                discarding_action(PendingAction::open_scene);
            if (ImGui::MenuItem("Save scene", "Ctrl+S", false, scene_modified() || !scene_has_file))
                save_scene();
            if (ImGui::MenuItem("Save scene as...", "Ctrl+Shift+S"))
                file_dialog(FileAction::save_as, scene_filename.data());
            ImGui::Separator();
            if (ImGui::MenuItem("Import model..."))
                file_dialog(FileAction::import, model_filename.data());
            future_action("Export project...");
            ImGui::Separator();
            if (ImGui::MenuItem("Quit"))
                discarding_action(PendingAction::quit);
            ImGui::EndMenu();
        }
        const bool edit_menu = ImGui::BeginMenu("Edit");
        note_item("menu:edit");
        if (edit_menu) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_labels.empty()))
                mutate("scene.undo", {}, "Undone");
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo_labels.empty()))
                mutate("scene.redo", {}, "Redone");
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X", false, !selection.empty()))
                group_operation("scene.cut", "Cut selection");
            if (ImGui::MenuItem("Copy", "Ctrl+C", false, !selection.empty()))
                group_operation("scene.copy", "Copied selection");
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, clipboard_ready))
                group_operation("scene.paste", "Pasted selection", false);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection.empty()))
                duplicate_selection();
            if (ImGui::MenuItem("Delete selection", "Delete", false, !selection.empty())) {
                group_operation("scene.destroy_many", "Deleted selection");
            }
            if (ImGui::MenuItem("Clear selection", nullptr, false, !selection.empty()))
                select({});
            ImGui::Separator();
            if (ImGui::MenuItem("Game Configuration...")) open_game_config();
            note_item("menu:edit:game_configuration");
            future_action("Editor preferences...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::MenuItem("Add Node...")) open_add_node(selection);
            if (ImGui::MenuItem("Save selection as template...", nullptr, false, !selection.empty()))
                if (const auto* entity = find_entity(selection))
                    begin_save_template(selection, string_or(*entity, "name"));
            if (ImGui::MenuItem("Frame selection", "F", false, !selection.empty()))
                focus_selection();
            ImGui::Separator();
            future_action("Scene settings...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Editor camera", nullptr, camera_enabled))
                camera_enabled = true;
            if (ImGui::MenuItem("Scene camera", nullptr, !camera_enabled))
                camera_enabled = false;
            if (ImGui::MenuItem("Reset editor camera")) {
                navigation_camera = EditorCamera{};
                camera_enabled = true;
            }
            ImGui::Separator();
            for (std::size_t i = 0; i < panel_open.size(); ++i)
                ImGui::MenuItem(panel_names[i], nullptr, &panel_open[i]);
            ImGui::Separator();
            ImGui::MenuItem("Ground grid", nullptr, &grid_enabled);
            ImGui::MenuItem("Collider wireframes", nullptr, &collider_wireframes_enabled);
            ImGui::MenuItem("Camera and light icons", nullptr, &node_icons_enabled);
            ImGui::MenuItem("Camera view wireframe", nullptr, &camera_wireframes_enabled);
            future_action("Wireframe");
            future_action("Lighting debug");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run")) {
            const auto* status = runtime_status.object();
            const bool game = status && string_or(*status, "mode") == "game";
            const bool paused = status && boolean_or(*status, "paused", false);
            if (ImGui::MenuItem(game ? "Stop Game" : "Run Game", game ? "F8" : "F5")) {
                if (game) mutate("runtime.stop", {}, "Game stopped");
                else request_play();
            }
            ImGui::BeginDisabled(!game);
            if (ImGui::MenuItem(paused ? "Resume simulation" : "Pause simulation"))
                mutate(paused ? "runtime.resume" : "runtime.pause", {},
                       paused ? "Resumed" : "Paused");
            if (ImGui::MenuItem("Step one frame"))
                mutate("runtime.step", "\"frames\":1", "Stepped one frame");
            ImGui::EndDisabled();
            ImGui::Separator();
            future_action("Run standalone game");
            future_action("Build project...");
            ImGui::EndMenu();
        }
        const bool tools_menu = ImGui::BeginMenu("Tools");
        note_item("menu:tools");
        if (tools_menu) {
            const auto video = call("video.status");
            const bool recording =
                video && video->object() && boolean_or(*video->object(), "recording", false);
            const bool finalizing =
                video && video->object() && boolean_or(*video->object(), "finalizing", false);
            if (ImGui::MenuItem("Capture GPU screenshot..."))
                file_dialog(FileAction::screenshot, capture_filename(".png"));
            if (ImGui::MenuItem("Record GPU WebM...", nullptr, false, !recording && !finalizing))
                file_dialog(FileAction::recording, capture_filename(".webm"));
            if (ImGui::MenuItem("Stop recording", nullptr, false, recording))
                defer_action("video.stop", {}, "Recording finalizing; see Diagnostics");
            ImGui::Separator();
            if (finalizing)
                ImGui::TextDisabled("WebM is finalizing in the background...");
            if (ImGui::MenuItem("Refresh assets and diagnostics")) {
                assets_pending = true;
                refresh_pending = true;
            }
            if (ImGui::MenuItem("Asset browser")) open_asset_browser(std::nullopt);
            note_item("menu:asset_browser");
            if (ImGui::MenuItem("Shader editor")) panel_open[11] = true;
            note_item("menu:shader_editor");
            if (ImGui::MenuItem("Animation timeline")) panel_open[6] = true;
            if (ImGui::MenuItem("Profiler")) panel_open[9] = true;
            if (ImGui::MenuItem("Audio mixer")) {
                panel_open[10] = true;
                refresh_audio_files();
            }
            note_item("menu:audio_mixer");
            if (ImGui::MenuItem("Agent workspace...", "Ctrl+Shift+A")) { panel_open[8] = true; agent_expand_pending = true; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            if (ImGui::MenuItem("Reset layout")) {
                panel_open = default_panels;
                layout.reset();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Drag tabs to move, dock or group panels.");
            ImGui::TextDisabled("Hold Shift while dragging to float a panel.");
            ImGui::TextDisabled("Drag dividers or edges to resize.");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Editor controls"))
                show_help = true;
            if (ImGui::MenuItem("About Relay"))
                show_about = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    void draw_script_dialogs() {
        if (open_trust_dialog) {
            ImGui::OpenPopup("Trust project scripts?");
            open_trust_dialog = false;
        }
        if (ImGui::BeginPopupModal("Trust project scripts?", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            const auto* status = project_status.object();
            const auto name = status ? string_or(*status, "name", "This project") : "This project";
            ImGui::Text("%s uses gameplay scripts.", name.c_str());
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0F);
            ImGui::TextUnformatted(
                "Scripts are native C++ compiled and run by Relay with your full user "
                "permissions, like any program you install. They can read, change or delete "
                "your files. Only trust projects whose code you trust, including code written by "
                "agents in this project.");
            ImGui::PopTextWrapPos();
            const bool accepted = ImGui::Button("Trust and run");
            note_item("dialog:trust:accept");
            if (accepted) {
                if (call("scripts.trust", "\"trusted\":true")) {
                    script_status_reply.clear();
                    refresh_scripts();
                    request_play();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (open_new_script_dialog) {
            ImGui::OpenPopup("New C++ script");
            new_script_name.fill('\0');
            open_new_script_dialog = false;
        }
        if (ImGui::BeginPopupModal("New C++ script", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Behaviour class name");
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            const bool entered = ImGui::InputTextWithHint(
                "##new_script", "PlayerController", new_script_name.data(), new_script_name.size(),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
            ImGui::TextDisabled("Creates scripts/<name>.cpp");
            const std::string name = new_script_name.data();
            ImGui::BeginDisabled(name.empty());
            if ((ImGui::Button("Create") || entered) && !name.empty()) {
                if (call("scripts.create", "\"behaviour\":\"" + json_escape(name) + '"')) {
                    set_status("Created scripts/" + name + ".cpp", false);
                    if (!attach_new_script_to.empty())
                        mutate("component.add", entity_field(attach_new_script_to) +
                                                    ",\"component\":\"script\",\"behaviour\":\"" +
                                                    json_escape(name) + '"',
                               "Created scripts/" + name + ".cpp and attached it");
                    attach_new_script_to.clear();
                    refresh_asset_listing();
                    set_asset_folder_open("scripts", true);
                    asset_selection = "scripts/" + name + ".cpp";
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // A searchable browser: categories on the left, components on the right, details below.
    void draw_add_component_window() {
        if (open_component_window) {
            ImGui::OpenPopup("Add Component");
            open_component_window = false;
        }
        const auto* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main_viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImVec2(640.0F * ui_scale, 460.0F * ui_scale), ImGuiCond_Appearing);
        bool keep_open = true;
        if (!ImGui::BeginPopupModal("Add Component", &keep_open, ImGuiWindowFlags_NoSavedSettings))
            return;
        const auto* entity = find_entity(selection);
        if (!entity) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        const auto& palette = editor_palette();
        ImGui::TextColored(editor_color(palette.text_dim), "Add to %s",
                           string_or(*entity, "name", "node").c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##component_search", "Search components", component_query.data(),
                                 component_query.size());
        note_item("component_window:search");

        struct Item {
            std::string key, name, category, description, fields;
            bool present{};
        };
        std::vector<Item> items;
        const auto* catalog = component_catalog.object();
        if (const auto* engine = catalog ? field(*catalog, "engine") : nullptr; engine && engine->array())
            for (const auto& value : *engine->array()) {
                const auto* kind = value.object();
                if (!kind || !boolean_or(*kind, "addable", false)) continue;
                const auto id = string_or(*kind, "id");
                if (id == "script") continue;
                const auto* data = field(*entity, component_key(id));
                items.push_back({id, string_or(*kind, "name"), string_or(*kind, "category"),
                                 string_or(*kind, "description"), ",\"component\":\"" + id + '"',
                                 data && !data->is_null()});
            }
        if (const auto* behaviours = catalog ? field(*catalog, "behaviours") : nullptr;
            behaviours && behaviours->array())
            for (const auto& value : *behaviours->array()) {
                const auto* info = value.object();
                if (!info) continue;
                const auto name = string_or(*info, "name");
                std::string description = "C++ behaviour from scripts/.";
                if (const auto* properties = field(*info, "properties");
                    properties && properties->array() && !properties->array()->empty()) {
                    description += " Properties:";
                    for (const auto& property : *properties->array())
                        if (const auto* object = property.object())
                            description += " " + string_or(*object, "name");
                } else {
                    description += " No editable properties.";
                }
                items.push_back({"script:" + name, name, "Scripts", description,
                                 ",\"component\":\"script\",\"behaviour\":\"" + name + '"', false});
            }
        const auto query = lowercase(component_query.data());
        const auto visible = [&](const Item& item) {
            return (component_category == "All" || item.category == component_category) &&
                   (query.empty() || lowercase(item.name).find(query) != std::string::npos ||
                    lowercase(item.description).find(query) != std::string::npos);
        };
        const Item* chosen = nullptr;
        for (const auto& item : items)
            if (item.key == component_selected && visible(item)) chosen = &item;
        if (!chosen)
            for (const auto& item : items)
                if (visible(item) && !item.present) {
                    chosen = &item;
                    component_selected = item.key;
                    break;
                }
        bool add_now = false;

        const float footer = ImGui::GetTextLineHeightWithSpacing() * 3.0F +
                             ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0F;
        if (ImGui::BeginChild("##component_categories", ImVec2(150.0F * ui_scale, -footer),
                              ImGuiChildFlags_Borders)) {
            for (const std::string category : {"All", "Rendering", "Physics", "Animation", "Scripts"}) {
                const auto count = std::count_if(items.begin(), items.end(), [&](const Item& item) {
                    return category == "All" || item.category == category;
                });
                const auto label = category + " (" + std::to_string(count) + ")";
                if (ImGui::Selectable(label.c_str(), component_category == category))
                    component_category = category;
                note_item("component_window:category:" + category);
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##component_items", ImVec2(0.0F, -footer), ImGuiChildFlags_Borders)) {
            bool any = false;
            for (const auto& item : items) {
                if (!visible(item)) continue;
                any = true;
                ImGui::PushID(item.key.c_str());
                const bool picked = ImGui::Selectable(
                    item.name.c_str(), chosen == &item,
                    ImGuiSelectableFlags_AllowDoubleClick |
                        (item.present ? ImGuiSelectableFlags_Disabled : 0));
                note_item("component_window:item:" + item.key);
                const auto tag = item.present ? std::string{"Added"} : item.category;
                const auto size = ImGui::CalcTextSize(tag.c_str());
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(ImGui::GetItemRectMax().x - size.x - 4.0F * ui_scale, ImGui::GetItemRectMin().y),
                    palette.text_faint, tag.c_str());
                if (picked) {
                    component_selected = item.key;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) add_now = true;
                }
                ImGui::PopID();
            }
            if (!any)
                ImGui::TextDisabled(component_category == "Scripts" && query.empty()
                                        ? "No built behaviours yet. Create one with New C++ script;\n"
                                          "it appears here once the project's scripts build."
                                        : "Nothing matches the search.");
        }
        ImGui::EndChild();

        if (chosen) {
            ImGui::TextUnformatted(chosen->name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            ImGui::TextWrapped("%s", chosen->description.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Choose a component.");
        }
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() -
                             ImGui::GetStyle().WindowPadding.y);
        if (ImGui::Button("New C++ script...")) {
            attach_new_script_to = selection;
            open_new_script_dialog = true;
            ImGui::CloseCurrentPopup();
        }
        const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Add").x +
                              ImGui::GetStyle().FramePadding.x * 4.0F +
                              ImGui::GetStyle().ItemSpacing.x + 24.0F * ui_scale;
        ImGui::SameLine(ImGui::GetWindowWidth() - buttons - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(!chosen || chosen->present);
        add_now |= ImGui::Button("Add");
        note_item("component_window:add");
        add_now |= ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        ImGui::EndDisabled();
        if (add_now && chosen && !chosen->present) {
            mutate("component.add", entity_field(selection) + chosen->fields, chosen->name + " added");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    const JsonValue::Object* node_type_info(const std::string& id) const {
        const auto* catalog = node_type_catalog.object();
        const auto* types = catalog ? field(*catalog, "types") : nullptr;
        if (types && types->array())
            for (const auto& value : *types->array())
                if (const auto* type = value.object(); type && string_or(*type, "id") == id) return type;
        return nullptr;
    }

    std::string component_display_name(const std::string& id) const {
        const auto* catalog = component_catalog.object();
        const auto* engine = catalog ? field(*catalog, "engine") : nullptr;
        if (engine && engine->array())
            for (const auto& value : *engine->array())
                if (const auto* kind = value.object(); kind && string_or(*kind, "id") == id)
                    return string_or(*kind, "name");
        return id;
    }

    static float template_icon_size() { return ImGui::GetFontSize() * 0.72F; }

    // A small monochrome painter's palette marking custom templates, with a tooltip saying so:
    // an outlined round body with a notch cut from its lower right, and four paint wells.
    void draw_template_icon(const ImVec2 at, const std::string& key) {
        const auto& palette = editor_palette();
        const float size = template_icon_size();
        const ImVec2 maximum(at.x + size, at.y + size);
        const bool hovered = ImGui::IsMouseHoveringRect(at, maximum) && ImGui::IsWindowHovered();
        // Faint text at half strength, so the icon sits back from the name it marks.
        const ImU32 color = hovered ? palette.text_faint
                                    : (palette.text_faint & ~IM_COL32_A_MASK) |
                                          (static_cast<ImU32>(0x80) << IM_COL32_A_SHIFT);
        const float thickness = std::max(1.0F, 1.1F * ui_scale);
        const float radius = size * 0.5F - thickness * 0.5F;
        const ImVec2 center(at.x + size * 0.5F, at.y + size * 0.5F);
        auto* list = ImGui::GetWindowDrawList();
        // The body runs clockwise round the circle, then back along the notch's concave edge.
        constexpr float notch_angle = 0.785398F, notch_gap = 0.45F, two_pi = 6.2831853F;
        const float start = notch_angle + notch_gap, end = notch_angle - notch_gap + two_pi;
        const ImVec2 notch(center.x + std::cos(notch_angle) * radius * 0.95F,
                           center.y + std::sin(notch_angle) * radius * 0.95F);
        const ImVec2 from(center.x + std::cos(end) * radius, center.y + std::sin(end) * radius);
        const ImVec2 to(center.x + std::cos(start) * radius, center.y + std::sin(start) * radius);
        const float from_angle = std::atan2(from.y - notch.y, from.x - notch.x);
        float to_angle = std::atan2(to.y - notch.y, to.x - notch.x);
        if (to_angle > from_angle) to_angle -= two_pi;
        list->PathArcTo(center, radius, start, end, 24);
        list->PathArcTo(notch, std::hypot(from.x - notch.x, from.y - notch.y), from_angle, to_angle, 8);
        list->PathStroke(color, ImDrawFlags_Closed, thickness);
        const ImVec2 wells[]{{-0.46F, 0.08F}, {-0.28F, -0.42F}, {0.16F, -0.52F}, {0.5F, -0.18F}};
        for (const auto& well : wells)
            list->AddCircleFilled(ImVec2(center.x + well.x * radius, center.y + well.y * radius),
                                  radius * 0.15F, color, 10);
        note_rect(key, at, maximum);
        if (hovered) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Custom template");
            note_item("tooltip:custom_template");
            ImGui::EndTooltip();
        }
    }

    // A custom template's row: a leaf in the node type tree, or a search result.
    void draw_template_row(const TemplateEntry& entry, const bool in_tree, bool& create_now) {
        ImGui::PushID(entry.id.c_str());
        bool clicked = false;
        float label_x = 0.0F;
        if (in_tree) {
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
            if (node_selected == entry.id) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx("##template", flags, "%s", entry.name.c_str());
            clicked = ImGui::IsItemClicked();
            label_x = ImGui::GetItemRectMin().x + ImGui::GetTreeNodeToLabelSpacing();
        } else {
            clicked = ImGui::Selectable(entry.name.c_str(), node_selected == entry.id,
                                        ImGuiSelectableFlags_AllowDoubleClick);
            label_x = ImGui::GetItemRectMin().x;
        }
        note_item("node_window:template:" + entry.name);
        if (clicked) {
            node_selected = entry.id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) create_now = true;
        }
        const float icon = template_icon_size();
        const float row_top = ImGui::GetItemRectMin().y, row_bottom = ImGui::GetItemRectMax().y;
        draw_template_icon(ImVec2(label_x + ImGui::CalcTextSize(entry.name.c_str()).x +
                                      ImGui::GetStyle().ItemInnerSpacing.x * 1.5F,
                                  (row_top + row_bottom - icon) * 0.5F),
                           "node_window:template_icon:" + entry.name);
        ImGui::PopID();
    }

    // One row of the node type tree, with its subtypes and then its custom templates beneath it.
    void draw_node_type_row(const JsonValue::Object& type, bool& create_now) {
        const auto id = string_or(type, "id");
        std::vector<const JsonValue::Object*> subtypes;
        const auto* types = field(*node_type_catalog.object(), "types");
        for (const auto& value : *types->array())
            if (const auto* child = value.object(); child && string_or(*child, "parent") == id)
                subtypes.push_back(child);
        std::vector<const TemplateEntry*> templates;
        for (const auto& entry : node_templates)
            if (entry.type == id) templates.push_back(&entry);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (subtypes.empty() && templates.empty())
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (node_selected == id) flags |= ImGuiTreeNodeFlags_Selected;
        const bool creatable = boolean_or(type, "creatable", false);
        if (!creatable) ImGui::PushStyleColor(ImGuiCol_Text, editor_color(editor_palette().text_faint));
        const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", string_or(type, "name").c_str());
        if (!creatable) ImGui::PopStyleColor();
        note_item("node_window:type:" + id);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            node_selected = id;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && creatable) create_now = true;
        }
        if (open && !(subtypes.empty() && templates.empty())) {
            for (const auto* child : subtypes) draw_node_type_row(*child, create_now);
            for (const auto* entry : templates) draw_template_row(*entry, true, create_now);
            ImGui::TreePop();
        }
    }

    // Node types and custom templates as one inheritance tree; details on the right.
    void draw_add_node_window() {
        if (open_node_window) {
            ImGui::OpenPopup("Add Node");
            open_node_window = false;
        }
        const auto* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(main_viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSize(ImVec2(680.0F * ui_scale, 480.0F * ui_scale), ImGuiCond_Appearing);
        bool keep_open = true;
        if (!ImGui::BeginPopupModal("Add Node", &keep_open, ImGuiWindowFlags_NoSavedSettings)) return;
        const auto& palette = editor_palette();
        const auto* parent = node_parent.empty() ? nullptr : find_entity(node_parent);
        if (parent) {
            const auto label = "Add as a child of " + string_or(*parent, "name", "the selection");
            editor_checkbox(label.c_str(), &node_as_child);
        } else {
            ImGui::TextColored(editor_color(palette.text_dim), "Adds a node at the top of the scene");
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##node_search", "Search node types and templates", node_query.data(),
                                 node_query.size());
        note_item("node_window:search");
        const auto query = lowercase(node_query.data());
        const auto* catalog = node_type_catalog.object();
        const auto* types = catalog ? field(*catalog, "types") : nullptr;
        bool create_now = false;

        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild("##node_tree", ImVec2(ImGui::GetContentRegionAvail().x * 0.5F, -footer),
                              ImGuiChildFlags_Borders)) {
            if (types && types->array()) {
                if (query.empty()) {
                    for (const auto& value : *types->array())
                        if (const auto* type = value.object(); type && string_or(*type, "parent").empty())
                            draw_node_type_row(*type, create_now);
                } else {
                    for (const auto& value : *types->array()) {
                        const auto* type = value.object();
                        if (!type || !boolean_or(*type, "creatable", false) ||
                            lowercase(string_or(*type, "name")).find(query) == std::string::npos)
                            continue;
                        const auto id = string_or(*type, "id");
                        if (ImGui::Selectable(string_or(*type, "name").c_str(), node_selected == id,
                                              ImGuiSelectableFlags_AllowDoubleClick)) {
                            node_selected = id;
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) create_now = true;
                        }
                        note_item("node_window:type:" + id);
                    }
                    for (const auto& entry : node_templates)
                        if (lowercase(entry.name).find(query) != std::string::npos)
                            draw_template_row(entry, false, create_now);
                }
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        const auto template_entry = std::find_if(node_templates.begin(), node_templates.end(),
            [&](const TemplateEntry& entry) { return entry.id == node_selected; });
        const auto* type = node_type_info(node_selected);
        const bool is_template = template_entry != node_templates.end();
        const bool creatable = is_template || (type && boolean_or(*type, "creatable", false));
        if (ImGui::BeginChild("##node_details", ImVec2(0.0F, -footer), ImGuiChildFlags_Borders)) {
            // The inheritance chain, root first.
            const auto inherits = [&](std::string current) {
                std::vector<std::string> chain;
                for (; !current.empty();) {
                    const auto* ancestor = node_type_info(current);
                    if (!ancestor) break;
                    chain.insert(chain.begin(), string_or(*ancestor, "name"));
                    current = string_or(*ancestor, "parent");
                }
                if (chain.empty()) return;
                std::string path;
                for (const auto& name : chain) path += (path.empty() ? "" : " > ") + name;
                ImGui::TextColored(editor_color(palette.text_dim), "Inherits %s", path.c_str());
            };
            if (is_template) {
                ImGui::PushFont(fonts.heading, fonts.heading_size * 1.1F);
                ImGui::TextUnformatted(template_entry->name.c_str());
                ImGui::PopFont();
                const float icon = template_icon_size();
                draw_template_icon(ImVec2(ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemInnerSpacing.x * 1.5F,
                                          (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y - icon) * 0.5F),
                                   "node_window:details_template_icon");
                inherits(template_entry->type);
                ImGui::Spacing();
                ImGui::TextWrapped("A custom template saved in this project. Creates a copy of the "
                                   "saved node and its children; later changes to the template do "
                                   "not affect copies.");
                ImGui::Spacing();
                ImGui::SeparatorText("Components");
                for (const auto& component : template_entry->components)
                    ImGui::BulletText("%s", component_display_name(component).c_str());
                for (const auto& behaviour : template_entry->behaviours)
                    ImGui::BulletText("%s (Script)", behaviour.c_str());
            } else if (type) {
                ImGui::PushFont(fonts.heading, fonts.heading_size * 1.1F);
                ImGui::TextUnformatted(string_or(*type, "name").c_str());
                ImGui::PopFont();
                inherits(string_or(*type, "parent"));
                ImGui::Spacing();
                ImGui::TextWrapped("%s", string_or(*type, "description").c_str());
                ImGui::Spacing();
                ImGui::SeparatorText("Components");
                if (const auto* components = field(*type, "components"); components && components->array())
                    for (const auto& component : *components->array())
                        if (component.string())
                            ImGui::BulletText("%s", component_display_name(*component.string()).c_str());
                if (!creatable)
                    ImGui::TextDisabled(string_or(*type, "id") == "Model"
                                            ? "Import a model file to create one."
                                            : "Choose one of the types below it.");
            }
        }
        ImGui::EndChild();

        ImGui::SetNextItemWidth(220.0F * ui_scale);
        const auto hint = is_template ? template_entry->name
                                      : (type ? string_or(*type, "name") : std::string{"Name"});
        ImGui::InputTextWithHint("##node_name", hint.c_str(), node_name.data(), node_name.size());
        note_item("node_window:name");
        const float buttons = ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Create").x +
                              ImGui::GetStyle().FramePadding.x * 4.0F +
                              ImGui::GetStyle().ItemSpacing.x + 24.0F * ui_scale;
        ImGui::SameLine(ImGui::GetWindowWidth() - buttons - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        ImGui::BeginDisabled(!creatable);
        create_now |= ImGui::Button("Create");
        note_item("node_window:create");
        create_now |= ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        ImGui::EndDisabled();
        if (create_now && creatable) {
            create_node_from(node_selected, node_as_child && parent ? node_parent : std::string{},
                             node_name.data());
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void open_game_config() {
        game_config_open = true;
        load_input_edit();
    }

    void load_input_edit() {
        const auto map = call("input.map");
        const auto* object = map ? map->object() : nullptr;
        const auto* current = object ? field(*object, "map") : nullptr;
        if (!current) return;
        std::string error;
        if (auto parsed = parse_input_map(json_stringify(*current), error))
            input_edit = std::move(*parsed);
        input_file_saved = boolean_or(*object, "saved", false);
        input_name_buffers.clear();
    }

    void save_input_edit() {
        if (!call("input.set_map", "\"map\":\"" + json_escape(input_map_json(input_edit)) + '"')) {
            load_input_edit();
            return;
        }
        input_file_saved = true;
        game_lock_mouse = input_edit.lock_mouse;
        set_status("Input map saved", false);
    }

    static bool analog_input(const std::string& control) {
        return control == "gamepad:leftx" || control == "gamepad:lefty" ||
               control == "gamepad:rightx" || control == "gamepad:righty" ||
               control == "gamepad:left_trigger" || control == "gamepad:right_trigger";
    }

    // "key:left_shift" reads as "Left Shift", "gamepad:leftx" as "Left Stick X".
    static std::string control_label(const std::string& control) {
        const auto colon = control.find(':');
        if (colon == std::string::npos) return control;
        const auto device = control.substr(0, colon);
        const auto name = control.substr(colon + 1U);
        std::string words;
        bool start = true;
        for (const char character : name) {
            if (character == '_') {
                words += ' ';
                start = true;
                continue;
            }
            words += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(character)))
                           : character;
            start = false;
        }
        if (device == "mouse") return words + " Mouse";
        if (device == "gamepad") {
            if (name == "leftx") return "Left Stick X";
            if (name == "lefty") return "Left Stick Y";
            if (name == "rightx") return "Right Stick X";
            if (name == "righty") return "Right Stick Y";
            return "Gamepad " + words;
        }
        return words;
    }

    void begin_binding_capture(const BindingCapture::Target target, const std::size_t index) {
        binding_capture = {};
        binding_capture.active = true;
        binding_capture.target = target;
        binding_capture.index = index;
    }

    void cancel_binding_capture() { binding_capture = {}; }

    // Records a captured control into the entry that asked for it and saves the map.
    void finish_binding_capture(const std::string& control) {
        using Target = BindingCapture::Target;
        auto& capture = binding_capture;
        // An analog binding waits for a stick or trigger; other controls are ignored.
        if (capture.target == Target::analog && !analog_input(control)) return;
        if (capture.target == Target::pair_negative) {
            capture.negative = control;
            capture.target = Target::pair_positive;
            return;
        }
        if (capture.target == Target::action) {
            if (capture.index < input_edit.actions.size()) {
                auto& bindings = input_edit.actions[capture.index].bindings;
                if (std::find(bindings.begin(), bindings.end(), control) == bindings.end() &&
                    bindings.size() < maximum_input_bindings)
                    bindings.push_back(control);
            }
        } else if (capture.index < input_edit.axes.size() &&
                   input_edit.axes[capture.index].bindings.size() < maximum_input_bindings) {
            auto& bindings = input_edit.axes[capture.index].bindings;
            if (capture.target == Target::analog) bindings.push_back({"", "", control, 1.0});
            else bindings.push_back({capture.negative, control, "", 1.0});
        }
        binding_capture = {};
        save_input_edit();
    }

    // Row layout that wraps: the next item stays on this line if it fits before `right`.
    struct Flow {
        float right{};
        bool first{true};
    };
    static float button_width(const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0F;
    }
    static void flow(Flow& row, const float width) {
        if (!row.first) {
            ImGui::SameLine();
            if (ImGui::GetCursorScreenPos().x + width > row.right) ImGui::NewLine();
        }
        row.first = false;
    }
    static Flow begin_flow() {
        return {ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x, true};
    }
    bool flow_button(Flow& row, const char* label) {
        flow(row, button_width(label));
        return ImGui::SmallButton(label);
    }

    // The prompt a binding capture shows in place of the add buttons; mouse clicks inside it bind.
    void draw_capture_prompt(Flow& row, const char* text) {
        flow(row, button_width(text));
        ImGui::PushStyleColor(ImGuiCol_Button, editor_color(editor_palette().accent_soft));
        ImGui::SmallButton(text);
        ImGui::PopStyleColor();
        const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
        binding_capture.zone = {minimum.x, minimum.y, maximum.x, maximum.y};
        note_item("config:input:capture");
        if (flow_button(row, "Cancel")) cancel_binding_capture();
    }

    // A binding chip: the control's name and a remove button. Returns true when removed.
    bool binding_chip(Flow& row, const std::string& label, const std::string& id) {
        flow(row, button_width(label.c_str()) + 2.0F * ui_scale + button_width("x"));
        ImGui::PushID(id.c_str());
        ImGui::PushStyleColor(ImGuiCol_Button, editor_color(editor_palette().surface));
        ImGui::SmallButton(label.c_str());
        ImGui::PopStyleColor();
        note_item("config:input:chip:" + label);
        ImGui::SameLine(0.0F, 2.0F * ui_scale);
        const bool removed = ImGui::SmallButton("x");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this binding");
        ImGui::PopID();
        return removed;
    }

    bool input_name_taken(const std::string& name) const {
        return std::any_of(input_edit.actions.begin(), input_edit.actions.end(),
                           [&](const InputAction& action) { return action.name == name; }) ||
               std::any_of(input_edit.axes.begin(), input_edit.axes.end(),
                           [&](const InputAxis& axis) { return axis.name == name; });
    }

    // Edits an action or axis name in place, committing when the field is left.
    bool input_name_field(const std::string& key, std::string& name) {
        auto& buffer = input_name_buffers[key];
        if (ImGui::GetActiveID() != ImGui::GetID("##name")) {
            buffer.fill('\0');
            std::copy_n(name.begin(), std::min<std::size_t>(name.size(), 64U), buffer.begin());
        }
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("##name", buffer.data(), buffer.size(), ImGuiInputTextFlags_CharsNoBlank);
        if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
        const std::string renamed = buffer.data();
        if (renamed == name) return false;
        if (!valid_input_name(renamed)) {
            set_status("Input names are letters, digits and underscores", true);
            return false;
        }
        if (input_name_taken(renamed)) {
            set_status("An action or axis is already called " + renamed, true);
            return false;
        }
        name = renamed;
        return true;
    }

    // The live input.state entry for an action or axis, while the game runs.
    const JsonValue::Object* live_entry(const char* list, const std::string& name) const {
        const auto* object = input_live.object();
        const auto* entries = object ? field(*object, list) : nullptr;
        if (entries && entries->array())
            for (const auto& value : *entries->array())
                if (const auto* entry = value.object(); entry && string_or(*entry, "name") == name)
                    return entry;
        return nullptr;
    }

    // A name field and button that add an entry; returns the valid new name when pressed.
    std::optional<std::string> add_input_entry(std::array<char, 65>& buffer, const char* id,
                                               const char* hint, const char* button) {
        ImGui::SetNextItemWidth(180.0F * ui_scale);
        const bool entered = ImGui::InputTextWithHint(
            id, hint, buffer.data(), buffer.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsNoBlank);
        note_item(std::string{"config:input:"} + (id + 2));
        ImGui::SameLine();
        const std::string name = buffer.data();
        ImGui::BeginDisabled(name.empty());
        const bool pressed = ImGui::Button(button) || entered;
        ImGui::EndDisabled();
        note_item(std::string{"config:input:"} + (id + 2) + ":add");
        if (!pressed || name.empty()) return std::nullopt;
        if (!valid_input_name(name) || input_name_taken(name)) {
            set_status(input_name_taken(name) ? "That name is already used"
                                              : "Input names are letters, digits and underscores",
                       true);
            return std::nullopt;
        }
        buffer.fill('\0');
        return name;
    }

    void draw_input_page() {
        using Target = BindingCapture::Target;
        const auto& palette = editor_palette();
        bool changed = false;
        const auto* project = project_status.object();
        if (!project || string_or(*project, "filename").empty())
            ImGui::TextColored(editor_color(palette.warning),
                               "Open a project to save its input map.");
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("Scripts read actions, such as jump, with relay::input::pressed, and "
                           "axes from -1 to 1, such as move_x, with relay::input::axis.");
        ImGui::PopStyleColor();
        ImGui::TextColored(editor_color(palette.text_faint), "%s",
                           input_file_saved ? "Saved in the project file"
                                            : "Engine defaults until you change something");
        if (editor_checkbox("Lock the mouse cursor while the game has input", &input_edit.lock_mouse))
            changed = true;
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) {
            const bool lock = input_edit.lock_mouse;
            input_edit = default_input_map();
            input_edit.lock_mouse = lock;
            input_name_buffers.clear();
            cancel_binding_capture();
            changed = true;
        }
        note_item("config:input:reset");
        const bool live = input_live.object() != nullptr;
        const auto table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                 ImGuiTableFlags_SizingStretchProp;
        std::optional<std::size_t> remove_action, remove_axis;

        ImGui::SeparatorText("Actions");
        if (ImGui::BeginTable("##actions", 4, table_flags)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 150.0F * ui_scale);
            ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 44.0F * ui_scale);
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 56.0F * ui_scale);
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < input_edit.actions.size(); ++index) {
                auto& action = input_edit.actions[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                changed |= input_name_field("action:" + std::to_string(index), action.name);
                ImGui::TableNextColumn();
                auto row = begin_flow();
                std::optional<std::size_t> remove_binding;
                for (std::size_t item = 0; item < action.bindings.size(); ++item)
                    if (binding_chip(row, control_label(action.bindings[item]), std::to_string(item)))
                        remove_binding = item;
                if (remove_binding) {
                    action.bindings.erase(action.bindings.begin() +
                                          static_cast<std::ptrdiff_t>(*remove_binding));
                    changed = true;
                }
                if (binding_capture.active && binding_capture.target == Target::action &&
                    binding_capture.index == index) {
                    draw_capture_prompt(row, "Press a key or gamepad button, or click here");
                } else {
                    if (flow_button(row, "+ Add")) begin_binding_capture(Target::action, index);
                    note_item("config:input:action:" + action.name + ":add");
                }
                ImGui::TableNextColumn();
                if (live) {
                    const auto* entry = live_entry("actions", action.name);
                    const bool held = entry && boolean_or(*entry, "held", false);
                    ImGui::TextColored(editor_color(held ? palette.success : palette.text_faint),
                                       held ? "held" : "-");
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Delete")) remove_action = index;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (const auto name = add_input_entry(new_action_name, "##new_action", "new_action",
                                              "Add action")) {
            input_edit.actions.push_back({*name, {}});
            begin_binding_capture(Target::action, input_edit.actions.size() - 1U);
            changed = true;
        }

        ImGui::SeparatorText("Axes");
        if (ImGui::BeginTable("##axes", 5, table_flags)) {
            ImGui::TableSetupColumn("Axis", ImGuiTableColumnFlags_WidthFixed, 150.0F * ui_scale);
            ImGui::TableSetupColumn("Deadzone", ImGuiTableColumnFlags_WidthFixed, 80.0F * ui_scale);
            ImGui::TableSetupColumn("Bindings", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 64.0F * ui_scale);
            ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 56.0F * ui_scale);
            ImGui::TableHeadersRow();
            for (std::size_t index = 0; index < input_edit.axes.size(); ++index) {
                auto& axis = input_edit.axes[index];
                ImGui::PushID(static_cast<int>(index) + 100000);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                changed |= input_name_field("axis:" + std::to_string(index), axis.name);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1.0F);
                auto deadzone = static_cast<float>(axis.deadzone);
                if (editor_slider("##deadzone", &deadzone, 0.0F, 0.95F, "%.2f"))
                    axis.deadzone = deadzone;
                changed |= ImGui::IsItemDeactivatedAfterEdit();
                ImGui::TableNextColumn();
                auto row = begin_flow();
                std::optional<std::size_t> remove_binding;
                for (std::size_t item = 0; item < axis.bindings.size(); ++item) {
                    auto& binding = axis.bindings[item];
                    const auto label =
                        binding.analog.empty()
                            ? control_label(binding.negative) + " / " + control_label(binding.positive)
                            : control_label(binding.analog) + (binding.scale < 0.0 ? " (inverted)" : "");
                    if (binding_chip(row, label, std::to_string(item))) remove_binding = item;
                }
                if (remove_binding) {
                    axis.bindings.erase(axis.bindings.begin() +
                                        static_cast<std::ptrdiff_t>(*remove_binding));
                    changed = true;
                }
                const bool capturing = binding_capture.active && binding_capture.index == index &&
                                       binding_capture.target != Target::action;
                if (capturing) {
                    draw_capture_prompt(row, binding_capture.target == Target::analog
                                            ? "Move a gamepad stick or trigger"
                                        : binding_capture.target == Target::pair_negative
                                            ? "Press the control for -1 (left, down or back)"
                                            : "Now press the control for +1");
                } else {
                    if (flow_button(row, "+ Keys")) begin_binding_capture(Target::pair_negative, index);
                    note_item("config:input:axis:" + axis.name + ":keys");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Bind two buttons: one pulls towards -1, the other towards +1");
                    if (flow_button(row, "+ Stick")) begin_binding_capture(Target::analog, index);
                    note_item("config:input:axis:" + axis.name + ":stick");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bind a gamepad stick or trigger");
                    bool inverts = false;
                    for (const auto& binding : axis.bindings) inverts |= !binding.analog.empty();
                    if (inverts) {
                        if (flow_button(row, "Invert...")) ImGui::OpenPopup("##invert_menu");
                        if (ImGui::BeginPopup("##invert_menu")) {
                            for (auto& binding : axis.bindings) {
                                if (binding.analog.empty()) continue;
                                if (ImGui::MenuItem(control_label(binding.analog).c_str(), nullptr,
                                                    binding.scale < 0.0)) {
                                    binding.scale = -binding.scale;
                                    changed = true;
                                }
                            }
                            ImGui::EndPopup();
                        }
                    }
                }
                ImGui::TableNextColumn();
                if (live) {
                    const auto* entry = live_entry("axes", axis.name);
                    const auto value = entry ? number_or(*entry, "value", 0.0) : 0.0;
                    char text[16];
                    std::snprintf(text, sizeof text, "%+.2f", value);
                    ImGui::ProgressBar(static_cast<float>((value + 1.0) * 0.5), ImVec2(-1.0F, 0.0F),
                                       text);
                }
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Delete")) remove_axis = index;
                note_item("config:input:axis:" + axis.name + ":delete");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (const auto name = add_input_entry(new_axis_name, "##new_axis", "new_axis", "Add axis")) {
            input_edit.axes.push_back({*name, 0.2, {}});
            changed = true;
        }
        if (remove_action) {
            input_edit.actions.erase(input_edit.actions.begin() +
                                     static_cast<std::ptrdiff_t>(*remove_action));
            cancel_binding_capture();
            input_name_buffers.clear();
            changed = true;
        }
        if (remove_axis) {
            input_edit.axes.erase(input_edit.axes.begin() + static_cast<std::ptrdiff_t>(*remove_axis));
            cancel_binding_capture();
            input_name_buffers.clear();
            changed = true;
        }
        if (changed) save_input_edit();
    }

    // Lighting effects. Each checkbox saves through graphics.set_settings; the status under it
    // comes from the renderer, which may not support an effect on this GPU.
    void draw_graphics_page() {
        const auto& palette = editor_palette();
        const auto* status = graphics_status.object();
        const auto* settings_value = status ? field(*status, "settings") : nullptr;
        const auto* settings = settings_value ? settings_value->object() : nullptr;
        const auto* renderer_value = status ? field(*status, "renderer") : nullptr;
        const auto* renderer = renderer_value ? renderer_value->object() : nullptr;
        const auto* project = project_status.object();
        const bool has_project = project && !string_or(*project, "filename").empty();
        if (!has_project)
            ImGui::TextColored(editor_color(palette.warning),
                               "Open a project to save its graphics settings.");
        ImGui::TextColored(editor_color(palette.text_faint), "%s",
                           status && boolean_or(*status, "saved", false)
                               ? "Saved in the project file"
                               : "Defaults until you change something");
        const auto effect = [&](const char* label, const char* key, const char* id,
                                const char* description, const JsonValue::Object* live) {
            bool enabled = settings && boolean_or(*settings, key, true);
            // Without a project the save fails and the status bar says why.
            if (editor_checkbox(label, &enabled)) {
                if (auto saved = call("graphics.set_settings",
                                      std::string{"\""} + key + "\":" + (enabled ? "true" : "false"))) {
                    set_status(std::string(label) + (enabled ? " on" : " off"), false);
                    if (auto refreshed = call("graphics.settings")) graphics_status = std::move(*refreshed);
                }
            }
            note_item(id);
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            ImGui::TextWrapped("%s", description);
            ImGui::PopStyleColor();
            if (live) {
                const auto error = string_or(*live, "error");
                if (boolean_or(*live, "active", false))
                    ImGui::TextColored(editor_color(palette.success), "Running");
                else if (!error.empty())
                    ImGui::TextColored(editor_color(palette.warning), "Unavailable: %s",
                                       error.c_str());
                else if (!enabled)
                    ImGui::TextColored(editor_color(palette.text_faint), "Off");
                else
                    ImGui::TextColored(editor_color(palette.text_faint),
                                       "Starts with the next frame of a scene");
            }
            ImGui::Unindent();
        };
        ImGui::SeparatorText("Lighting");
        const auto* gi_value = renderer ? field(*renderer, "global_illumination") : nullptr;
        effect("Global illumination", "global_illumination", "config:graphics:global_illumination",
               "Light bounces between surfaces, and rough surfaces reflect their surroundings. "
               "Uses AMD FidelityFX Brixelizer GI and replaces the flat sky light on opaque "
               "surfaces.",
               gi_value ? gi_value->object() : nullptr);
        const auto* reflections_value = renderer ? field(*renderer, "reflections") : nullptr;
        effect("Ray traced reflections", "reflections", "config:graphics:reflections",
               "Sharp reflections on smooth surfaces, traced with hardware ray tracing and "
               "denoised with AMD FidelityFX.",
               reflections_value ? reflections_value->object() : nullptr);
        if (!renderer)
            ImGui::TextColored(editor_color(palette.text_faint),
                               "No GPU renderer is attached, so availability is unknown.");

        ImGui::SeparatorText("Frame rate");
        bool vsync = settings && boolean_or(*settings, "vsync", false);
        if (editor_checkbox("Vsync", &vsync)) {
            if (call("graphics.set_settings", std::string("\"vsync\":") + (vsync ? "true" : "false"))) {
                set_status(vsync ? "Vsync on" : "Vsync off", false);
                if (auto refreshed = call("graphics.settings")) graphics_status = std::move(*refreshed);
            }
        }
        note_item("config:graphics:vsync");
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("Shows each frame in step with the display's refresh, so the frame rate "
                           "never exceeds it. Off shows frames as soon as they are ready: faster and "
                           "more responsive, but the image can tear.");
        ImGui::PopStyleColor();
        const auto* presentation_value = renderer ? field(*renderer, "presentation") : nullptr;
        if (const auto* presentation = presentation_value ? presentation_value->object() : nullptr) {
            const auto mode = string_or(*presentation, "mode");
            if (boolean_or(*presentation, "vsync_requested", false) != vsync)
                ImGui::TextColored(editor_color(palette.text_faint), "Applies with the next frame");
            else if (mode == "immediate")
                ImGui::TextColored(editor_color(palette.success), "Running without vsync");
            else if (mode == "mailbox")
                ImGui::TextColored(editor_color(palette.success),
                                   "Running without vsync (mailbox: no tearing, but frames the "
                                   "display misses are dropped)");
            else if (!vsync)
                ImGui::TextColored(editor_color(palette.warning),
                                   "Unavailable: this display only presents with vsync");
            else
                ImGui::TextColored(editor_color(palette.success), "Running with vsync");
        }
        ImGui::Unindent();
        const int limit = settings ? static_cast<int>(number_or(*settings, "frame_rate_limit", 0.0)) : 0;
        const auto save_limit = [&](const int value) {
            const auto clamped = std::clamp(value, 0, static_cast<int>(maximum_frame_rate_limit));
            if (call("graphics.set_settings", "\"frame_rate_limit\":" + std::to_string(clamped))) {
                set_status(clamped == 0 ? std::string("Frame rate unlimited")
                                        : "Frame rate limited to " + std::to_string(clamped) + " FPS",
                           false);
                if (auto refreshed = call("graphics.settings")) graphics_status = std::move(*refreshed);
            }
        };
        if (!frame_rate_limit_editing) frame_rate_limit_edit = limit;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Frame rate limit");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0F * ui_scale);
        ImGui::InputInt("##frame_rate_limit", &frame_rate_limit_edit, 0, 0);
        note_item("config:graphics:frame_rate_limit");
        frame_rate_limit_editing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() && frame_rate_limit_edit != limit)
            save_limit(frame_rate_limit_edit);
        ImGui::SameLine();
        ImGui::TextColored(editor_color(palette.text_dim), "FPS");
        for (const int preset : {0, 30, 60, 120, 144, 240}) {
            ImGui::SameLine();
            const auto label = preset == 0 ? std::string("Unlimited") : std::to_string(preset);
            const bool current = preset == limit;
            if (current) ImGui::PushStyleColor(ImGuiCol_Button, palette.accent);
            if (ImGui::SmallButton(label.c_str()) && !current) save_limit(preset);
            if (current) ImGui::PopStyleColor();
            note_item("config:graphics:frame_rate_limit:" + std::to_string(preset));
        }
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("The most frames per second while the game runs; 0 is unlimited. With vsync "
                           "on, the display's refresh rate is the ceiling either way. The editor "
                           "itself draws at most 250 frames per second.");
        ImGui::PopStyleColor();
        ImGui::Unindent();
    }

    // Mixer buses: a tree under Master with volume, mute, solo and live meters.
    void draw_audio_page() {
        const auto& palette = editor_palette();
        const auto* project = project_status.object();
        const bool has_project = project && !string_or(*project, "filename").empty();
        const auto* status = audio_status.object();
        const auto* output_value = status ? field(*status, "output") : nullptr;
        const auto* output = output_value ? output_value->object() : nullptr;
        ImGui::SeparatorText("Output");
        if (output && boolean_or(*output, "open", false))
            ImGui::TextColored(editor_color(palette.success), "%s (%s, %d Hz)",
                               string_or(*output, "device").c_str(),
                               string_or(*output, "backend").c_str(),
                               static_cast<int>(number_or(*output, "sample_rate", 0.0)));
        else if (output && !string_or(*output, "error").empty())
            ImGui::TextColored(editor_color(palette.warning), "No sound: %s",
                               string_or(*output, "error").c_str());
        else
            ImGui::TextColored(editor_color(palette.text_faint),
                               "Sound output is off (RELAY_AUDIO=0 or a headless session)");

        const auto* shown_settings = audio_settings.object();
        const auto* saved_settings = shown_settings ? field(*shown_settings, "settings") : nullptr;
        const bool binaural = saved_settings && saved_settings->object() &&
                              string_or(*saved_settings->object(), "spatialization", "stereo") == "binaural";
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Listening on");
        ImGui::SameLine();
        if (ImGui::RadioButton("Speakers", !binaural) && binaural &&
            call("audio.set_spatialization", "\"mode\":\"stereo\"")) {
            set_status("Positioned sounds pan for speakers", false);
            refresh_audio_files();
        }
        note_item("config:audio:speakers");
        ImGui::SameLine();
        if (ImGui::RadioButton("Headphones", binaural) && !binaural &&
            call("audio.set_spatialization", "\"mode\":\"binaural\"")) {
            set_status("Positioned sounds use the head model for headphones", false);
            refresh_audio_files();
        }
        note_item("config:audio:headphones");
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("Headphones gives each ear its own delay and head shadow, so sounds sit "
                           "left, right and behind you more clearly. It is a head model, not a "
                           "measured HRTF, so height is only faintly placed.");
        ImGui::PopStyleColor();
        ImGui::Unindent();

        ImGui::SeparatorText("Mixer buses");
        if (ImGui::Button("Open the Mixer")) {
            panel_open[10] = true;
            ImGui::SetWindowFocus("Mixer");
        }
        note_item("config:audio:open_mixer");
        ImGui::SameLine();
        ImGui::TextColored(editor_color(palette.text_dim), "for faders, meters and effects");
        if (!has_project)
            ImGui::TextColored(editor_color(palette.warning), "Open a project to save its mixer.");
        // Buttons below save and re-read audio_settings mid-loop, so the page draws from a copy.
        const JsonValue shown = audio_settings;
        const auto* object = shown.object();
        ImGui::TextColored(editor_color(palette.text_faint), "%s",
                           object && boolean_or(*object, "saved", false)
                               ? "Saved in the project file"
                               : "Defaults until you change something");
        const auto* settings = object ? field(*object, "settings") : nullptr;
        const auto* buses = settings && settings->object() ? field(*settings->object(), "buses") : nullptr;
        const auto* levels = status ? field(*status, "buses") : nullptr;
        const auto level_of = [&](const std::string& name, const char* key) {
            if (levels && levels->array())
                for (const auto& bus : *levels->array())
                    if (const auto* entry = bus.object(); entry && string_or(*entry, "name") == name)
                        return number_or(*entry, key, minimum_audio_volume_db);
            return minimum_audio_volume_db;
        };
        const auto save = [&](const std::string& fields, const std::string& message,
                              const char* method = "audio.set_bus") {
            if (call(method, fields)) {
                set_status(message, false);
                refresh_audio_files();
            }
        };
        const auto names = audio_bus_names();
        if (!buses || !buses->array()) return;
        // Depth for indenting the tree; parents are listed before children.
        std::map<std::string, int, std::less<>> depth;
        constexpr ImGuiTableFlags table_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                                ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##buses", 6, table_flags)) {
            ImGui::TableSetupColumn("Bus", ImGuiTableColumnFlags_WidthStretch, 1.4F);
            ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthStretch, 1.0F);
            ImGui::TableSetupColumn("Volume", ImGuiTableColumnFlags_WidthStretch, 1.2F);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64.0F * ui_scale);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthStretch, 1.4F);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24.0F * ui_scale);
            ImGui::TableHeadersRow();
            for (const auto& value : *buses->array()) {
                const auto* bus = value.object();
                if (!bus) continue;
                const auto name = string_or(*bus, "name");
                const auto parent = string_or(*bus, "parent");
                const bool master = parent.empty();
                const int level = master ? 0 : depth[parent] + 1;
                depth[name] = level;
                const auto quoted = "\"name\":\"" + json_escape(name) + '"';
                ImGui::PushID(name.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Indent(static_cast<float>(level) * 14.0F * ui_scale + 0.001F);
                if (bus_rename_target == name) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
                    const bool entered = ImGui::InputText("##rename", bus_rename.data(), bus_rename.size(),
                                                          ImGuiInputTextFlags_EnterReturnsTrue);
                    if (entered || ImGui::IsItemDeactivated()) {
                        const std::string renamed{bus_rename.data()};
                        if (entered && !renamed.empty() && renamed != name)
                            save(quoted + ",\"new_name\":\"" + json_escape(renamed) + '"',
                                 "Bus renamed to " + renamed);
                        bus_rename_target.clear();
                    }
                } else {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(name.c_str());
                    if (!master && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        bus_rename_target = name;
                        bus_rename.fill('\0');
                        std::copy_n(name.begin(), std::min(name.size(), bus_rename.size() - 1U), bus_rename.begin());
                    }
                    if (!master && ImGui::IsItemHovered()) ImGui::SetTooltip("Double-click to rename");
                }
                ImGui::Unindent(static_cast<float>(level) * 14.0F * ui_scale + 0.001F);

                ImGui::TableNextColumn();
                if (master) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginCombo("##parent", parent.c_str())) {
                        for (const auto& option : names)
                            if (option != name && ImGui::Selectable(option.c_str(), option == parent))
                                save(quoted + ",\"parent\":\"" + json_escape(option) + '"',
                                     name + " now feeds " + option);
                        ImGui::EndCombo();
                    }
                }

                ImGui::TableNextColumn();
                const auto volume = number_or(*bus, "volume_db", 0.0);
                if (bus_volume_target != name) bus_volume_edit = volume;
                double edit = bus_volume_target == name ? bus_volume_edit : volume;
                const double low = minimum_audio_volume_db, high = maximum_audio_volume_db;
                ImGui::SetNextItemWidth(-FLT_MIN);
                (void)editor_slider("##volume", &edit, low, high, edit <= low ? "-inf dB" : "%.1f dB");
                if (ImGui::IsItemActivated()) bus_volume_target = name;
                if (bus_volume_target == name) bus_volume_edit = edit;
                // Saved once when the drag ends, since each save rewrites the project file.
                if (ImGui::IsItemDeactivated() && bus_volume_target == name) {
                    if (bus_volume_edit != volume)
                        save(quoted + ",\"volume_db\":" + number_text(bus_volume_edit),
                             name + " volume set");
                    bus_volume_target.clear();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag, or click the value to type one");

                ImGui::TableNextColumn();
                const auto toggle = [&](const char* label, const char* key, const ImU32 on_color) {
                    const bool on = boolean_or(*bus, key, false);
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button, on_color);
                    if (ImGui::SmallButton(label))
                        save(quoted + ",\"" + key + "\":" + (on ? "false" : "true"),
                             name + (std::string_view(key) == "mute" ? (on ? " unmuted" : " muted")
                                                                     : (on ? " unsoloed" : " soloed")));
                    if (on) ImGui::PopStyleColor();
                };
                toggle("M", "mute", palette.danger);
                note_item("config:audio:bus:" + name + ":mute");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute");
                ImGui::SameLine(0.0F, 4.0F * ui_scale);
                toggle("S", "solo", palette.warning);
                note_item("config:audio:bus:" + name + ":solo");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo: hear only soloed buses");

                ImGui::TableNextColumn();
                draw_level_meter(level_of(name, "peak_left_db"), level_of(name, "peak_right_db"));

                ImGui::TableNextColumn();
                if (!master) {
                    if (ImGui::SmallButton("x"))
                        save(quoted, "Removed bus " + name, "audio.remove_bus");
                    note_item("config:audio:bus:" + name + ":remove");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Remove; its children move up to %s", parent.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SetNextItemWidth(200.0F * ui_scale);
        const bool entered = ImGui::InputTextWithHint("##new_bus", "New bus name", new_bus_name.data(),
                                                      new_bus_name.size(),
                                                      ImGuiInputTextFlags_EnterReturnsTrue);
        note_item("config:audio:new_bus");
        ImGui::SameLine();
        const std::string requested{new_bus_name.data()};
        ImGui::BeginDisabled(requested.empty());
        const bool add = ImGui::Button("Add bus");
        note_item("config:audio:add_bus");
        if ((add || entered) && !requested.empty()) {
            save("\"name\":\"" + json_escape(requested) + '"', "Added bus " + requested);
            new_bus_name.fill('\0');
        }
        ImGui::EndDisabled();
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
        ImGui::TextWrapped("Sources choose a bus in the Inspector. Each bus feeds its parent, so "
                           "turning down SFX turns down everything beneath it. Meters show levels "
                           "while sounds play.");
        ImGui::PopStyleColor();
        ImGui::Unindent();
    }

    // Two thin bars, left over right, from -60 dB to 0 dB.
    void draw_level_meter(const double left_db, const double right_db) {
        const auto& palette = editor_palette();
        const float width = ImGui::GetContentRegionAvail().x;
        const float bar = 5.0F * ui_scale;
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(width, bar * 2.0F + 2.0F * ui_scale));
        auto* draw = ImGui::GetWindowDrawList();
        const auto draw_bar = [&](const float y, const double db) {
            const float fill = static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
            draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + bar), palette.input);
            const ImU32 color = db > -1.0 ? palette.danger : db > -12.0 ? palette.warning : palette.success;
            if (fill > 0.0F)
                draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width * fill, y + bar), color);
        };
        draw_bar(origin.y, left_db);
        draw_bar(origin.y + bar + 2.0F * ui_scale, right_db);
    }

    // Project-wide game settings, one page per area. Input is the first; others follow.
    void draw_game_config() {
        if (!game_config_open) {
            if (binding_capture.active) cancel_binding_capture();
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(900.0F * ui_scale, 680.0F * ui_scale), ImGuiCond_FirstUseEver);
        const bool visible = ImGui::Begin("Game Configuration", &game_config_open);
        note_item("config:window");
        if (!visible) {
            ImGui::End();
            return;
        }
        if (ImGui::BeginChild("##config_pages", ImVec2(150.0F * ui_scale, 0.0F), ImGuiChildFlags_Borders)) {
            if (ImGui::Selectable("Input", game_config_page == 0)) game_config_page = 0;
            note_item("config:page:input");
            if (ImGui::Selectable("Graphics", game_config_page == 1)) {
                game_config_page = 1;
                if (auto settings = call("graphics.settings")) graphics_status = std::move(*settings);
            }
            note_item("config:page:graphics");
            if (ImGui::Selectable("Audio", game_config_page == 2)) {
                game_config_page = 2;
                refresh_audio_files();
                refresh_audio_status();
            }
            note_item("config:page:audio");
            for (const char* page : {"Physics"}) {
                ImGui::BeginDisabled();
                ImGui::Selectable(page);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Coming soon");
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##config_page", ImVec2(0.0F, 0.0F))) {
            if (game_config_page == 1)
                draw_graphics_page();
            else if (game_config_page == 2)
                draw_audio_page();
            else
                draw_input_page();
        }
        ImGui::EndChild();
        ImGui::End();
    }


    void draw_template_dialog() {
        if (open_template_dialog) {
            ImGui::OpenPopup("Save as template");
            open_template_dialog = false;
        }
        if (!ImGui::BeginPopupModal("Save as template", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextUnformatted("Template name");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputText("##template_name", template_name.data(),
                                              template_name.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        note_item("dialog:template:name");
        ImGui::TextDisabled("Saves the node and its children to templates/, for the Add Node window.");
        editor_checkbox("Replace an existing template", &template_replace);
        const std::string name = template_name.data();
        ImGui::BeginDisabled(name.empty());
        const bool save = ImGui::Button("Save") || entered;
        note_item("dialog:template:save");
        if (save && !name.empty()) {
            if (call("templates.save", entity_field(template_save_entity) + ",\"name\":\"" +
                                           json_escape(name) + "\",\"replace\":" +
                                           (template_replace ? "true" : "false"))) {
                set_status("Saved template " + name, false);
                assets_pending = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void draw_dialogs() {
        draw_script_dialogs();
        draw_template_dialog();
        draw_add_component_window();
        draw_add_node_window();
        if (open_discard_dialog) {
            ImGui::OpenPopup("Unsaved changes");
            open_discard_dialog = false;
        }
        if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("%s has unsaved changes.", scene_has_file ? scene_filename.data()
                                                                 : "This untitled scene");
            const auto action = pending_action;
            const char* verb = action == PendingAction::quit      ? "Quit anyway"
                               : action == PendingAction::open_scene ? "Open anyway"
                                                                     : "Discard and continue";
            // Enter takes the safe branch and only when there is a file to take it to. Leaving a
            // prompt that can only be answered with the mouse strands anyone who reached it from
            // a keyboard shortcut, but defaulting to discarding unsaved work would be worse.
            const bool confirmed = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            if (scene_has_file) {
                const bool save_now = ImGui::Button("Save and continue");
                ImGui::SetItemDefaultFocus();
                if ((save_now || confirmed) && save_scene()) {
                    ImGui::CloseCurrentPopup();
                    run_pending_action(action);
                }
                ImGui::SameLine();
            } else {
                ImGui::TextDisabled("Use File > Save scene as... first to keep it.");
            }
            if (ImGui::Button(verb)) {
                ImGui::CloseCurrentPopup();
                run_pending_action(action);
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                pending_action = PendingAction::none;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (open_file_dialog) {
            ImGui::OpenPopup("Project action");
            open_file_dialog = false;
        }
        if (ImGui::BeginPopupModal("Project action", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const bool scene_action =
                file_action == FileAction::open || file_action == FileAction::save_as;
            const bool project_action = file_action == FileAction::new_project || file_action == FileAction::open_project;
            const char* title = file_action == FileAction::new_project ? "New project"
                                : file_action == FileAction::open_project ? "Open project"
                                : file_action == FileAction::add_project_scene ? "Add existing scene to project"
                                : file_action == FileAction::open         ? "Open scene"
                                : file_action == FileAction::save_as    ? "Save scene as"
                                : file_action == FileAction::import     ? "Import model"
                                : file_action == FileAction::screenshot ? "Capture GPU screenshot"
                                                                        : "Record GPU WebM";
            ImGui::TextUnformatted(title);
            const auto* project = active_project();
            const auto root = project ? string_or(*project, "root") : std::string{};
            const auto directory = (scene_action || file_action == FileAction::add_project_scene)
                ? (root.empty() ? "scenes/" : root + "/scenes/")
                : project_action ? std::string("workspace (relative path)")
                : file_action == FileAction::import ? assets_root : std::string("captures/");
            ImGui::TextDisabled("Folder: %s", directory.c_str());
            ImGui::SetNextItemWidth(360.0F * ui_scale);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            const bool submitted =
                ImGui::InputText("Filename", action_filename.data(), action_filename.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            if (file_action == FileAction::new_project)
                ImGui::InputText("Project name", project_name.data(), project_name.size());
            if (project_action) ImGui::TextDisabled("Use a workspace-relative path, e.g. projects/my-project/project.relayproject.");
            if (file_action == FileAction::screenshot || file_action == FileAction::recording)
                ImGui::TextDisabled("Source: real Vulkan GPU; editor panels are excluded.");
            if (!dialog_error.empty())
                ImGui::TextWrapped("%s", dialog_error.c_str());
            if (file_action == FileAction::recording)
                ImGui::TextDisabled("30 fps, up to 10 seconds; stop early from Tools.");
            if (file_action == FileAction::recording && runtime_status.object() &&
                boolean_or(*runtime_status.object(), "paused", false))
                ImGui::TextWrapped(
                    "Simulation is paused. Resume from Run or step frames to feed the recording.");
            if (ImGui::Button("Continue") || submitted) {
                const auto filename = json_escape(action_filename.data());
                bool success = false;
                if (project_action) {
                    success = open_or_create_project(file_action == FileAction::new_project, action_filename.data());
                } else if (file_action == FileAction::add_project_scene) {
                    success = mutate("project.add_scene", "\"scene_file\":\"" + filename + '\"', "Scene added to project");
                } else if (scene_action) {
                    success = open_or_save_scene(file_action == FileAction::open,
                                                 action_filename.data());
                } else if (file_action == FileAction::import) {
                    import_model(action_filename.data());
                    success = !status_is_error;
                } else if (file_action == FileAction::screenshot) {
                    success = defer_action("render.capture_async",
                                     "\"path\":\"" + filename + "\",\"source\":\"vulkan\"",
                                     "GPU screenshot queued; source=vulkan");
                } else {
                    success =
                        defer_action("video.start",
                               "\"filename\":\"" + filename +
                                   "\",\"source\":\"vulkan\",\"fps\":30,\"maximum_frames\":300",
                               "GPU WebM recording started; source=vulkan");
                }
                if (success)
                    ImGui::CloseCurrentPopup();
                else
                    dialog_error = status_message;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (show_help) {
            if (ImGui::Begin("Editor controls", &show_help, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    "MMB: orbit | Shift+MMB: pan | Wheel: zoom\nRMB / Shift+F: freelook | WASD/QE: "
                    "fly\nShift: faster | Alt: slower | Escape: leave freelook\nF: frame selection "
                    "| W/E/R: move/rotate/scale\nCtrl+Z: undo | Ctrl+Shift+Z: redo\nCtrl+D: "
                    "duplicate | Delete: delete selection\nCtrl+A: select all | Ctrl+C/X/V: copy/cut/paste\nCtrl/Shift+click: toggle/range selection\nCtrl+N: new scene | Ctrl+O: open | "
                    "Ctrl+S: save | Ctrl+Shift+S: save as\nDrag panel tabs to dock; Shift-drag "
                    "to float.\nF5: run game | F8: stop game.");
            }
            ImGui::End();
        }
        if (show_about) {
            if (ImGui::Begin("About Relay", &show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    "Relay Engine\nA shared creative workspace for people and agents.");
                ImGui::Separator();
                ImGui::TextDisabled("Experimental Linux/Vulkan editor.\nDisabled menu items mark "
                                    "planned features.");
            }
            ImGui::End();
        }
    }

    void draw_toolbar() {
        const float row_y = ImGui::GetCursorScreenPos().y;
        ImGui::BeginDisabled(undo_labels.empty());
        if (toolbar_button("##undo", ToolIcon::undo, false,
                           undo_labels.empty() ? "Nothing to undo" : undo_labels.front().c_str()))
            mutate("scene.undo", {}, "Undone");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(redo_labels.empty());
        if (toolbar_button("##redo", ToolIcon::redo, false,
                           redo_labels.empty() ? "Nothing to redo" : redo_labels.front().c_str()))
            mutate("scene.redo", {}, "Redone");
        ImGui::EndDisabled();
        toolbar_divider();
        if (toolbar_button("##camera", ToolIcon::camera, camera_enabled,
                           "Toggle editor view / scene camera"))
            camera_enabled = !camera_enabled;
        ImGui::BeginDisabled(!camera_enabled);
        if (toolbar_button("##frame", ToolIcon::focus, false, "Frame selection (F)"))
            focus_selection();
        toolbar_divider();
        if (toolbar_button("##move", ToolIcon::move, gizmo_operation == ImGuizmo::TRANSLATE,
                           "Move (W)"))
            gizmo_operation = ImGuizmo::TRANSLATE;
        if (toolbar_button("##rotate", ToolIcon::rotate, gizmo_operation == ImGuizmo::ROTATE,
                           "Rotate (E)"))
            gizmo_operation = ImGuizmo::ROTATE;
        if (toolbar_button("##scale", ToolIcon::scale, gizmo_operation == ImGuizmo::SCALE,
                           "Scale (R)"))
            gizmo_operation = ImGuizmo::SCALE;
        if (toolbar_button("##orientation",
                           gizmo_mode == ImGuizmo::LOCAL ? ToolIcon::local : ToolIcon::world, false,
                           gizmo_mode == ImGuizmo::LOCAL ? "Local axes: switch to world"
                                                         : "World axes: switch to local"))
            gizmo_mode = gizmo_mode == ImGuizmo::LOCAL ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
        ImGui::EndDisabled();

        const float left_end = ImGui::GetCursorScreenPos().x;
        const float button_width = 32.0F * ui_scale;
        const float group_width = 3.0F * button_width +
                                  2.0F * ImGui::GetStyle().ItemSpacing.x;
        const float toolbar_center = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x * 0.5F;
        ImGui::SetCursorScreenPos({std::max(toolbar_center - group_width * 0.5F, left_end), row_y});
        const auto* status = runtime_status.object();
        const bool game = status && string_or(*status, "mode") == "game";
        const bool paused = status && boolean_or(*status, "paused", false);
        const bool run_pressed = toolbar_button("##run_game", game ? ToolIcon::stop : ToolIcon::play,
                                                game, game ? "Stop Game (F8)" : "Run Game (F5)");
        note_item("toolbar:run");
        if (run_pressed) {
            if (game) mutate("runtime.stop", {}, "Game stopped");
            else request_play();
        }
        ImGui::BeginDisabled(!game);
        if (toolbar_button("##simulation", paused ? ToolIcon::play : ToolIcon::pause, paused,
                           paused ? "Resume simulation" : "Pause simulation"))
            mutate(paused ? "runtime.resume" : "runtime.pause", {}, paused ? "Resumed" : "Paused");
        if (toolbar_button("##step", ToolIcon::step, false, "Advance exactly one game frame"))
            mutate("runtime.step", "\"frames\":1", "Stepped one frame");
        ImGui::EndDisabled();
        ImGui::NewLine();
    }

    void draw_viewport_fps() {
        if (!viewport_visible || !viewport_draw_list) return;
        char label[32];
        std::snprintf(label, sizeof(label), "%.0f FPS", ImGui::GetIO().Framerate);
        const auto size = ImGui::CalcTextSize(label);
        const float inset = 10.0F * ui_scale;
        const ImVec2 at{viewport_max.x - size.x - inset, viewport_min.y + inset};
        if (at.x < viewport_min.x + inset || at.y + size.y > viewport_max.y - inset) return;
        viewport_draw_list->PushClipRect(viewport_min, viewport_max, true);
        viewport_draw_list->AddText({at.x + 1.0F, at.y + 1.0F}, IM_COL32(0, 0, 0, 180), label);
        viewport_draw_list->AddText(at, IM_COL32(255, 255, 255, 220), label);
        viewport_draw_list->PopClipRect();
    }

    // Keyboard shortcuts, ignored whenever a text field has focus so typing a name never switches
    // the gizmo or deletes the selection.
    void update_shortcuts() {
        // The shader editor takes typing, Ctrl+S and Ctrl+Z itself; the Asset Browser takes arrows,
        // Enter, Backspace and Ctrl+F, and Delete there must not delete scene nodes.
        if (shader_editor_focused || browser.focused) return;
        if (ImGui::GetIO().WantTextInput || (ImGui::GetActiveID() != 0 && ImGui::GetInputTextState(ImGui::GetActiveID())) || navigating ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            return;
        const auto& shortcuts = ImGui::GetIO();
        if (update_asset_shortcuts()) return;
        if (hierarchy_focused && shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            hierarchy_search_focus = true;
            return;
        }
        if (!assets_focused && ImGui::IsKeyPressed(ImGuiKey_F2, false) && panel_open[0] &&
            selections.handles.size() == 1U) {
            if (const auto* entity = find_entity(selection))
                begin_rename(RenameKind::entity, selection, string_or(*entity, "name"));
        }
        if (shortcuts.KeyCtrl && shortcuts.KeyShift && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            panel_open[8] = true; agent_expand_pending = true; return;
        }
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            if (shortcuts.KeyShift) file_dialog(FileAction::save_as, scene_filename.data());
            else (void)save_scene();
        }
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
            discarding_action(PendingAction::open_scene);
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false))
            discarding_action(PendingAction::new_scene);
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !selection.empty())
            duplicate_selection();
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            select({});
            for (const auto* entity : entities)
                selections.click(string_or(*entity, "entity"), true, false);
        }
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !selection.empty())
            group_operation("scene.copy", "Copied selection");
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false) && !selection.empty())
            group_operation("scene.cut", "Cut selection");
        if (shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false) && clipboard_ready)
            group_operation("scene.paste", "Pasted selection", false);
        if (!shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_operation = ImGuizmo::TRANSLATE;
        if (!shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false))
            gizmo_operation = ImGuizmo::ROTATE;
        if (!shortcuts.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_R, false))
            gizmo_operation = ImGuizmo::SCALE;
        if (!shortcuts.KeyCtrl && !ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
            focus_selection();
        if (!assets_focused && ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selection.empty()) {
            group_operation("scene.destroy_many", "Deleted selection");
        }
        const auto& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            mutate(io.KeyShift ? "scene.redo" : "scene.undo", {},
                   io.KeyShift ? "Redone" : "Undone");
        }
    }

    // Asset browser: a tree over the project folder, backed by the assets.* file methods. Only the
    // root and expanded folders are listed.
    static std::string parent_path_of(const std::string& path) {
        const auto slash = path.rfind('/');
        return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    }
    static std::string join_path(const std::string& directory, const std::string& name) {
        return directory.empty() ? name : directory + '/' + name;
    }
    static std::string base_name(const std::string& path) {
        const auto slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1U);
    }
    static bool within(const std::string& path, const std::string& folder) {
        return path == folder || path.starts_with(folder + '/');
    }

    [[nodiscard]] const AssetEntry* asset_entry(const std::string& path) const {
        if (const auto folder = asset_folders.find(parent_path_of(path)); folder != asset_folders.end())
            for (const auto& entry : folder->second)
                if (entry.path == path) return &entry;
        for (const auto& entry : asset_results)
            if (entry.path == path) return &entry;
        return nullptr;
    }

    [[nodiscard]] bool asset_search_active() const {
        return asset_query[0] != '\0' || !asset_kind_filters.empty();
    }

    void run_asset_search() {
        asset_results.clear();
        asset_results_truncated = false;
        if (!asset_search_active()) return;
        std::string kinds = "[";
        for (const auto& kind : asset_kind_filters) {
            if (kinds.size() > 1) kinds += ',';
            kinds += '"' + kind + '"';
        }
        const auto found = call("assets.search", "\"query\":\"" + json_escape(asset_query.data()) +
                                                     "\",\"kinds\":" + kinds + ']', false);
        const auto* object = found ? found->object() : nullptr;
        if (!object) return;
        asset_results_truncated = boolean_or(*object, "truncated", false);
        if (const auto* values = field(*object, "entries"); values && values->array())
            for (const auto& value : *values->array())
                if (const auto* entry = value.object())
                    asset_results.push_back({string_or(*entry, "name"), string_or(*entry, "path"),
                                             string_or(*entry, "kind"),
                                             string_or(*entry, "type") == "folder",
                                             boolean_or(*entry, "importable", false),
                                             boolean_or(*entry, "protected", false)});
    }

    void clear_asset_search() {
        asset_query.fill('\0');
        asset_kind_filters.clear();
        asset_results.clear();
    }

    // Leaves the search and shows `path` in the tree, expanding its folders.
    void reveal_asset(const std::string& path) {
        clear_asset_search();
        for (auto folder = parent_path_of(path); !folder.empty(); folder = parent_path_of(folder))
            expanded_asset_folders.insert(folder);
        if (asset_entry(path) == nullptr || asset_entry(path)->folder)
            expanded_asset_folders.insert(path);
        refresh_asset_listing();
        asset_selection = path;
    }

    // Lists one folder. Returns false when it no longer exists.
    bool list_asset_folder(const std::string& directory) {
        const auto listing = call("assets.browse",
                                  "\"directory\":\"" + json_escape(directory) + '"', false);
        const auto* object = listing ? listing->object() : nullptr;
        if (!object) {
            asset_folders.erase(directory);
            return false;
        }
        if (directory.empty()) {
            assets_root = string_or(*object, "root", assets_root);
            thumbnails.set_root(assets_root);
        }
        asset_listing_truncated |= boolean_or(*object, "truncated", false);
        auto& entries = asset_folders[directory];
        entries.clear();
        if (const auto* values = field(*object, "entries"); values && values->array())
            for (const auto& value : *values->array())
                if (const auto* entry = value.object())
                    entries.push_back({string_or(*entry, "name"), string_or(*entry, "path"),
                                       string_or(*entry, "kind"),
                                       string_or(*entry, "type") == "folder",
                                       boolean_or(*entry, "importable", false),
                                       boolean_or(*entry, "protected", false)});
        return true;
    }

    // Lists and opens every folder below the root, breadth first, up to 256 folders so a huge
    // project cannot stall the editor.
    void expand_all_asset_folders() {
        std::deque<std::string> pending{std::string{}};
        std::size_t opened = 0;
        while (!pending.empty() && opened < 256U) {
            const auto folder = pending.front();
            pending.pop_front();
            if (!folder.empty()) {
                if (!list_asset_folder(folder)) continue;
                expanded_asset_folders.insert(folder);
                ++opened;
            }
            if (const auto found = asset_folders.find(folder); found != asset_folders.end())
                for (const auto& entry : found->second)
                    if (entry.folder) pending.push_back(entry.path);
        }
    }

    void refresh_asset_listing() {
        asset_listing_truncated = false;
        asset_folders.clear();
        list_asset_folder({});
        // Parents come before children in the ordered set, so a vanished folder's descendants
        // are dropped when their parent listing no longer contains them.
        for (auto folder = expanded_asset_folders.begin(); folder != expanded_asset_folders.end();) {
            const auto* entry = asset_entry(*folder);
            if (entry && entry->folder && list_asset_folder(*folder)) ++folder;
            else folder = expanded_asset_folders.erase(folder);
        }
        run_asset_search();
        if (!asset_selection.empty() && !asset_entry(asset_selection)) asset_selection.clear();
        if (inline_rename.kind == RenameKind::asset && !asset_entry(inline_rename.target))
            inline_rename = {};
    }

    void set_asset_folder_open(const std::string& folder, const bool open) {
        if (open == expanded_asset_folders.contains(folder)) return;
        if (open) {
            expanded_asset_folders.insert(folder);
            list_asset_folder(folder);
        } else {
            std::erase_if(expanded_asset_folders,
                          [&](const std::string& path) { return within(path, folder); });
        }
    }

    // After a move or rename, expanded folders and the selection follow the entry's new path.
    void follow_asset_move(const std::string& from, const std::string& to) {
        std::set<std::string> expanded;
        for (const auto& path : expanded_asset_folders)
            expanded.insert(within(path, from) ? to + path.substr(from.size()) : path);
        expanded_asset_folders = std::move(expanded);
        if (within(asset_selection, from)) asset_selection = to + asset_selection.substr(from.size());
    }

    bool move_asset(const std::string& from, const std::string& to) {
        if (!call("assets.move", "\"from\":\"" + json_escape(from) + "\",\"to\":\"" +
                                     json_escape(to) + '"'))
            return false;
        follow_asset_move(from, to);
        refresh_asset_listing();
        return true;
    }

    void move_asset_into(const std::string& path, const std::string& folder) {
        if (path == folder || parent_path_of(path) == folder || within(folder, path)) return;
        if (move_asset(path, join_path(folder, base_name(path)))) {
            if (!folder.empty()) set_asset_folder_open(folder, true);
            set_status("Moved " + base_name(path) + " to " + (folder.empty() ? "project root" : folder),
                       false);
        }
    }

    void create_asset_folder(const std::string& parent) {
        if (!parent.empty()) set_asset_folder_open(parent, true);
        const auto listed = asset_folders.find(parent);
        const auto taken = [&](const std::string& name) {
            return listed != asset_folders.end() &&
                   std::any_of(listed->second.begin(), listed->second.end(),
                               [&](const AssetEntry& entry) { return entry.name == name; });
        };
        std::string name = "New folder";
        for (int suffix = 2; taken(name); ++suffix) name = "New folder " + std::to_string(suffix);
        const auto path = join_path(parent, name);
        if (!call("assets.create_folder", "\"path\":\"" + json_escape(path) + '"')) return;
        refresh_asset_listing();
        asset_selection = path;
        begin_rename(RenameKind::asset, path, name);
    }

    void open_asset(const AssetEntry& entry) {
        if (entry.folder && asset_search_active()) reveal_asset(entry.path);
        else if (entry.folder) set_asset_folder_open(entry.path, !expanded_asset_folders.contains(entry.path));
        else if (entry.importable) import_model(entry.path);
        else if (entry.kind == "template" && entry.path.starts_with("templates/"))
            instantiate_template("project:" + entry.name.substr(0, entry.name.size() -
                                                                     std::string_view{".relay-template.json"}.size()));
        else if (entry.kind == "audio")
            preview_sound(entry.path);
        else if (entry.kind == "script")
            set_status("Edit " + entry.name + " in your code editor; Relay rebuilds scripts when "
                       "they change", false);
        else if (entry.kind == "shader" && entry.path.ends_with(".relay-shader"))
            open_shader(entry.path);
        else if (entry.kind == "material")
            set_status("Choose " + entry.name + " in a Mesh renderer's Material, a Post Process node "
                       "or a Sky node's Skybox in the Inspector (or drop it there) to edit it",
                       false);
        else set_status("No editor for " + entry.name + " yet", false);
    }

    // Folder targets accept any dragged asset entry and move it inside.
    void asset_folder_drop_target(const std::string& folder) {
        if (!ImGui::BeginDragDropTarget()) return;
        if (const auto* payload = ImGui::AcceptDragDropPayload("relay.asset")) {
            const std::string dragged(static_cast<const char*>(payload->Data));
            move_asset_into(dragged, folder);
        }
        ImGui::EndDragDropTarget();
    }

    void draw_asset_icon(const ImVec2 at, const float size, const AssetEntry& entry) {
        const auto icon = entry.folder               ? AssetIcon::folder
                          : entry.kind == "material" ? asset_icon_for_kind("material", material_facts_of(entry.path).type)
                                                     : asset_icon_for_kind(entry.kind);
        draw_asset_type_icon(ImGui::GetWindowDrawList(), at, size, icon, entry.folder || entry.importable ? 1.0F : 0.85F);
    }

    void draw_asset_context_menu(const AssetEntry& entry) {
        if (!ImGui::BeginPopupContextItem()) return;
        asset_selection = entry.path;
        if (asset_search_active()) {
            if (ImGui::MenuItem("Show in folder")) reveal_asset(entry.path);
            ImGui::Separator();
        }
        if (entry.kind == "audio") {
            if (ImGui::MenuItem("Preview")) preview_sound(entry.path);
            if (ImGui::MenuItem("Stop previews")) (void)call("audio.stop");
            ImGui::Separator();
        }
        if (entry.importable) {
            if (ImGui::MenuItem("Import to scene")) import_model(entry.path);
            if (ImGui::MenuItem("Import to scene as static mesh"))
                import_model(entry.path, "static_mesh");
            ImGui::Separator();
        }
        if (entry.folder) {
            draw_asset_create_menu(entry.path);
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Open in file browser")) open_in_file_browser(entry.path, entry.folder);
        note_item("assets:menu:file_browser");
        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2", false, !entry.locked))
            begin_rename(RenameKind::asset, entry.path, entry.name);
        if (ImGui::MenuItem("Delete", "Del", false, !entry.locked))
            asset_delete_pending = entry.path;
        if (entry.locked) ImGui::TextDisabled("Owned by the project");
        ImGui::EndPopup();
    }

    void draw_asset_create_menu(const std::string& parent) {
        if (!ImGui::BeginMenu("Create")) return;
        if (ImGui::MenuItem("Folder")) create_asset_folder(parent);
        ImGui::Separator();
        if (ImGui::MenuItem("C++ script...")) {
            attach_new_script_to.clear();
            open_new_script_dialog = true;
        }
        if (ImGui::MenuItem("Surface shader")) (void)create_shader(parent, false);
        note_item("assets:create:surface_shader");
        if (ImGui::MenuItem("Post-processing shader")) (void)create_shader(parent, true);
        note_item("assets:create:post_shader");
        if (ImGui::MenuItem("Material")) {
            if (const auto created = create_material(parent, false)) {
                asset_selection = *created;
                begin_rename(RenameKind::asset, *created, base_name(*created));
            }
        }
        note_item("assets:create:material");
        if (ImGui::MenuItem("Post-processing material")) {
            if (const auto created = create_material(parent, true)) {
                asset_selection = *created;
                begin_rename(RenameKind::asset, *created, base_name(*created));
            }
        }
        note_item("assets:create:post_material");
        if (ImGui::MenuItem("Sky material")) create_sky_material(parent);
        note_item("assets:create:sky_material");
        future_action("Text file");
        ImGui::EndMenu();
    }

    // Draws a tree row, or with `flat` a search result row that shows its folder instead of children.
    void draw_asset_node(const AssetEntry& entry, const bool flat = false) {
        ImGui::PushID(entry.path.c_str());
        const bool editing = renaming(RenameKind::asset, entry.path);
        const bool expanded = !flat && entry.folder && expanded_asset_folders.contains(entry.path);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!entry.folder || flat) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (asset_selection == entry.path) flags |= ImGuiTreeNodeFlags_Selected;
        if (entry.folder) ImGui::SetNextItemOpen(expanded, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        const bool open = ImGui::TreeNodeEx("##asset", flags);
        ImGui::PopStyleVar();
        note_item("asset:" + entry.path);
        const auto row_min = ImGui::GetItemRectMin();
        const auto row_max = ImGui::GetItemRectMax();
        if (!editing) {
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                const bool sole = asset_selection == entry.path;
                asset_selection = entry.path;
                note_slow_click(RenameKind::asset, entry.path, sole && !entry.locked);
            }
            const bool activated = (!entry.folder || flat) && ImGui::IsItemHovered() &&
                                   ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("relay.asset", entry.path.c_str(), entry.path.size() + 1U);
                ImGui::Text(entry.importable ? "Import or move %s" : "Move %s", entry.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (entry.folder) asset_folder_drop_target(entry.path);
            draw_asset_context_menu(entry);
            if (activated) open_asset(entry);
        }
        if (!flat && entry.folder && open != expanded) set_asset_folder_open(entry.path, open);

        // The icon and name, or the rename field, sit where the tree node's label would.
        const float icon = ImGui::GetTextLineHeight();
        const float label_x = row_min.x + ImGui::GetTreeNodeToLabelSpacing();
        draw_asset_icon(ImVec2(label_x, row_min.y), icon, entry);
        ImGui::SameLine(0.0F, 0.0F);
        const float text_x = label_x + icon + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetCursorScreenPos(ImVec2(text_x, row_min.y));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        if (editing) {
            const auto result = rename_field("rename:asset", row_max.x - text_x);
            if (result == RenameResult::commit) {
                const std::string renamed = inline_rename.buffer.data();
                const auto destination = join_path(parent_path_of(entry.path), renamed);
                inline_rename = {};
                if (!renamed.empty() && renamed != entry.name && move_asset(entry.path, destination))
                    set_status("Renamed " + entry.name + " to " + renamed, false);
            }
            if (result != RenameResult::editing) inline_rename = {};
        } else if (entry.folder || entry.importable) {
            ImGui::TextUnformatted(entry.name.c_str());
        } else {
            ImGui::TextColored(editor_color(editor_palette().text_dim), "%s", entry.name.c_str());
        }
        if (flat && !editing) {
            const auto folder = parent_path_of(entry.path);
            ImGui::SameLine();
            ImGui::TextColored(editor_color(editor_palette().text_faint), "%s",
                               folder.empty() ? "project root" : folder.c_str());
        }
        ImGui::PopStyleVar();
        if (!flat && entry.folder && open) {
            // Drawn from a copy: a move or rename inside may replace the listing.
            if (const auto listing = asset_folders.find(entry.path); listing != asset_folders.end())
                for (const auto& child : std::vector<AssetEntry>(listing->second))
                    draw_asset_node(child);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void draw_asset_delete_dialog() {
        if (asset_delete_pending.empty()) return;
        if (!ImGui::IsPopupOpen("Delete asset?")) ImGui::OpenPopup("Delete asset?");
        if (!ImGui::BeginPopupModal("Delete asset?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            asset_delete_pending.clear();
            return;
        }
        ImGui::Text("Delete %s?", base_name(asset_delete_pending).c_str());
        ImGui::TextDisabled("It moves to the project's hidden .relay-trash folder.");
        if (ImGui::Button("Delete") || ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            if (call("assets.delete", "\"path\":\"" + json_escape(asset_delete_pending) + '"')) {
                set_status("Deleted " + base_name(asset_delete_pending), false);
                if (within(asset_selection, asset_delete_pending)) asset_selection.clear();
                refresh_asset_listing();
            }
            asset_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            asset_delete_pending.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    static constexpr std::array<std::pair<const char*, const char*>, 12> asset_kind_labels{{
        {"model", "Models"}, {"scene", "Scenes"}, {"template", "Templates"},
        {"image", "Images"}, {"material", "Materials"}, {"shader", "Shaders"},
        {"script", "Scripts"}, {"text", "Text"}, {"audio", "Audio"}, {"media", "Video"},
        {"folder", "Folders"}, {"other", "Other"}}};

    void draw_asset_search_bar() {
        const auto& style = ImGui::GetStyle();
        const float button = ImGui::GetFrameHeight();
        if (asset_search_focus) {
            ImGui::SetKeyboardFocusHere();
            asset_search_focus = false;
        }
        ImGui::SetNextItemWidth(-(button * 2.0F + style.ItemSpacing.x * 2.0F));
        if (ImGui::InputTextWithHint("##asset_search", "Search assets", asset_query.data(),
                                     asset_query.size()))
            run_asset_search();
        note_item("assets:search");
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && asset_query[0]) {
            asset_query.fill('\0');
            run_asset_search();
        }
        ImGui::SameLine();
        const bool any_open = !expanded_asset_folders.empty();
        ImGui::BeginDisabled(asset_search_active());
        if (expand_collapse_button("##asset_expand", any_open)) {
            if (any_open) expanded_asset_folders.clear();
            else expand_all_asset_folders();
        }
        ImGui::EndDisabled();
        note_item("assets:expand_all");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(any_open ? "Collapse all" : "Expand all");
        ImGui::SameLine();
        const bool filtering = !asset_kind_filters.empty();
        if (filter_button("##asset_filter", filtering)) ImGui::OpenPopup("##asset_filters");
        note_item("assets:filter");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(filtering ? "Filter by type (%zu active)" : "Filter by type",
                              asset_kind_filters.size());
        if (ImGui::BeginPopup("##asset_filters")) {
            // Toggling a category keeps the menu open so several can be picked in one go.
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            for (const auto& [kind, label] : asset_kind_labels) {
                bool enabled = asset_kind_filters.contains(kind);
                if (ImGui::MenuItem(label, nullptr, &enabled)) {
                    if (enabled) asset_kind_filters.insert(kind);
                    else asset_kind_filters.erase(kind);
                    run_asset_search();
                }
                note_item(std::string("assets:filter:") + kind);
            }
            ImGui::PopItemFlag();
            ImGui::Separator();
            if (ImGui::MenuItem("Clear filters", nullptr, false, !asset_kind_filters.empty())) {
                asset_kind_filters.clear();
                run_asset_search();
            }
            ImGui::EndPopup();
        }
        // Active filters as removable chips.
        if (!asset_kind_filters.empty()) {
            bool first = true;
            for (const auto& [kind, label] : asset_kind_labels) {
                if (!asset_kind_filters.contains(kind)) continue;
                const std::string chip = std::string(label) + "  x##chip_" + kind;
                const float width = ImGui::CalcTextSize(chip.c_str(), nullptr, true).x +
                                    style.FramePadding.x * 2.0F;
                if (!first && ImGui::GetContentRegionAvail().x > width + style.ItemSpacing.x)
                    ImGui::SameLine();
                first = false;
                if (ImGui::SmallButton(chip.c_str())) {
                    asset_kind_filters.erase(kind);
                    run_asset_search();
                    break;
                }
            }
        }
    }

    void draw_assets() {
        assets_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        draw_asset_search_bar();
        if (begin_region("##asset_files")) {
            const auto root = asset_folders.find({});
            if (asset_search_active()) {
                if (asset_results.empty())
                    ImGui::TextColored(editor_color(editor_palette().text_faint), "No matching assets.");
                for (const auto& entry : std::vector<AssetEntry>(asset_results)) draw_asset_node(entry, true);
                if (asset_results_truncated)
                    ImGui::TextColored(editor_color(editor_palette().text_faint),
                                       "More matches not shown; refine the search.");
            } else if (root == asset_folders.end() || root->second.empty())
                ImGui::TextColored(editor_color(editor_palette().text_faint),
                                   "Empty project. Right-click to create a folder.");
            else
                for (const auto& entry : std::vector<AssetEntry>(root->second)) draw_asset_node(entry);
            // Space below the tree clears the selection, takes drops into the project root, and
            // offers creation there.
            const auto available = ImGui::GetContentRegionAvail();
            ImGui::Dummy(ImVec2(std::max(available.x, 1.0F),
                                std::max(available.y, ImGui::GetFrameHeight())));
            note_item("assets:empty");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) asset_selection.clear();
            asset_folder_drop_target({});
            if (ImGui::BeginPopupContextItem("##assets_empty")) {
                draw_asset_create_menu({});
                if (ImGui::MenuItem("Open in file browser")) open_in_file_browser({}, true);
                if (ImGui::MenuItem("Refresh")) refresh_asset_listing();
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        draw_asset_delete_dialog();
    }

    // Keys for the focused asset browser. Returns true when it consumed the key press.
    bool update_asset_shortcuts() {
        if (!assets_focused) return false;
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            asset_search_focus = true;
            return true;
        }
        const auto* entry = asset_selection.empty() ? nullptr : asset_entry(asset_selection);
        if (!entry) return ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false) && !entry->locked) {
            begin_rename(RenameKind::asset, entry->path, entry->name);
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            if (!entry->locked) asset_delete_pending = entry->path;
            return true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) {
            open_asset(AssetEntry(*entry));
            return true;
        }
        if (entry->folder && (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) ||
                              ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false))) {
            set_asset_folder_open(entry->path, ImGui::IsKeyPressed(ImGuiKey_RightArrow, false));
            return true;
        }
        return false;
    }

    // Asset references. Inspector fields that name an asset show its kind's icon and its name
    // without folders or extension; clicking the field opens the Asset Browser window to choose another.

    // Something to choose that is not a project file: a built-in or imported mesh or material, a
    // ready-made effect or a scene node. Choices with an empty group ("None", "Gradient") are
    // buttons at the bottom of the browser; the others live in a virtual folder: "@builtin",
    // "@ready", "@scene", or the path of the model file an imported mesh or material came from.
    struct AssetChoice {
        std::string value, label;
        AssetIcon icon{AssetIcon::other};
        std::string group, detail;
        std::optional<std::array<float, 4>> swatch;
    };
    // What the browser is choosing for. `choose` receives a project path or a choice's value; it
    // captures what it needs by value, since the Inspector may show another node by then.
    // `what` names what the field takes, for refusing drops ("a surface material"); when empty it
    // is worked out from the kinds and choices.
    struct AssetPick {
        std::string key, title, current, what;
        std::vector<std::string> kinds;
        std::function<bool(const std::string&)> accepts;
        std::vector<AssetChoice> choices;
        std::vector<std::pair<std::string, std::function<void()>>> actions;
        std::function<void(const std::string&)> choose;
    };
    // Imported and built-in meshes and materials from render.assets, for their people-facing names.
    struct RegistryItem {
        std::string label, source;
        std::optional<std::array<float, 4>> color;
    };
    std::map<std::string, RegistryItem, std::less<>> registry_items;

    struct BrowserEntry {
        AssetEntry entry;
        std::uintmax_t size{};
    };
    struct MaterialFacts {
        std::string type, shader, panorama;
        double checked{-1000.0};
    };
    enum class BrowserSort : std::uint8_t { name, type, size, folder };
    struct AssetBrowser {
        bool open{}, focus{}, search_focus{};
        std::optional<AssetPick> pick;
        std::string folder;
        std::vector<std::string> back, forward;
        std::string selected;
        std::array<char, 65> query{};
        std::set<std::string> kinds;
        bool whole_project{true}, match_paths{}, show_all{}, list_view{};
        BrowserSort sort{BrowserSort::name};
        float tile{96.0F};
        std::map<std::string, std::vector<BrowserEntry>> folders;
        std::set<std::string> expanded;
        std::vector<BrowserEntry> results;
        bool truncated{};
        std::string searched;
        double listed{-1000.0};
        std::string playing;
        std::int64_t sound{};
        double playing_until{};
        // Set while drawing and acted on once the frame's tiles are no longer in use, since
        // navigating or choosing replaces the listings they point into.
        std::optional<std::string> go_to, activate;
        // Whether the window had focus last frame; the editor's shortcuts stand aside while it does.
        bool focused{};
    } browser;
    std::map<std::string, MaterialFacts> material_facts;

    static double wall_seconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // A material's type (surface, post_process or sky), shader and panorama, read at most every
    // few seconds per file.
    const MaterialFacts& material_facts_of(const std::string& path) {
        auto& facts = material_facts[path];
        const double now = wall_seconds();
        if (now - facts.checked < 4.0) return facts;
        facts.checked = now;
        facts = MaterialFacts{{}, {}, {}, now};
        if (auto info = call("assets.material", "\"path\":\"" + json_escape(path) + '"', false); info && info->object())
            if (const auto* material = field(*info->object(), "material"); material && material->object()) {
                facts.type = string_or(*material->object(), "type");
                facts.shader = string_or(*material->object(), "shader");
                return facts;
            }
        if (auto info = call("assets.sky_material", "\"path\":\"" + json_escape(path) + '"', false); info && info->object())
            if (const auto* material = field(*info->object(), "material"); material && material->object()) {
                facts.type = "sky";
                facts.panorama = string_or(*material->object(), "panorama");
            }
        return facts;
    }

    static std::string builtin_label(const std::string_view name) {
        std::string label(name.substr(std::string_view{"builtin."}.size()));
        for (auto& c : label)
            if (c == '_' || c == '.') c = ' ';
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        return label;
    }

    struct Reference {
        std::string name;
        AssetIcon icon{AssetIcon::other};
        std::string detail;
    };
    // How an asset reference reads in a field: a built-in or imported asset's own name, or a
    // project file's name without folders or extension.
    Reference describe_reference(const std::string& value, const AssetIcon fallback) {
        if (value.empty()) return {{}, fallback, {}};
        const bool mesh = std::find(mesh_names.begin(), mesh_names.end(), value) != mesh_names.end();
        if (value.starts_with("builtin."))
            return {builtin_label(value), mesh ? AssetIcon::mesh : AssetIcon::material, "Built into Relay"};
        if (const auto found = registry_items.find(value); found != registry_items.end() && !found->second.label.empty())
            return {found->second.label, mesh ? AssetIcon::mesh : AssetIcon::material,
                    "From " + (found->second.source.empty() ? std::string{"an imported model"} : found->second.source)};
        auto icon = asset_icon_for_file(value);
        if (icon == AssetIcon::material) icon = asset_icon_for_kind("material", material_facts_of(value).type);
        return {asset_display_name(value), icon, value};
    }

    // What a dragged project file or hierarchy node would set on a field: the value to choose, a
    // model to open in the browser (a model holding several of the meshes or materials the field
    // takes), or why it cannot be dropped there. Nothing is being dragged when all are empty.
    struct DropVerdict {
        std::optional<std::string> value, open_model;
        std::string refusal;
        [[nodiscard]] bool usable() const { return value || open_model; }
    };

    [[nodiscard]] static bool dragging_reference() {
        const auto* payload = ImGui::GetDragDropPayload();
        return payload && (payload->IsDataType("relay.asset") || payload->IsDataType("relay.entity"));
    }

    static std::string with_article(const std::string& noun) {
        if (noun.empty()) return noun;
        auto lower = noun;
        lower[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[0])));
        return (std::string_view("aeiou").find(lower[0]) != std::string_view::npos ? "an " : "a ") + lower;
    }

    DropVerdict drop_verdict(const AssetPick& pick) {
        DropVerdict verdict;
        const auto* payload = ImGui::GetDragDropPayload();
        if (!payload || !payload->Data) return verdict;
        const bool nodes = std::any_of(pick.choices.begin(), pick.choices.end(),
                                       [](const AssetChoice& choice) { return choice.group == "@scene"; });
        auto what = pick.what;
        if (what.empty()) {
            if (nodes) what = "a node";
            else if (!pick.kinds.empty()) what = with_article(asset_icon_label(asset_icon_for_kind(pick.kinds.front())));
            else if (!pick.choices.empty()) what = with_article(asset_icon_label(pick.choices.back().icon));
            else what = "something else";
        }
        const std::string dragged(static_cast<const char*>(payload->Data));
        if (payload->IsDataType("relay.entity")) {
            if (std::any_of(pick.choices.begin(), pick.choices.end(), [&](const AssetChoice& choice) {
                    return choice.group == "@scene" && choice.value == dragged;
                }))
                verdict.value = dragged;
            else {
                const auto* node = find_entity(dragged);
                verdict.refusal = (node ? string_or(*node, "name", "This node") : std::string{"This node"}) +
                                  (nodes ? " cannot be used here: this takes " : " is a node: this takes ") + what;
            }
            return verdict;
        }
        if (!payload->IsDataType("relay.asset")) return verdict;
        const auto* entry = asset_entry(dragged);
        const bool folder = entry ? entry->folder : false;
        std::string kind = entry ? entry->kind : asset_kind_of_file(dragged);
        if (!folder && std::find(pick.kinds.begin(), pick.kinds.end(), kind) != pick.kinds.end() &&
            (!pick.accepts || pick.accepts(dragged))) {
            verdict.value = dragged;
            return verdict;
        }
        // A model file stands for the meshes or materials imported from it.
        std::vector<const AssetChoice*> inside;
        for (const auto& choice : pick.choices)
            if (choice.group == dragged) inside.push_back(&choice);
        if (inside.size() == 1U) verdict.value = inside.front()->value;
        else if (inside.size() > 1U) verdict.open_model = dragged;
        if (verdict.usable()) return verdict;
        auto icon = folder ? AssetIcon::folder : asset_icon_for_file(dragged);
        if (icon == AssetIcon::material) icon = asset_icon_for_kind("material", material_facts_of(dragged).type);
        verdict.refusal = asset_display_name(dragged) + " is " + with_article(asset_icon_label(icon)) + ": this takes " + what;
        if (icon == AssetIcon::model && std::any_of(pick.choices.begin(), pick.choices.end(), [](const AssetChoice& choice) {
                return !choice.group.empty() && !choice.group.starts_with('@');
            }))
            verdict.refusal += " (import the model first to use its meshes)";
        return verdict;
    }

    // Takes a drop onto the last item when `verdict` allows it, choosing the value or opening the
    // model in the browser; refusals show why instead of the drag's own tooltip.
    void accept_reference_drop(const AssetPick& pick, const DropVerdict& verdict) {
        if (!verdict.usable()) {
            if (!verdict.refusal.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
                ImGui::SetTooltip("%s", verdict.refusal.c_str());
            return;
        }
        if (!ImGui::BeginDragDropTarget()) return;
        const auto* payload = ImGui::GetDragDropPayload();
        const std::string type = payload && payload->IsDataType("relay.entity") ? "relay.entity" : "relay.asset";
        if (ImGui::AcceptDragDropPayload(type.c_str())) {
            if (verdict.value) {
                if (pick.choose) pick.choose(*verdict.value);
            } else {
                open_asset_browser(named_pick(pick));
                browser_navigate(*verdict.open_model);
            }
        }
        ImGui::EndDragDropTarget();
    }

    // Draws one reference field in the Inspector's label column layout. `empty` is what an unset
    // value reads as. Clicking the field opens the Asset Browser with `make_pick`'s request. Files
    // from Assets or the browser, and nodes from the Hierarchy, drop onto it when they are what the
    // field takes; while something is dragged, fields that would take it are outlined.
    void asset_field(const char* label, const std::string& value, const std::string& key,
                     const std::function<AssetPick()>& make_pick,
                     const char* empty = "None", const AssetIcon empty_icon = AssetIcon::other,
                     const std::optional<Reference>& shown = std::nullopt) {
        const auto& palette = editor_palette();
        const auto& style = ImGui::GetStyle();
        ImGui::PushID(key.c_str());
        inspector_field_label(label);
        const float button = ImGui::GetFrameHeight();
        const float width = std::max(ImGui::CalcItemWidth(), button * 2.0F);
        const auto reference = shown ? *shown : describe_reference(value, empty_icon);
        ImGui::InvisibleButton("##field", ImVec2(width, button));
        note_item(key);
        const bool hovered = ImGui::IsItemHovered();
        const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        const bool picking = browser.open && browser.pick && browser.pick->key == key;
        bool drop_ok = false;
        if (dragging_reference()) {
            const auto pick = make_pick();
            const auto verdict = drop_verdict(pick);
            drop_ok = verdict.usable();
            accept_reference_drop(pick, verdict);
        }
        if (hovered && !ImGui::GetDragDropPayload()) {
            const bool file = !value.empty() && !reference.detail.empty() && reference.detail == value;
            ImGui::SetTooltip("%s%s%s%s%s", reference.name.empty() ? empty : reference.name.c_str(),
                              reference.name.empty() ? "" : "  ·  ", reference.name.empty() ? "" : asset_icon_label(reference.icon),
                              reference.detail.empty() ? "" : ("\n" + reference.detail).c_str(),
                              file ? "\nClick to choose another; right-click to show it in Assets" : "\nClick to choose another");
        }
        if (ImGui::IsItemClicked()) open_asset_browser(named_pick(make_pick()));
        if (!value.empty() && reference.detail == value && ImGui::BeginPopupContextItem("##reference_menu")) {
            if (ImGui::MenuItem("Show in Assets")) {
                panel_open[2] = true;
                reveal_asset(value);
            }
            ImGui::EndPopup();
        }
        auto* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(low, high, hovered ? palette.surface : palette.input, style.FrameRounding);
        if (picking || drop_ok) list->AddRect(low, high, palette.accent_hovered, style.FrameRounding, 0, std::max(1.0F, ui_scale));
        const float icon = std::round(ImGui::GetTextLineHeight() * 0.95F);
        const float icon_x = low.x + std::round(style.FramePadding.x * 0.7F);
        draw_asset_type_icon(list, ImVec2(icon_x, low.y + (button - icon) * 0.5F), icon, reference.icon,
                             reference.name.empty() ? 0.35F : 0.8F);
        const float text_x = icon_x + icon + std::round(style.ItemInnerSpacing.x * 0.8F);
        const std::string text = reference.name.empty() ? std::string{empty} : reference.name;
        list->PushClipRect(low, high, true);
        if (reference.name.empty()) ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
        ImGui::RenderTextEllipsis(list, ImVec2(text_x, low.y + style.FramePadding.y),
                                  ImVec2(high.x - style.FramePadding.x * 0.5F, high.y), high.x - style.FramePadding.x * 0.5F,
                                  text.c_str(), nullptr, nullptr);
        if (reference.name.empty()) ImGui::PopStyleColor();
        list->PopClipRect();
        ImGui::PopID();
    }

    // Picks keep changing the node they were opened for, even when another is selected meanwhile,
    // so the browser's title names it.
    AssetPick named_pick(AssetPick pick) const {
        if (const auto* node = drawing_inspector ? find_entity(selection) : nullptr)
            pick.title += "  ·  " + string_or(*node, "name", "Node");
        return pick;
    }

    // A button that opens the browser to add something, such as a track or an effect.
    // Files that fit drop onto it, like a reference field.
    void asset_add_button(const char* label, const std::string& key, const std::function<AssetPick()>& make_pick) {
        ImGui::PushID(key.c_str());
        inspector_field_label("");
        if (ImGui::Button(label, ImVec2(ImGui::CalcItemWidth(), 0.0F))) open_asset_browser(named_pick(make_pick()));
        note_item(key);
        if (dragging_reference()) {
            const auto pick = make_pick();
            const auto verdict = drop_verdict(pick);
            if (verdict.usable())
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                    editor_palette().accent_hovered, ImGui::GetStyle().FrameRounding, 0,
                                                    std::max(1.0F, ui_scale));
            accept_reference_drop(pick, verdict);
        }
        ImGui::PopID();
    }

    // The project's model files with imported meshes or materials, as choice groups.
    void add_registry_choices(AssetPick& pick, const std::vector<std::string>& names, const bool meshes) {
        for (const auto& name : names) {
            AssetChoice choice;
            choice.value = name;
            choice.icon = meshes ? AssetIcon::mesh : AssetIcon::material;
            const auto found = registry_items.find(name);
            if (found != registry_items.end()) choice.swatch = found->second.color;
            if (name.starts_with("builtin.")) {
                choice.label = builtin_label(name);
                choice.group = "@builtin";
                choice.detail = "Built into Relay";
            } else if (found != registry_items.end() && !found->second.source.empty()) {
                choice.label = found->second.label.empty() ? name : found->second.label;
                choice.group = found->second.source;
                choice.detail = "From " + found->second.source;
            } else {
                choice.label = found != registry_items.end() && !found->second.label.empty() ? found->second.label : name;
                choice.group = "@imported";
                choice.detail = name;
            }
            pick.choices.push_back(std::move(choice));
        }
    }

    // Browser folders that are not project folders.
    static bool virtual_folder(const std::string& folder) { return folder.starts_with('@'); }
    static const char* virtual_folder_label(const std::string& folder) {
        if (folder == "@builtin") return "Built-in";
        if (folder == "@ready") return "Ready-made effects";
        if (folder == "@scene") return "Scene";
        if (folder == "@imported") return "Imported";
        return "Project";
    }
    [[nodiscard]] bool choice_group(const std::string& path) const {
        return browser.pick && std::any_of(browser.pick->choices.begin(), browser.pick->choices.end(),
                                           [&](const AssetChoice& choice) { return choice.group == path; });
    }

    void open_asset_browser(std::optional<AssetPick> pick) {
        stop_browser_sound();
        const bool was_open = browser.open;
        browser.open = browser.focus = true;
        browser.pick = std::move(pick);
        browser.results.clear();
        browser.searched = "\x01";
        browser.selected.clear();
        browser.folders.clear();
        browser.listed = -1000.0;
        if (!browser.pick) {
            if (!was_open) {
                browser.back.clear();
                browser.forward.clear();
            }
            // Browsing shows project folders only; a pick may have left it in a virtual one.
            auto start = virtual_folder(browser.folder) ? std::string{} : browser.folder;
            if (!start.empty() && !list_browser_folder(start)) start.clear();
            browser.folder.clear();
            browser_navigate(start, false);
            return;
        }
        // Start where the current value lives, or in the folder its kind usually lives in.
        const auto& current = browser.pick->current;
        std::string start;
        const auto choice = std::find_if(browser.pick->choices.begin(), browser.pick->choices.end(),
                                         [&](const AssetChoice& item) { return item.value == current && !item.group.empty(); });
        if (choice != browser.pick->choices.end()) {
            start = choice->group;
            browser.selected = "choice:" + current;
        } else if (!current.empty() && !current.starts_with("builtin.") && !current.starts_with("asset.")) {
            start = parent_path_of(current);
            browser.selected = "file:" + current;
        } else if (!browser.pick->choices.empty() && browser.pick->kinds.empty()) {
            for (const auto& item : browser.pick->choices)
                if (!item.group.empty()) {
                    start = item.group;
                    break;
                }
        }
        browser.back.clear();
        browser.forward.clear();
        browser.folder.clear();
        browser_navigate(start, false);
    }

    void close_asset_browser() {
        stop_browser_sound();
        browser.open = false;
        browser.pick.reset();
    }

    void stop_browser_sound() {
        if (browser.sound > 0) (void)call("audio.stop", "\"sound\":" + std::to_string(browser.sound), false);
        browser.sound = 0;
        browser.playing.clear();
    }

    void play_browser_sound(const std::string& path) {
        stop_browser_sound();
        const auto played = call("audio.play_clip", "\"clip\":\"" + json_escape(path) + '"');
        if (!played || !played->object()) return;
        browser.sound = static_cast<std::int64_t>(number_or(*played->object(), "sound", 0.0));
        browser.playing = path;
        const auto* info = audio_clip_summary(path);
        browser.playing_until = wall_seconds() + (info ? number_or(*info, "duration_seconds", 3.0) : 3.0) + 0.1;
    }

    // Opens `folder` in the browser: a project folder, a model file's contents or a virtual folder.
    void browser_navigate(const std::string& folder, const bool remember = true) {
        if (remember && folder != browser.folder) {
            browser.back.push_back(browser.folder);
            browser.forward.clear();
        }
        browser.folder = folder;
        if (!virtual_folder(folder) && !choice_group(folder)) {
            for (auto parent = folder; !parent.empty(); parent = parent_path_of(parent)) browser.expanded.insert(parent);
            list_browser_folder(folder);
        }
        browser.listed = wall_seconds();
    }

    bool list_browser_folder(const std::string& directory) {
        const auto listing = call("assets.browse", "\"directory\":\"" + json_escape(directory) + '"', false);
        const auto* object = listing ? listing->object() : nullptr;
        if (!object) {
            browser.folders.erase(directory);
            return false;
        }
        if (directory.empty()) {
            assets_root = string_or(*object, "root", assets_root);
            thumbnails.set_root(assets_root);
        }
        auto& entries = browser.folders[directory];
        entries.clear();
        if (const auto* values = field(*object, "entries"); values && values->array())
            for (const auto& value : *values->array())
                if (const auto* entry = value.object()) entries.push_back(browser_entry(*entry));
        return true;
    }

    static BrowserEntry browser_entry(const JsonValue::Object& entry) {
        return {{string_or(entry, "name"), string_or(entry, "path"), string_or(entry, "kind"),
                 string_or(entry, "type") == "folder", boolean_or(entry, "importable", false),
                 boolean_or(entry, "protected", false)},
                static_cast<std::uintmax_t>(number_or(entry, "size", 0.0))};
    }

    [[nodiscard]] bool browser_searching() const { return browser.query[0] != '\0' || !browser.kinds.empty(); }

    // The file kinds a search asks for: the filter, else what the pick accepts (and models, which
    // hold imported meshes and materials).
    [[nodiscard]] std::vector<std::string> browser_search_kinds() const {
        if (!browser.kinds.empty()) return {browser.kinds.begin(), browser.kinds.end()};
        if (!browser.pick || browser.show_all) return {};
        auto kinds = browser.pick->kinds;
        if (std::any_of(browser.pick->choices.begin(), browser.pick->choices.end(),
                        [](const AssetChoice& choice) { return !choice.group.empty() && !choice.group.starts_with('@'); }))
            kinds.push_back("model");
        if (kinds.empty()) kinds.push_back("folder");
        return kinds;
    }

    void run_browser_search() {
        std::string kinds = "[";
        for (const auto& kind : browser_search_kinds()) kinds += (kinds.size() > 1U ? ",\"" : "\"") + kind + '"';
        kinds += ']';
        const auto folder = browser.whole_project || virtual_folder(browser.folder) ? std::string{}
                            : choice_group(browser.folder)                         ? parent_path_of(browser.folder)
                                                                                   : browser.folder;
        const auto key = std::string(browser.query.data()) + '\n' + kinds + '\n' + folder + (browser.match_paths ? "\np" : "");
        if (key == browser.searched) return;
        browser.searched = key;
        browser.results.clear();
        browser.truncated = false;
        if (!browser_searching()) return;
        const auto found = call("assets.search", "\"query\":\"" + json_escape(browser.query.data()) + "\",\"kinds\":" + kinds +
                                                     ",\"folder\":\"" + json_escape(folder) + "\",\"paths\":" +
                                                     (browser.match_paths ? "true" : "false"),
                                false);
        const auto* object = found ? found->object() : nullptr;
        if (!object) return;
        browser.truncated = boolean_or(*object, "truncated", false);
        if (const auto* values = field(*object, "entries"); values && values->array())
            for (const auto& value : *values->array())
                if (const auto* entry = value.object()) browser.results.push_back(browser_entry(*entry));
    }

    // Whether a file can be chosen for the current pick.
    bool browser_accepts(const AssetEntry& entry) {
        if (!browser.pick) return true;
        const auto& kinds = browser.pick->kinds;
        if (std::find(kinds.begin(), kinds.end(), entry.kind) == kinds.end()) return false;
        return !browser.pick->accepts || browser.pick->accepts(entry.path);
    }

    struct BrowserTile {
        enum class Type : std::uint8_t { folder, file, choice, group } type{};
        std::string id;
        const AssetEntry* entry{};
        std::uintmax_t size{};
        const AssetChoice* choice{};
        bool usable{true};
        std::string label, where;
        AssetIcon icon{AssetIcon::other};
    };

    static bool words_match(const std::string& text, const std::string_view query) {
        auto lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::size_t start = 0;
        while (start < query.size()) {
            const auto end = std::min(query.find(' ', start), query.size());
            auto word = std::string(query.substr(start, end - start));
            std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!word.empty() && lower.find(word) == std::string::npos) return false;
            start = end + 1U;
        }
        return true;
    }

    std::vector<BrowserTile> browser_tiles() {
        std::vector<BrowserTile> tiles;
        const bool searching = browser_searching();
        const auto add_file = [&](const BrowserEntry& item, const bool show_folder) {
            const auto& entry = item.entry;
            BrowserTile tile;
            tile.entry = &entry;
            tile.size = item.size;
            tile.label = entry.folder ? entry.name : asset_display_name(entry.name);
            if (show_folder) tile.where = parent_path_of(entry.path);
            if (entry.folder) {
                if (searching && browser.pick && !browser.kinds.contains("folder") && browser.kinds.empty() &&
                    std::find(browser.pick->kinds.begin(), browser.pick->kinds.end(), "folder") == browser.pick->kinds.end() &&
                    !browser.show_all)
                    return;
                tile.type = BrowserTile::Type::folder;
                tile.id = "folder:" + entry.path;
                tile.icon = AssetIcon::folder;
            } else if (choice_group(entry.path)) {
                tile.type = BrowserTile::Type::group;
                tile.id = "folder:" + entry.path;
                tile.icon = AssetIcon::model;
            } else {
                tile.type = BrowserTile::Type::file;
                tile.id = "file:" + entry.path;
                tile.icon = asset_icon_for_file(entry.name);
                if (entry.kind == "material") tile.icon = asset_icon_for_kind("material", material_facts_of(entry.path).type);
                tile.usable = browser_accepts(entry);
                if (!tile.usable && browser.pick && !browser.show_all) return;
            }
            if (!browser.kinds.empty() && tile.type != BrowserTile::Type::group && !browser.kinds.contains(entry.kind)) return;
            tiles.push_back(std::move(tile));
        };
        const auto add_choice = [&](const AssetChoice& choice, const bool show_folder) {
            BrowserTile tile;
            tile.type = BrowserTile::Type::choice;
            tile.id = "choice:" + choice.value;
            tile.choice = &choice;
            tile.label = choice.label;
            tile.icon = choice.icon;
            if (show_folder) tile.where = virtual_folder(choice.group) ? virtual_folder_label(choice.group) : choice.group;
            tiles.push_back(std::move(tile));
        };
        if (searching) {
            if (browser.pick && browser.kinds.empty())
                for (const auto& choice : browser.pick->choices)
                    if (!choice.group.empty() && browser.query[0] && words_match(choice.label, browser.query.data()))
                        add_choice(choice, true);
            for (const auto& item : browser.results) add_file(item, true);
        } else if (virtual_folder(browser.folder) || choice_group(browser.folder)) {
            if (browser.pick)
                for (const auto& choice : browser.pick->choices)
                    if (choice.group == browser.folder) add_choice(choice, false);
        } else if (const auto listing = browser.folders.find(browser.folder); listing != browser.folders.end()) {
            for (const auto& item : listing->second) add_file(item, false);
        }
        const auto rank = [](const BrowserTile& tile) { return tile.type == BrowserTile::Type::folder ? 0 : 1; };
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::stable_sort(tiles.begin(), tiles.end(), [&](const BrowserTile& a, const BrowserTile& b) {
            if (rank(a) != rank(b)) return rank(a) < rank(b);
            switch (browser.sort) {
            case BrowserSort::type:
                if (a.icon != b.icon) return std::string_view(asset_icon_label(a.icon)) < asset_icon_label(b.icon);
                break;
            case BrowserSort::size:
                if (a.size != b.size) return a.size > b.size;
                break;
            case BrowserSort::folder:
                if (a.where != b.where) return lower(a.where) < lower(b.where);
                break;
            case BrowserSort::name: break;
            }
            return lower(a.label) < lower(b.label);
        });
        return tiles;
    }

    static std::string size_text(const std::uintmax_t bytes) {
        std::array<char, 32> text{};
        if (bytes >= 1024U * 1024U)
            std::snprintf(text.data(), text.size(), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        else if (bytes >= 1024U)
            std::snprintf(text.data(), text.size(), "%.0f KB", static_cast<double>(bytes) / 1024.0);
        else
            std::snprintf(text.data(), text.size(), "%ju B", bytes);
        return text.data();
    }

    // An image fitted inside a square, keeping its aspect.
    static void draw_fitted_image(ImDrawList* list, ImTextureData* texture, const ImVec2 low, const float side) {
        if (texture->Status == ImTextureStatus_Destroyed) texture->SetStatus(ImTextureStatus_WantCreate);
        const float width = static_cast<float>(texture->Width), height = static_cast<float>(texture->Height);
        const float scale = side / std::max(width, height);
        const ImVec2 size(width * scale, height * scale);
        const ImVec2 corner(low.x + (side - size.x) * 0.5F, low.y + (side - size.y) * 0.5F);
        list->AddImageRounded(texture->GetTexRef(), corner, ImVec2(corner.x + size.x, corner.y + size.y), ImVec2(0, 0),
                              ImVec2(1, 1), IM_COL32_WHITE, std::max(2.0F, side * 0.04F));
    }

    // The picture in a tile: a thumbnail when the asset has one, otherwise its icon.
    void draw_tile_picture(ImDrawList* list, const BrowserTile& tile, const ImVec2 low, const float side, const float alpha) {
        ImTextureData* picture = nullptr;
        if (tile.type == BrowserTile::Type::file || tile.type == BrowserTile::Type::group) {
            const auto& path = tile.entry->path;
            if (tile.entry->kind == "image" && panorama_file(path)) {
                picture = thumbnails.file(path, AssetThumbnails::Source::image);
            } else if (tile.entry->kind == "model" && !path.ends_with(".blend")) {
                picture = thumbnails.file(path, AssetThumbnails::Source::model);
            } else if (tile.entry->kind == "material") {
                const auto& facts = material_facts_of(path);
                if (facts.type == "surface") picture = thumbnails.material(path, facts.shader);
                else if (facts.type == "sky" && panorama_file(facts.panorama))
                    picture = thumbnails.file(facts.panorama, AssetThumbnails::Source::image);
            }
        }
        if (picture) {
            draw_fitted_image(list, picture, low, side);
            // The kind's icon in a corner, so a picture of a sphere still reads as a material.
            const float badge = std::clamp(side * 0.24F, 12.0F * ui_scale, 20.0F * ui_scale);
            const ImVec2 corner(low.x + 2.0F, low.y + side - badge - 2.0F);
            list->AddRectFilled(corner, ImVec2(corner.x + badge, corner.y + badge), IM_COL32(0, 0, 0, 170), badge * 0.25F);
            draw_asset_type_icon(list, ImVec2(corner.x + badge * 0.12F, corner.y + badge * 0.12F), badge * 0.76F, tile.icon, alpha);
            return;
        }
        // Imported meshes are drawn alone from the model they came from.
        if (tile.choice && tile.choice->icon == AssetIcon::mesh && !tile.choice->group.empty() &&
            !virtual_folder(tile.choice->group) && !tile.choice->group.ends_with(".blend"))
            picture = thumbnails.mesh(tile.choice->group, tile.choice->value);
        if (picture) {
            draw_fitted_image(list, picture, low, side);
            return;
        }
        if (tile.choice && tile.choice->swatch) {
            // A built-in or imported material's own colour on a ball.
            const auto& color = *tile.choice->swatch;
            const auto channel = [&](const std::size_t index) { return static_cast<int>(std::clamp(color[index], 0.0F, 1.0F) * 255.0F); };
            const ImVec2 centre(low.x + side * 0.5F, low.y + side * 0.5F);
            list->AddCircleFilled(centre, side * 0.34F, IM_COL32(channel(0) / 2, channel(1) / 2, channel(2) / 2, static_cast<int>(255 * alpha)), 32);
            list->AddCircleFilled(ImVec2(centre.x - side * 0.04F, centre.y - side * 0.04F), side * 0.29F,
                                  IM_COL32(channel(0), channel(1), channel(2), static_cast<int>(255 * alpha)), 32);
            list->AddCircleFilled(ImVec2(centre.x - side * 0.13F, centre.y - side * 0.13F), side * 0.06F,
                                  IM_COL32(255, 255, 255, static_cast<int>(150 * alpha)), 12);
            return;
        }
        const float icon = side * 0.56F;
        draw_asset_type_icon(list, ImVec2(low.x + (side - icon) * 0.5F, low.y + (side - icon) * 0.5F), icon, tile.icon, alpha);
    }

    // Double-click or Enter on a tile: open folders, choose usable items, or open files.
    void activate_tile(const BrowserTile& tile) {
        if (tile.type == BrowserTile::Type::folder || tile.type == BrowserTile::Type::group) {
            if (browser_searching()) {
                browser.query.fill('\0');
                browser.kinds.clear();
            }
            browser_navigate(std::string(tile.entry->path));
            return;
        }
        if (browser.pick) {
            if (!tile.usable) return;
            auto choose = browser.pick->choose;
            const auto value = tile.type == BrowserTile::Type::choice ? tile.choice->value : tile.entry->path;
            close_asset_browser();
            if (choose) choose(value);
            return;
        }
        if (tile.entry) open_asset(*tile.entry);
    }

    void draw_browser_tree_folder(const std::string& path, const std::string& name) {
        ImGui::PushID(path.c_str());
        const bool current = browser.folder == path;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (current) flags |= ImGuiTreeNodeFlags_Selected;
        const auto listing = browser.folders.find(path);
        const bool known = listing != browser.folders.end();
        const bool has_folders = !known || std::any_of(listing->second.begin(), listing->second.end(),
                                                       [](const BrowserEntry& item) { return item.entry.folder; });
        if (!has_folders) flags |= ImGuiTreeNodeFlags_Leaf;
        ImGui::SetNextItemOpen(browser.expanded.contains(path) || path.empty(), path.empty() ? ImGuiCond_Once : ImGuiCond_Always);
        const bool open = ImGui::TreeNodeEx("##folder", flags);
        note_item("asset_browser:tree:" + path);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) browser.go_to = path;
        if (ImGui::IsItemToggledOpen()) {
            if (open) browser.expanded.insert(path);
            else browser.expanded.erase(path);
        }
        const auto low = ImGui::GetItemRectMin();
        const float icon = ImGui::GetTextLineHeight();
        const float x = low.x + ImGui::GetTreeNodeToLabelSpacing();
        draw_asset_type_icon(ImGui::GetWindowDrawList(), ImVec2(x, low.y + (ImGui::GetItemRectSize().y - icon) * 0.5F), icon,
                             AssetIcon::folder, 0.9F);
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::SetCursorScreenPos(ImVec2(x + icon + ImGui::GetStyle().ItemInnerSpacing.x, low.y));
        ImGui::TextUnformatted(name.c_str());
        if (open) {
            if (!known) list_browser_folder(path);
            if (const auto listed = browser.folders.find(path); listed != browser.folders.end())
                for (const auto& item : std::vector<BrowserEntry>(listed->second))
                    if (item.entry.folder) draw_browser_tree_folder(item.entry.path, item.entry.name);
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void draw_browser_tree() {
        const auto virtual_row = [&](const std::string& folder, const AssetIcon icon) {
            ImGui::PushID(folder.c_str());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                       ImGuiTreeNodeFlags_SpanAvailWidth;
            if (browser.folder == folder) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::TreeNodeEx("##virtual", flags);
            note_item("asset_browser:tree:" + folder);
            if (ImGui::IsItemClicked()) browser.go_to = folder;
            const auto low = ImGui::GetItemRectMin();
            const float size = ImGui::GetTextLineHeight();
            const float x = low.x + ImGui::GetTreeNodeToLabelSpacing();
            draw_asset_type_icon(ImGui::GetWindowDrawList(), ImVec2(x, low.y + (ImGui::GetItemRectSize().y - size) * 0.5F),
                                 size, icon, 0.9F);
            ImGui::SameLine(0.0F, 0.0F);
            ImGui::SetCursorScreenPos(ImVec2(x + size + ImGui::GetStyle().ItemInnerSpacing.x, low.y));
            ImGui::TextUnformatted(virtual_folder_label(folder));
            ImGui::PopID();
        };
        if (browser.pick) {
            // Each virtual folder with the icon of what it holds.
            std::map<std::string, AssetIcon> groups;
            for (const auto& choice : browser.pick->choices)
                if (virtual_folder(choice.group)) groups.emplace(choice.group, choice.icon);
            for (const auto* folder : {"@scene", "@builtin", "@ready", "@imported"})
                if (const auto found = groups.find(folder); found != groups.end()) virtual_row(folder, found->second);
            if (browser.pick->kinds.empty() &&
                std::none_of(browser.pick->choices.begin(), browser.pick->choices.end(), [](const AssetChoice& choice) {
                    return !choice.group.empty() && !virtual_folder(choice.group);
                }))
                return;
        }
        draw_browser_tree_folder({}, "Project");
    }

    void draw_browser_toolbar() {
        const auto& palette = editor_palette();
        const auto& style = ImGui::GetStyle();
        const float button = ImGui::GetFrameHeight();
        ImGui::BeginDisabled(browser.back.empty());
        if (ImGui::ArrowButton("##back", ImGuiDir_Left)) {
            browser.forward.push_back(browser.folder);
            const auto target = browser.back.back();
            browser.back.pop_back();
            browser_navigate(target, false);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Back");
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::BeginDisabled(browser.forward.empty());
        if (ImGui::ArrowButton("##forward", ImGuiDir_Right)) {
            browser.back.push_back(browser.folder);
            const auto target = browser.forward.back();
            browser.forward.pop_back();
            browser_navigate(target, false);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Forward");
        ImGui::SameLine(0.0F, 2.0F);
        ImGui::BeginDisabled(browser.folder.empty() || virtual_folder(browser.folder));
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) browser_navigate(parent_path_of(browser.folder));
        ImGui::EndDisabled();
        note_item("asset_browser:up");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Up to the parent folder");

        // The path as clickable steps.
        const float controls = 300.0F * ui_scale + button * 3.0F + style.ItemSpacing.x * 4.0F;
        ImGui::SameLine();
        const float crumbs_end = ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - controls, 40.0F);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 0.5F, style.FramePadding.y));
        std::vector<std::pair<std::string, std::string>> steps;
        if (virtual_folder(browser.folder)) {
            steps.emplace_back(browser.folder, virtual_folder_label(browser.folder));
        } else {
            steps.emplace_back(std::string{}, "Project");
            std::string built;
            for (std::size_t start = 0; start < browser.folder.size();) {
                const auto end = std::min(browser.folder.find('/', start), browser.folder.size());
                built = browser.folder.substr(0, end);
                steps.emplace_back(built, browser.folder.substr(start, end - start));
                start = end + 1U;
            }
        }
        if (browser_searching()) steps.emplace_back("\x01", browser.whole_project ? "Search everywhere" : "Search here");
        for (std::size_t index = 0; index < steps.size(); ++index) {
            const auto& [path, name] = steps[index];
            if (index) {
                ImGui::SameLine(0.0F, 0.0F);
                ImGui::TextColored(editor_color(palette.text_faint), "/");
                ImGui::SameLine(0.0F, 0.0F);
            }
            if (ImGui::GetCursorPosX() + ImGui::CalcTextSize(name.c_str()).x > crumbs_end) {
                ImGui::TextColored(editor_color(palette.text_faint), "...");
                break;
            }
            ImGui::PushID(static_cast<int>(index));
            const bool last = index + 1U == steps.size();
            if (last) ImGui::PushStyleColor(ImGuiCol_Text, palette.text);
            else ImGui::PushStyleColor(ImGuiCol_Text, palette.text_dim);
            if (ImGui::Button(name.c_str()) && path != "\x01") {
                browser.query.fill('\0');
                browser.kinds.clear();
                browser_navigate(path);
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Search, filters, options and view on the right.
        ImGui::SameLine(std::max(crumbs_end, ImGui::GetCursorPosX()) + style.ItemSpacing.x);
        if (browser.search_focus) {
            ImGui::SetKeyboardFocusHere();
            browser.search_focus = false;
        }
        ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - button * 3.0F - style.ItemSpacing.x * 3.0F,
                                         80.0F * ui_scale));
        ImGui::InputTextWithHint("##browser_search", "Search (words in any order)", browser.query.data(), browser.query.size());
        note_item("asset_browser:search");
        if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) browser.query.fill('\0');
        ImGui::SameLine();
        if (filter_button("##browser_filter", !browser.kinds.empty())) ImGui::OpenPopup("##browser_filters");
        note_item("asset_browser:filter");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only some types");
        if (ImGui::BeginPopup("##browser_filters")) {
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            for (const auto& [kind, label] : asset_kind_labels) {
                bool enabled = browser.kinds.contains(kind);
                if (ImGui::MenuItem(label, nullptr, &enabled)) {
                    if (enabled) browser.kinds.insert(kind);
                    else browser.kinds.erase(kind);
                }
                note_item(std::string("asset_browser:filter:") + kind);
            }
            ImGui::PopItemFlag();
            ImGui::Separator();
            if (ImGui::MenuItem("Clear filters", nullptr, false, !browser.kinds.empty())) browser.kinds.clear();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("...##browser_options", ImVec2(button, button))) ImGui::OpenPopup("##browser_settings");
        note_item("asset_browser:options");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search and view options");
        if (ImGui::BeginPopup("##browser_settings")) {
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            ImGui::SeparatorText("Search");
            if (ImGui::MenuItem("Whole project", nullptr, browser.whole_project)) browser.whole_project = true;
            note_item("asset_browser:scope:project");
            if (ImGui::MenuItem("This folder and below", nullptr, !browser.whole_project)) browser.whole_project = false;
            note_item("asset_browser:scope:folder");
            ImGui::MenuItem("Match folder names too", nullptr, &browser.match_paths);
            note_item("asset_browser:match_paths");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("\"trees bark\" then finds textures/trees/Oak_bark.png");
            if (browser.pick) {
                ImGui::MenuItem("Show files that do not fit", nullptr, &browser.show_all);
                note_item("asset_browser:show_all");
            }
            ImGui::SeparatorText("Sort by");
            for (const auto& [sort, label] : std::array<std::pair<BrowserSort, const char*>, 4>{
                     {{BrowserSort::name, "Name"}, {BrowserSort::type, "Type"}, {BrowserSort::size, "Size"},
                      {BrowserSort::folder, "Folder"}}})
                if (ImGui::MenuItem(label, nullptr, browser.sort == sort)) browser.sort = sort;
            ImGui::SeparatorText("View");
            if (ImGui::MenuItem("Thumbnails", nullptr, !browser.list_view)) browser.list_view = false;
            if (ImGui::MenuItem("List", nullptr, browser.list_view)) browser.list_view = true;
            note_item("asset_browser:list_view");
            if (!browser.list_view) {
                ImGui::SetNextItemWidth(160.0F * ui_scale);
                (void)editor_slider("Size##tile", &browser.tile, 56.0F, 192.0F, "%.0f px");
            }
            ImGui::PopItemFlag();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("##browser_view", ImVec2(button, button))) browser.list_view = !browser.list_view;
        note_item("asset_browser:view");
        {
            // Shows the view the button switches to: a grid of squares or lines of a list.
            auto* list = ImGui::GetWindowDrawList();
            const auto low = ImGui::GetItemRectMin();
            const ImU32 color = ImGui::IsItemHovered() ? palette.text : palette.text_dim;
            const float unit = button / 10.0F;
            if (browser.list_view) {
                for (const float y : {3.0F, 5.5F})
                    for (const float x : {3.0F, 5.5F})
                        list->AddRectFilled(ImVec2(low.x + x * unit, low.y + y * unit),
                                            ImVec2(low.x + (x + 1.8F) * unit, low.y + (y + 1.8F) * unit), color, unit * 0.3F);
            } else {
                for (const float y : {3.2F, 5.0F, 6.8F})
                    list->AddRectFilled(ImVec2(low.x + 2.8F * unit, low.y + y * unit), ImVec2(low.x + 7.2F * unit, low.y + (y + 0.9F) * unit),
                                        color, unit * 0.3F);
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(browser.list_view ? "Show thumbnails" : "Show a list");

        // Active type filters as removable chips.
        if (!browser.kinds.empty()) {
            bool first = true;
            for (const auto& [kind, label] : asset_kind_labels) {
                if (!browser.kinds.contains(kind)) continue;
                const std::string chip = std::string(label) + "  x##browser_chip_" + kind;
                if (!first) ImGui::SameLine();
                first = false;
                if (ImGui::SmallButton(chip.c_str())) {
                    browser.kinds.erase(kind);
                    break;
                }
            }
        }
    }

    // One tile of the thumbnail grid.
    void draw_browser_tile(const BrowserTile& tile, const ImVec2 low, const float side, const float height) {
        const auto& palette = editor_palette();
        auto* list = ImGui::GetWindowDrawList();
        ImGui::PushID(tile.id.c_str());
        ImGui::SetCursorScreenPos(low);
        ImGui::SetNextItemAllowOverlap();
        const float pad = std::round(6.0F * ui_scale);
        ImGui::InvisibleButton("##tile", ImVec2(side + pad * 2.0F, height));
        const auto key = tile.type == BrowserTile::Type::choice ? "asset_browser:choice:" + tile.choice->value
                                                                : "asset_browser:item:" + tile.entry->path;
        note_item(key);
        const bool hovered = ImGui::IsItemHovered();
        const bool selected = browser.selected == tile.id;
        if (ImGui::IsItemClicked()) browser.selected = tile.id;
        const bool activated = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (tile.entry && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("relay.asset", tile.entry->path.c_str(), tile.entry->path.size() + 1U);
            ImGui::TextUnformatted(tile.label.c_str());
            ImGui::EndDragDropSource();
        }
        draw_browser_item_menu(tile);
        const ImVec2 high(low.x + side + pad * 2.0F, low.y + height);
        if (selected) {
            list->AddRectFilled(low, high, palette.accent_soft, 6.0F * ui_scale);
            list->AddRect(low, high, palette.accent, 6.0F * ui_scale, 0, std::max(1.0F, ui_scale));
        } else if (hovered) {
            list->AddRectFilled(low, high, palette.surface_hovered, 6.0F * ui_scale);
        }
        const ImVec2 picture(low.x + pad, low.y + pad);
        list->AddRectFilled(picture, ImVec2(picture.x + side, picture.y + side), palette.surface, 5.0F * ui_scale);
        const float alpha = tile.usable ? 1.0F : 0.35F;
        draw_tile_picture(list, tile, picture, side, alpha);
        if (tile.type == BrowserTile::Type::group) {
            // A model holding meshes or materials to choose: a chevron in the corner says it opens.
            const float badge = std::clamp(side * 0.2F, 12.0F * ui_scale, 18.0F * ui_scale);
            const ImVec2 corner(picture.x + side - badge - 3.0F, picture.y + 3.0F);
            list->AddRectFilled(corner, ImVec2(corner.x + badge, corner.y + badge), IM_COL32(0, 0, 0, 170), badge * 0.3F);
            const ImVec2 chevron[]{{corner.x + badge * 0.4F, corner.y + badge * 0.26F},
                                   {corner.x + badge * 0.66F, corner.y + badge * 0.5F},
                                   {corner.x + badge * 0.4F, corner.y + badge * 0.74F}};
            list->AddPolyline(chevron, 3, palette.text, ImDrawFlags_None, std::max(1.2F, 1.4F * ui_scale));
        }
        // Names wrap onto a second line when a folder is shown under them in search results.
        const auto& style = ImGui::GetStyle();
        const float line = ImGui::GetTextLineHeight();
        const float text_top = picture.y + side + style.ItemInnerSpacing.y;
        const float text_width = ImGui::CalcTextSize(tile.label.c_str()).x;
        const float left = low.x + pad * 0.5F, right = high.x - pad * 0.5F;
        const float x = text_width < right - left ? low.x + (high.x - low.x - text_width) * 0.5F : left;
        if (!tile.usable) ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
        ImGui::RenderTextEllipsis(list, ImVec2(x, text_top), ImVec2(right, text_top + line), right, tile.label.c_str(), nullptr,
                                  nullptr);
        if (!tile.usable) ImGui::PopStyleColor();
        if (!tile.where.empty()) {
            const auto where = tile.where;
            const float where_width = ImGui::CalcTextSize(where.c_str()).x;
            const float where_x = where_width < right - left ? low.x + (high.x - low.x - where_width) * 0.5F : left;
            ImGui::PushStyleColor(ImGuiCol_Text, palette.text_faint);
            ImGui::RenderTextEllipsis(list, ImVec2(where_x, text_top + line), ImVec2(right, text_top + line * 2.0F), right,
                                      where.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        }
        // Sounds play from their tile.
        if (tile.entry && tile.entry->kind == "audio") {
            const float radius = std::max(9.0F * ui_scale, side * 0.13F);
            sound_button(tile.entry->path, key, ImVec2(picture.x + side - radius - 4.0F, picture.y + side - radius - 4.0F), radius);
        }
        if (hovered) browser_tile_tooltip(tile);
        ImGui::PopID();
        if (activated) browser.activate = tile.id;
    }

    // A round play button that previews a sound, or stops it while it plays.
    void sound_button(const std::string& path, const std::string& key, const ImVec2 centre, const float radius) {
        const auto& palette = editor_palette();
        auto* list = ImGui::GetWindowDrawList();
        const bool playing = browser.playing == path;
        ImGui::SetCursorScreenPos(ImVec2(centre.x - radius, centre.y - radius));
        if (ImGui::InvisibleButton("##play", ImVec2(radius * 2.0F, radius * 2.0F))) {
            if (playing) stop_browser_sound();
            else play_browser_sound(path);
        }
        note_item(key + ":play");
        const bool over = ImGui::IsItemHovered();
        if (over) ImGui::SetTooltip(playing ? "Stop" : "Listen");
        list->AddCircleFilled(centre, radius, over ? palette.accent_hovered : palette.accent, 24);
        if (playing) {
            const float half = radius * 0.34F;
            list->AddRectFilled(ImVec2(centre.x - half, centre.y - half), ImVec2(centre.x + half, centre.y + half), palette.text, 1.5F);
        } else {
            const float half = radius * 0.42F;
            list->AddTriangleFilled(ImVec2(centre.x - half * 0.7F, centre.y - half), ImVec2(centre.x + half, centre.y),
                                    ImVec2(centre.x - half * 0.7F, centre.y + half), palette.text);
        }
    }

    void browser_tile_tooltip(const BrowserTile& tile) {
        if (ImGui::GetDragDropPayload()) return;
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(tile.label.c_str());
        ImGui::TextColored(editor_color(editor_palette().text_dim), "%s", asset_icon_label(tile.icon));
        if (tile.choice && !tile.choice->detail.empty())
            ImGui::TextColored(editor_color(editor_palette().text_faint), "%s", tile.choice->detail.c_str());
        if (tile.entry) {
            ImGui::TextColored(editor_color(editor_palette().text_faint), "%s", tile.entry->path.c_str());
            if (!tile.entry->folder && tile.size) ImGui::TextColored(editor_color(editor_palette().text_faint), "%s", size_text(tile.size).c_str());
        }
        if (!tile.usable) ImGui::TextColored(editor_color(editor_palette().warning), "Cannot be used here");
        ImGui::EndTooltip();
    }

    void draw_browser_item_menu(const BrowserTile& tile) {
        if (!tile.entry || !ImGui::BeginPopupContextItem("##item_menu")) return;
        browser.selected = tile.id;
        if (tile.entry->kind == "audio") {
            if (ImGui::MenuItem("Listen")) play_browser_sound(tile.entry->path);
            if (ImGui::MenuItem("Stop", nullptr, false, !browser.playing.empty())) stop_browser_sound();
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Show in Assets")) {
            panel_open[2] = true;
            reveal_asset(tile.entry->path);
        }
        if (ImGui::MenuItem("Open in file browser")) open_in_file_browser(tile.entry->path, tile.entry->folder);
        ImGui::EndPopup();
    }

    // One row of the list view.
    void draw_browser_row(const BrowserTile& tile) {
        const auto& palette = editor_palette();
        ImGui::PushID(tile.id.c_str());
        const float height = ImGui::GetFrameHeight();
        const auto low = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##row", ImVec2(width, height));
        const auto key = tile.type == BrowserTile::Type::choice ? "asset_browser:choice:" + tile.choice->value
                                                                : "asset_browser:item:" + tile.entry->path;
        note_item(key);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) browser.selected = tile.id;
        const bool activated = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (tile.entry && ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("relay.asset", tile.entry->path.c_str(), tile.entry->path.size() + 1U);
            ImGui::TextUnformatted(tile.label.c_str());
            ImGui::EndDragDropSource();
        }
        draw_browser_item_menu(tile);
        auto* list = ImGui::GetWindowDrawList();
        const ImVec2 high(low.x + width, low.y + height);
        if (browser.selected == tile.id) list->AddRectFilled(low, high, palette.accent_soft, 4.0F * ui_scale);
        else if (hovered) list->AddRectFilled(low, high, palette.surface_hovered, 4.0F * ui_scale);
        const float icon = ImGui::GetTextLineHeight();
        const float alpha = tile.usable ? 1.0F : 0.35F;
        if (tile.choice && tile.choice->swatch) draw_tile_picture(list, tile, ImVec2(low.x + 6.0F, low.y + (height - icon) * 0.5F), icon, alpha);
        else draw_asset_type_icon(list, ImVec2(low.x + 6.0F, low.y + (height - icon) * 0.5F), icon, tile.icon, alpha);
        const float text_y = low.y + (height - icon) * 0.5F;
        const auto column = [&](const float fraction, const std::string& text, const ImU32 color) {
            const float x = low.x + width * fraction;
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            ImGui::RenderTextEllipsis(list, ImVec2(x, text_y), ImVec2(low.x + width - 4.0F, high.y), low.x + width - 4.0F,
                                      text.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
        };
        ImGui::PushStyleColor(ImGuiCol_Text, tile.usable ? palette.text : palette.text_faint);
        ImGui::RenderTextEllipsis(list, ImVec2(low.x + 12.0F + icon, text_y), ImVec2(low.x + width * 0.45F, high.y),
                                  low.x + width * 0.45F - 6.0F, tile.label.c_str(), nullptr, nullptr);
        ImGui::PopStyleColor();
        column(0.46F, asset_icon_label(tile.icon), palette.text_dim);
        column(0.66F, tile.where.empty() && tile.entry ? parent_path_of(tile.entry->path) : tile.where, palette.text_faint);
        if (tile.entry && !tile.entry->folder && tile.size) column(0.84F, size_text(tile.size), palette.text_faint);
        if (tile.entry && tile.entry->kind == "audio") {
            const float radius = height * 0.36F;
            sound_button(tile.entry->path, key, ImVec2(low.x + width - radius - 6.0F, low.y + height * 0.5F), radius);
        }
        if (hovered && !ImGui::IsAnyItemActive()) browser_tile_tooltip(tile);
        ImGui::SetCursorScreenPos(low);
        ImGui::Dummy(ImVec2(width, height));
        ImGui::PopID();
        if (activated) browser.activate = tile.id;
    }

    void draw_browser_items(const std::vector<BrowserTile>& tiles) {
        const auto& palette = editor_palette();
        if (tiles.empty()) {
            ImGui::TextColored(editor_color(palette.text_faint), "%s",
                               browser_searching() ? "Nothing matches. Try fewer words, another type, or search the whole project."
                                                   : browser.pick ? "Nothing here can be used for this field." : "This folder is empty.");
            return;
        }
        if (browser.list_view) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(tiles.size()), ImGui::GetFrameHeightWithSpacing());
            while (clipper.Step())
                for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
                    draw_browser_row(tiles[static_cast<std::size_t>(index)]);
            clipper.End();
        } else {
            const float side = std::round(browser.tile * ui_scale);
            const float pad = std::round(6.0F * ui_scale);
            const float spacing = std::round(6.0F * ui_scale);
            const bool two_lines = browser_searching();
            const float height = pad + side + ImGui::GetStyle().ItemInnerSpacing.y +
                                 ImGui::GetTextLineHeight() * (two_lines ? 2.0F : 1.0F) + pad;
            const float cell = side + pad * 2.0F;
            const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (cell + spacing)));
            const int rows = (static_cast<int>(tiles.size()) + columns - 1) / columns;
            ImGuiListClipper clipper;
            clipper.Begin(rows, height + spacing);
            while (clipper.Step())
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto origin = ImGui::GetCursorScreenPos();
                    for (int column = 0; column < columns; ++column) {
                        const auto index = static_cast<std::size_t>(row * columns + column);
                        if (index >= tiles.size()) break;
                        draw_browser_tile(tiles[index], ImVec2(origin.x + static_cast<float>(column) * (cell + spacing), origin.y),
                                          side, height);
                    }
                    ImGui::SetCursorScreenPos(origin);
                    ImGui::Dummy(ImVec2(1.0F, height + spacing - ImGui::GetStyle().ItemSpacing.y));
                }
            clipper.End();
        }
        if (browser.truncated)
            ImGui::TextColored(editor_color(palette.text_faint), "Only the first 512 matches are shown; add words to narrow it.");
    }

    // Arrow keys move through the tiles, Enter uses the selection and Backspace goes up.
    void update_browser_keys(const std::vector<BrowserTile>& tiles, const int columns) {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy) ||
            ImGui::GetIO().WantTextInput)
            return;
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false)) browser.search_focus = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) && !browser.folder.empty() && !virtual_folder(browser.folder))
            browser_navigate(parent_path_of(browser.folder));
        if (tiles.empty()) return;
        auto current = std::find_if(tiles.begin(), tiles.end(), [&](const BrowserTile& tile) { return tile.id == browser.selected; });
        int index = current == tiles.end() ? -1 : static_cast<int>(current - tiles.begin());
        const int count = static_cast<int>(tiles.size());
        const auto move = [&](const int step) { index = std::clamp(index < 0 ? 0 : index + step, 0, count - 1); };
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(browser.list_view ? 1 : columns);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(browser.list_view ? -1 : -columns);
        if (index >= 0) browser.selected = tiles[static_cast<std::size_t>(index)].id;
        if (index >= 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
            browser.activate = browser.selected;
    }

    void draw_browser_footer(const std::vector<BrowserTile>& tiles) {
        const auto& palette = editor_palette();
        ImGui::Separator();
        if (browser.pick) {
            // "None", "Gradient" and similar, then actions such as "New material".
            for (const auto& choice : std::vector<AssetChoice>(browser.pick->choices)) {
                if (!choice.group.empty()) continue;
                if (ImGui::Button(choice.label.c_str())) {
                    auto choose = browser.pick->choose;
                    close_asset_browser();
                    if (choose) choose(choice.value);
                    return;
                }
                note_item("asset_browser:button:" + choice.value);
                ImGui::SameLine();
            }
            for (const auto& [label, action] : std::vector<std::pair<std::string, std::function<void()>>>(browser.pick->actions)) {
                if (ImGui::Button(label.c_str())) {
                    close_asset_browser();
                    if (action) action();
                    return;
                }
                note_item("asset_browser:action:" + label);
                ImGui::SameLine();
            }
        }
        const auto selected = std::find_if(tiles.begin(), tiles.end(), [&](const BrowserTile& tile) { return tile.id == browser.selected; });
        const auto& style = ImGui::GetStyle();
        const float buttons = browser.pick ? ImGui::CalcTextSize("Cancel").x + ImGui::CalcTextSize("Choose").x +
                                                 style.FramePadding.x * 4.0F + style.ItemSpacing.x * 2.0F
                                           : ImGui::CalcTextSize("Close").x + style.FramePadding.x * 2.0F + style.ItemSpacing.x;
        if (selected != tiles.end()) {
            ImGui::AlignTextToFramePadding();
            const auto text = selected->entry ? selected->entry->path : selected->choice->label;
            ImGui::TextColored(editor_color(palette.text_dim), "%s", text.c_str());
            ImGui::SameLine();
        }
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - buttons));
        if (browser.pick) {
            if (ImGui::Button("Cancel")) close_asset_browser();
            note_item("asset_browser:cancel");
            ImGui::SameLine();
            const bool usable = selected != tiles.end() && selected->usable;
            ImGui::BeginDisabled(!usable);
            if (ImGui::Button(selected != tiles.end() && selected->type != BrowserTile::Type::file &&
                                      selected->type != BrowserTile::Type::choice
                                  ? "Open"
                                  : "Choose"))
                browser.activate = selected->id;
            ImGui::EndDisabled();
            note_item("asset_browser:choose");
        } else if (ImGui::Button("Close")) {
            close_asset_browser();
        }
    }

    void draw_asset_browser() {
        browser.focused = false;
        if (!browser.open) return;
        if (browser.playing_until > 0.0 && !browser.playing.empty() && wall_seconds() > browser.playing_until) {
            browser.playing.clear();
            browser.sound = 0;
        }
        const auto* main_viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowSize(ImVec2(940.0F * ui_scale, 600.0F * ui_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(main_viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5F, 0.5F));
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0F * ui_scale, 300.0F * ui_scale), ImVec2(FLT_MAX, FLT_MAX));
        if (browser.focus) {
            ImGui::SetNextWindowFocus();
            browser.focus = false;
        }
        const auto title = (browser.pick ? browser.pick->title : std::string{"Asset Browser"}) + "###asset_browser";
        bool open = true;
        const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse);
        // Escape in one of the browser's menus closes only the menu.
        if (!open || (visible &&
                      ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows | ImGuiFocusedFlags_NoPopupHierarchy) &&
                      !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
            ImGui::End();
            close_asset_browser();
            return;
        }
        if (!visible) {
            ImGui::End();
            return;
        }
        browser.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        // The open folder is listed again every two seconds, so new and changed files show up.
        if (wall_seconds() - browser.listed > 2.0 && !virtual_folder(browser.folder) && !choice_group(browser.folder)) {
            list_browser_folder(browser.folder);
            browser.listed = wall_seconds();
            browser.searched = "\x01";
        }
        draw_browser_toolbar();
        run_browser_search();
        const auto tiles = browser_tiles();
        // Room for the separator and the row of buttons below the panes.
        const auto& spacing = ImGui::GetStyle().ItemSpacing;
        const float footer = ImGui::GetFrameHeight() + spacing.y * 3.0F + 2.0F;
        const float body = std::max(ImGui::GetContentRegionAvail().y - footer, 60.0F);
        int columns = 1;
        if (ImGui::BeginTable("##browser_split", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                              ImVec2(0.0F, body))) {
            ImGui::TableSetupColumn("folders", ImGuiTableColumnFlags_WidthFixed, 210.0F * ui_scale);
            ImGui::TableSetupColumn("items", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (begin_region("##browser_tree", ImVec2(0.0F, body))) draw_browser_tree();
            ImGui::EndChild();
            ImGui::TableNextColumn();
            if (begin_region("##browser_items", ImVec2(0.0F, body))) {
                const float cell = std::round(browser.tile * ui_scale) + std::round(6.0F * ui_scale) * 3.0F;
                columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
                draw_browser_items(tiles);
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
                    browser.selected.clear();
            }
            ImGui::EndChild();
            ImGui::EndTable();
        }
        draw_browser_footer(tiles);
        if (browser.open) update_browser_keys(tiles, columns);
        ImGui::End();
        if (const auto target = std::exchange(browser.activate, std::nullopt); target && browser.open) {
            const auto found = std::find_if(tiles.begin(), tiles.end(), [&](const BrowserTile& tile) { return tile.id == *target; });
            if (found != tiles.end()) activate_tile(*found);
        }
        if (const auto target = std::exchange(browser.go_to, std::nullopt); target && browser.open) {
            browser.query.fill('\0');
            browser.kinds.clear();
            browser_navigate(*target);
        }
    }

    // Where a model dropped on the viewport lands: the ground plane under the pointer, or a short
    // distance along the pointer ray when the ground is not in view.
    [[nodiscard]] Vec3 viewport_drop_point(const ImVec2 pointer) const {
        const auto width = viewport_max.x - viewport_min.x;
        const auto height = viewport_max.y - viewport_min.y;
        if (width <= 0.0F || height <= 0.0F) return {};
        const double horizontal =
            2.0 * static_cast<double>(pointer.x - viewport_min.x) / static_cast<double>(width) - 1.0;
        const double vertical =
            1.0 - 2.0 * static_cast<double>(pointer.y - viewport_min.y) / static_cast<double>(height);
        const auto direction = editor_screen_ray(view.position, view.target,
                                                 view.camera.field_of_view_y_degrees,
                                                 static_cast<double>(width) / height, horizontal,
                                                 vertical);
        double distance = 10.0;
        if (direction.y < -1e-4) distance = std::min(-view.position.y / direction.y, 200.0);
        if (distance <= 0.0) distance = 10.0;
        return {view.position.x + direction.x * distance, view.position.y + direction.y * distance,
                view.position.z + direction.z * distance};
    }

    void draw_viewport_drop_target() {
        if (!viewport_visible) return;
        if (!ImGui::BeginDragDropTargetCustom(ImRect(viewport_min, viewport_max),
                                              ImGui::GetID("##viewport_drop"))) return;
        if (const auto model = accept_model_drop())
            import_model(*model, "scene", {}, viewport_drop_point(ImGui::GetMousePos()));
        if (const auto dropped = accept_template_drop())
            instantiate_template(*dropped, {}, viewport_drop_point(ImGui::GetMousePos()));
        if (const auto sound = accept_sound_drop())
            create_sound_node(*sound, viewport_drop_point(ImGui::GetMousePos()));
        ImGui::EndDragDropTarget();
    }

    // Imports into the scene, optionally under `parent` or at a world `position`, then selects it.
    void import_model(const std::string& filename, const std::string& preset = "scene",
                      const std::string& parent = {}, std::optional<Vec3> position = {}) {
        const auto imported =
            call("assets.import_model", "\"filename\":\"" + json_escape(filename) +
                                            "\",\"instantiate\":true,\"preset\":\"" + preset + '"');
        if (!imported)
            return;
        std::vector<std::string> imported_handles;
        if (const auto* object = imported->object())
            if (const auto* list = field(*object, "roots"); list && list->array())
                for (const auto& value : *list->array())
                    if (const auto* handle = value.string()) imported_handles.push_back(*handle);
        for (const auto& handle : imported_handles) {
            if (!parent.empty())
                (void)call("scene.set_parent", entity_field(handle) + ",\"parent\":\"" + parent + '"');
            if (position)
                (void)call("scene.set_transform", entity_field(handle) + ",\"px\":" +
                                                      number_text(position->x) + ",\"py\":" +
                                                      number_text(position->y) + ",\"pz\":" +
                                                      number_text(position->z));
        }
        set_status("Imported " + base_name(filename), false);
        assets_pending = true;
        refresh_pending = true;
        refresh();
        // Select the imported root so it is immediately editable; frame it unless it was placed.
        if (!imported_handles.empty()) {
            select(imported_handles.front());
            if (!position) focus_selection();
        }
    }

    void draw_history() {
        if (begin_region("##history")) {
            // Newest first. Clicking an entry rewinds to just before it by undoing repeatedly,
            // which keeps the editor using the same undo operation an agent would call.
            for (std::size_t index = 0; index < undo_labels.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                if (ImGui::Selectable(undo_labels[index].c_str(), index == 0U)) {
                    for (std::size_t step = 0; step <= index; ++step) {
                        if (!call("scene.undo").has_value()) break;
                    }
                    set_status("Rewound " + std::to_string(index + 1U) + " change(s)", false);
                    refresh_pending = true;
                }
                ImGui::PopID();
            }
            if (undo_labels.empty()) {
                ImGui::TextColored(editor_color(editor_palette().text_faint), "No changes yet.");
            }
            if (!redo_labels.empty()) {
                ImGui::Dummy(ImVec2(0.0F, 4.0F * ui_scale));
                ImGui::TextColored(editor_color(editor_palette().text_faint), "Redo");
                for (const auto& label : redo_labels) {
                    ImGui::TextColored(editor_color(editor_palette().text_dim), "%s",
                                       label.c_str());
                }
            }
        }
        ImGui::EndChild();
    }

    void draw_script_diagnostics() {
        const auto* status = scripts();
        if (!status || !boolean_or(*status, "project", false)) return;
        const auto& palette = editor_palette();
        const auto* diagnostics = field(*status, "diagnostics");
        const auto* runtime_errors = field(*status, "runtime_errors");
        const bool problems = (diagnostics && diagnostics->array() && !diagnostics->array()->empty()) ||
                              (runtime_errors && runtime_errors->array() &&
                               !runtime_errors->array()->empty());
        if (!ImGui::CollapsingHeader("Scripts", problems ? ImGuiTreeNodeFlags_DefaultOpen : 0)) return;
        const bool trusted = boolean_or(*status, "trusted", false);
        const auto state = string_or(*status, "state", "idle");
        if (!trusted) {
            ImGui::TextDisabled("Native scripts are off until you trust this project.");
            if (ImGui::Button("Trust project scripts...")) open_trust_dialog = true;
            return;
        }
        const auto behaviours = script_behaviours();
        const auto colour = state == "failed"     ? palette.danger
                            : state == "building" ? palette.warning
                                                  : palette.success;
        ImGui::TextColored(editor_color(colour), "%s", state.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%zu behaviours, build %.0f, %.1f s%s", behaviours.size(),
                            number_or(*status, "loaded_build", 0.0),
                            number_or(*status, "build_seconds", 0.0),
                            boolean_or(*status, "stale", false) ? ", sources changed" : "");
        ImGui::BeginDisabled(state == "building");
        if (ImGui::Button("Build scripts")) start_script_build(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        editor_checkbox("Build on change", &auto_build_scripts);
        ImGui::SameLine();
        if (ImGui::Button("Revoke trust")) {
            if (call("scripts.trust", "\"trusted\":false")) script_status_reply.clear();
        }
        const auto error = string_or(*status, "error");
        if (state == "failed" && !error.empty())
            ImGui::TextColored(editor_color(palette.danger), "%s", error.c_str());
        ImGui::PushFont(fonts.monospace, fonts.monospace_size);
        if (diagnostics && diagnostics->array())
            for (const auto& item : *diagnostics->array()) {
                const auto* diagnostic = item.object();
                if (!diagnostic) continue;
                const auto severity = string_or(*diagnostic, "severity");
                ImGui::TextColored(editor_color(severity == "error"     ? palette.danger
                                                : severity == "warning" ? palette.warning
                                                                        : palette.text_faint),
                                   "%s:%.0f:%.0f", string_or(*diagnostic, "file").c_str(),
                                   number_or(*diagnostic, "line", 0.0),
                                   number_or(*diagnostic, "column", 0.0));
                ImGui::SameLine();
                ImGui::TextWrapped("%s", string_or(*diagnostic, "message").c_str());
            }
        if (runtime_errors && runtime_errors->array())
            for (const auto& item : *runtime_errors->array()) {
                const auto* failure = item.object();
                if (!failure) continue;
                ImGui::TextColored(editor_color(palette.danger), "%s %s",
                                   string_or(*failure, "behaviour").c_str(),
                                   string_or(*failure, "entity").c_str());
                ImGui::SameLine();
                ImGui::TextWrapped("%s: %s", string_or(*failure, "callback").c_str(),
                                   string_or(*failure, "message").c_str());
            }
        ImGui::PopFont();
        ImGui::Separator();
    }

    // Profiler panel. It reads profiler.read like any other protocol client, a few times a
    // second while visible, so the numbers it shows are the ones an agent would see.
    JsonValue profile;
    double seconds_since_profile{1.0};
    bool profile_dirty{true};
    int profile_window{120};
    bool profile_game_only{true};
    std::uint64_t profile_frame{};
    bool profile_paused{};
    bool profile_auto_paused{};
    bool profile_saw_game{};
    static constexpr std::size_t profile_history = 300U;

    [[nodiscard]] bool game_running() const {
        return runtime_status.object() && string_or(*runtime_status.object(), "mode") == "game";
    }

    void set_profiler_paused(const bool paused) {
        if (auto reply = call("profiler.set", paused ? "\"paused\":true" : "\"paused\":false");
            reply && reply->object())
            profile_paused = boolean_or(*reply->object(), "paused", paused);
        profile_auto_paused = false;
        profile_dirty = true;
    }

    // Stopping the game keeps its frames: the profiler pauses so editor frames do not push them
    // out, and resumes by itself when the next game starts.
    void update_profiler_session() {
        if (!panel_open[9]) {
            profile_saw_game = false;
            return;
        }
        const bool game = game_running();
        if (game && !profile_saw_game && profile_auto_paused) {
            set_profiler_paused(false);
            profile_frame = 0U;
        } else if (!game && profile_saw_game && !profile_paused) {
            set_profiler_paused(true);
            profile_auto_paused = true;
        }
        profile_saw_game = game;
    }

    void read_profile() {
        seconds_since_profile = 0.0;
        profile_dirty = false;
        std::string fields = "\"frames\":" + std::to_string(profile_window) +
                             ",\"history\":" + std::to_string(profile_history) +
                             ",\"game_only\":" + (profile_game_only ? "true" : "false");
        if (profile_frame != 0U) fields += ",\"frame\":" + std::to_string(profile_frame);
        if (auto reply = call("profiler.read", fields, false)) {
            profile = std::move(*reply);
            if (const auto* object = profile.object())
                profile_paused = boolean_or(*object, "paused", profile_paused);
        }
    }

    // A share of a total, drawn as a filled bar with the percentage over it.
    void share_bar(const double part, const double whole, const ImU32 colour) {
        const auto fraction = whole > 0.0 ? static_cast<float>(std::clamp(part / whole, 0.0, 1.0)) : 0.0F;
        char label[16];
        std::snprintf(label, sizeof(label), "%.1f%%", static_cast<double>(fraction) * 100.0);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight()), label);
        ImGui::PopStyleColor();
    }

    // Tables fill the panel but keep a usable height in a short dock, where the panel scrolls.
    [[nodiscard]] ImVec2 profile_table_size() const {
        return {0.0F, std::max(ImGui::GetContentRegionAvail().y, 160.0F * ui_scale)};
    }

    static std::string profile_ms(const double milliseconds) {
        char text[32];
        std::snprintf(text, sizeof(text), milliseconds >= 10.0 ? "%.1f ms" : "%.2f ms", milliseconds);
        return text;
    }

    // The Mixer panel: one strip per bus in tree order, each with a fader heard live while it is
    // dragged and saved when released, a stereo meter, mute and solo, and its effect chain.
    // Selecting an effect edits it below the strips.
    void draw_mixer() {
        const auto& palette = editor_palette();
        // Changes re-read audio_settings while the strips are drawn, and the selected effect is
        // edited after them, so everything here reads a copy that lives for the whole frame.
        const JsonValue shown = audio_settings;
        const auto* object = shown.object();
        const auto* settings = object ? field(*object, "settings") : nullptr;
        const auto* buses = settings && settings->object() ? field(*settings->object(), "buses") : nullptr;
        const auto* project = project_status.object();
        if (!project || string_or(*project, "filename").empty())
            ImGui::TextColored(editor_color(palette.warning), "Open a project to save its mixer.");
        if (!buses || !buses->array()) {
            ImGui::TextDisabled("No mixer settings yet.");
            return;
        }
        const auto* status = audio_status.object();
        const auto* levels = status ? field(*status, "buses") : nullptr;
        const auto level_of = [&](const std::string& name, const char* key) {
            if (levels && levels->array())
                for (const auto& bus : *levels->array())
                    if (const auto* entry = bus.object(); entry && string_or(*entry, "name") == name)
                        return number_or(*entry, key, minimum_audio_volume_db);
            return minimum_audio_volume_db;
        };
        // Sends a mixer change, previewing while a control is held and saving when it is let go.
        const auto send = [&](const char* method, const std::string& fields, const bool preview,
                              const std::string& message) {
            if (call(method, fields + (preview ? ",\"preview\":true" : ""))) {
                if (!preview) set_status(message, false);
                if (auto refreshed = call("audio.settings", {}, false)) audio_settings = std::move(*refreshed);
            }
        };
        // Strips fill the panel's height, the fader taking what the labels and buttons leave, and
        // the selected effect's settings sit beside them, so a short docked panel still works.
        const float strip_width = 118.0F * ui_scale;
        const auto available = ImGui::GetContentRegionAvail();
        const float fader_height = std::clamp(available.y - 120.0F * ui_scale, 48.0F * ui_scale,
                                              220.0F * ui_scale);
        const bool editing = mixer_effect >= 0;
        const float editor_width = editing ? std::min(360.0F * ui_scale, available.x * 0.45F) : 0.0F;
        ImGui::BeginChild("##strips", ImVec2(available.x - editor_width, 0.0F), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        const JsonValue::Object* selected_effect = nullptr;
        std::size_t selected_chain = 0;
        bool added = false;
        bool first = true;
        for (const auto& value : *buses->array()) {
            const auto* bus = value.object();
            if (!bus) continue;
            const auto name = string_or(*bus, "name");
            const auto quoted = "\"name\":\"" + json_escape(name) + '"';
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::PushID(name.c_str());
            ImGui::BeginChild("##strip", ImVec2(strip_width, 0.0F), ImGuiChildFlags_Borders);
            ImGui::AlignTextToFramePadding();
            ImGui::PushFont(fonts.heading, fonts.body_size);
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopFont();
            const auto parent = string_or(*bus, "parent");
            const auto toggle = [&](const char* label, const char* key, const ImU32 on_color, const char* tip) {
                const bool on = boolean_or(*bus, key, false);
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, on_color);
                if (ImGui::SmallButton(label))
                    send("audio.set_bus", quoted + ",\"" + key + "\":" + (on ? "false" : "true"), false,
                         name + (on ? " un" : " ") + key + (std::string_view(key) == "mute" ? "d" : "ed"));
                if (on) ImGui::PopStyleColor();
                note_item("mixer:bus:" + name + ':' + key);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
            };
            // Mute and solo at the right of the name row.
            const float buttons = ImGui::CalcTextSize("MS").x + ImGui::GetStyle().FramePadding.x * 4.0F +
                                  ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - buttons));
            toggle("M", "mute", palette.danger, "Mute");
            ImGui::SameLine();
            toggle("S", "solo", palette.warning, "Solo: hear only soloed buses");
            ImGui::TextColored(editor_color(palette.text_faint), "%s",
                               parent.empty() ? "output" : ("to " + parent).c_str());
            // Fader and meter side by side.
            const auto volume = number_or(*bus, "volume_db", 0.0);
            auto& draft = drafts[ImGui::GetID("##fader")];
            const double low = minimum_audio_volume_db, high = maximum_audio_volume_db;
            auto& edit = draft.begin(volume);
            const bool moved = ImGui::VSliderScalar("##fader", ImVec2(36.0F * ui_scale, fader_height),
                                                    ImGuiDataType_Double, &edit, &low, &high,
                                                    edit <= low ? "-inf" : "%.1f");
            note_item("mixer:bus:" + name + ":fader");
            if (moved) send("audio.set_bus", quoted + ",\"volume_db\":" + number_text(edit), true, {});
            if (ImGui::IsItemDeactivatedAfterEdit())
                send("audio.set_bus", quoted + ",\"volume_db\":" + number_text(edit), false,
                     name + " volume set");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to hear the change; Ctrl+click to type");
            draft.finish(ImGui::IsItemActive());
            ImGui::SameLine();
            {
                const auto origin = ImGui::GetCursorScreenPos();
                const float bar = 8.0F * ui_scale;
                ImGui::Dummy(ImVec2(bar * 2.0F + 3.0F * ui_scale, fader_height));
                auto* draw = ImGui::GetWindowDrawList();
                const auto meter = [&](const float x, const double db) {
                    const float fill = static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
                    draw->AddRectFilled(ImVec2(x, origin.y), ImVec2(x + bar, origin.y + fader_height), palette.input);
                    const ImU32 color = db > -1.0 ? palette.danger : db > -12.0 ? palette.warning : palette.success;
                    if (fill > 0.0F)
                        draw->AddRectFilled(ImVec2(x, origin.y + fader_height * (1.0F - fill)),
                                            ImVec2(x + bar, origin.y + fader_height), color);
                };
                meter(origin.x, level_of(name, "peak_left_db"));
                meter(origin.x + bar + 3.0F * ui_scale, level_of(name, "peak_right_db"));
            }
            // The effect chain, then a picker to add to it.
            const auto* effects = field(*bus, "effects");
            std::size_t count = 0;
            if (effects && effects->array()) {
                count = effects->array()->size();
                for (std::size_t index = 0; index < count; ++index) {
                    const auto* effect = (*effects->array())[index].object();
                    if (!effect) continue;
                    ImGui::PushID(static_cast<int>(index));
                    const bool enabled = boolean_or(*effect, "enabled", true);
                    const bool chosen = mixer_bus == name && mixer_effect == static_cast<int>(index);
                    if (!enabled) ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_faint));
                    if (ImGui::Selectable(effect_label(string_or(*effect, "type")), chosen)) {
                        mixer_bus = name;
                        mixer_effect = static_cast<int>(index);
                    }
                    if (!enabled) ImGui::PopStyleColor();
                    note_item("mixer:bus:" + name + ":effect:" + std::to_string(index));
                    if (chosen) {
                        selected_effect = effect;
                        selected_chain = count;
                    }
                    ImGui::PopID();
                }
            }
            if (count < maximum_bus_effects) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##add", "+ Effect", ImGuiComboFlags_NoArrowButton)) {
                    for (const char* type : {"reverb", "delay", "eq", "compressor", "limiter", "lowpass", "highpass"}) {
                        if (ImGui::Selectable(effect_label(type))) {
                            send("audio.set_effect", "\"bus\":\"" + json_escape(name) + "\",\"type\":\"" + type + '"',
                                 false, std::string(effect_label(type)) + " added to " + name);
                            mixer_bus = name;
                            mixer_effect = static_cast<int>(count);
                            added = true;
                        }
                        note_item("mixer:bus:" + name + ":add:" + type);
                    }
                    ImGui::EndCombo();
                }
                note_item("mixer:bus:" + name + ":add");
            }
            ImGui::EndChild();
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (!selected_effect) {
            // A new effect is not in this frame's listing yet; keep it selected for the next.
            if (!added) mixer_effect = -1;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Select an effect in a strip to edit it. Effects run top to bottom, "
                                  "before the bus volume.");
            return;
        }
        if (!editing) return; // Chosen this frame; its settings appear next frame.
        ImGui::SameLine();
        ImGui::BeginChild("##effect", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders);
        draw_mixer_effect(*selected_effect, selected_chain, send);
        ImGui::EndChild();
    }

    static const char* effect_label(const std::string_view type) {
        if (type == "reverb") return "Reverb";
        if (type == "delay") return "Delay";
        if (type == "eq") return "EQ";
        if (type == "compressor") return "Compressor";
        if (type == "limiter") return "Limiter";
        if (type == "lowpass") return "Low-pass";
        if (type == "highpass") return "High-pass";
        return "Effect";
    }

    template <typename Send>
    void draw_mixer_effect(const JsonValue::Object& effect, const std::size_t chain, const Send& send) {
        const auto type = string_or(effect, "type");
        const auto target = "\"bus\":\"" + json_escape(mixer_bus) + "\",\"index\":" + std::to_string(mixer_effect);
        ImGui::SeparatorText((std::string(effect_label(type)) + " on " + mixer_bus).c_str());
        bool enabled = boolean_or(effect, "enabled", true);
        if (editor_checkbox("Enabled", &enabled))
            send("audio.set_effect", target + ",\"enabled\":" + (enabled ? "true" : "false"), false,
                 std::string(effect_label(type)) + (enabled ? " on" : " off"));
        note_item("mixer:effect:enabled");
        ImGui::SameLine();
        if (ImGui::SmallButton("Move up") && mixer_effect > 0) {
            send("audio.move_effect", target + ",\"to\":" + std::to_string(mixer_effect - 1), false, "Effect moved");
            --mixer_effect;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Move down") && static_cast<std::size_t>(mixer_effect) + 1U < chain) {
            send("audio.move_effect", target + ",\"to\":" + std::to_string(mixer_effect + 1), false, "Effect moved");
            ++mixer_effect;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            send("audio.remove_effect", target, false, std::string(effect_label(type)) + " removed");
            mixer_effect = -1;
            return;
        }
        note_item("mixer:effect:remove");
        struct Parameter {
            const char* key;
            const char* label;
            double minimum, maximum;
            const char* format;
        };
        static constexpr std::array<Parameter, 19> parameters{{
            {"room_size", "Room size", 0, 1, "%.2f"}, {"damping", "Damping", 0, 1, "%.2f"},
            {"width", "Width", 0, 1, "%.2f"}, {"pre_delay_ms", "Pre-delay", 0, 250, "%.0f ms"},
            {"time_ms", "Time", 1, 2000, "%.0f ms"}, {"feedback", "Feedback", 0, 0.95, "%.2f"},
            {"mix", "Mix", 0, 1, "%.2f"}, {"low_db", "Low", -24, 24, "%.1f dB"},
            {"mid_db", "Mid", -24, 24, "%.1f dB"}, {"mid_frequency", "Mid frequency", 100, 10000, "%.0f Hz"},
            {"high_db", "High", -24, 24, "%.1f dB"}, {"threshold_db", "Threshold", -60, 0, "%.1f dB"},
            {"ratio", "Ratio", 1, 20, "%.1f:1"}, {"attack_ms", "Attack", 0.1, 500, "%.1f ms"},
            {"release_ms", "Release", 1, 5000, "%.0f ms"}, {"makeup_db", "Makeup", 0, 24, "%.1f dB"},
            {"ceiling_db", "Ceiling", -24, 0, "%.1f dB"}, {"cutoff_hz", "Cutoff", 20, 20000, "%.0f Hz"},
            {"resonance", "Resonance", 0.1, 10, "%.2f"}}};
        ImGui::PushItemWidth(-110.0F * ui_scale);
        for (const auto& parameter : parameters) {
            const auto* value = field(effect, parameter.key);
            if (!value || !value->number()) continue;
            ImGui::PushID(parameter.key);
            auto& draft = drafts[ImGui::GetID("##parameter")];
            auto& edit = draft.begin(*value->number());
            const bool logarithmic = std::string_view(parameter.key).ends_with("_hz") ||
                                     std::string_view(parameter.key) == "mid_frequency";
            const bool moved = editor_slider(parameter.label, &edit, parameter.minimum, parameter.maximum,
                                             parameter.format, logarithmic);
            note_item(std::string("mixer:effect:param:") + parameter.key);
            const auto fields = target + ",\"" + parameter.key + "\":" + number_text(edit);
            if (moved) send("audio.set_effect", fields, true, {});
            if (ImGui::IsItemDeactivatedAfterEdit())
                send("audio.set_effect", fields, false, std::string(parameter.label) + " set");
            draft.finish(ImGui::IsItemActive());
            ImGui::PopID();
        }
        ImGui::PopItemWidth();
    }

    void draw_profiler() {
        seconds_since_profile += static_cast<double>(ImGui::GetIO().DeltaTime);
        if (profile_dirty || (!profile_paused && seconds_since_profile >= 0.25)) read_profile();
        const auto* report = profile.object();
        // The default dock along the bottom is wide and short, so the overview and the tables sit
        // side by side there and stack in a narrow panel.
        const auto available = ImGui::GetContentRegionAvail();
        if (available.x >= 760.0F * ui_scale) {
            if (begin_region("##profile_overview", ImVec2(std::floor(available.x * 0.42F), 0.0F)))
                draw_profile_overview(report, true);
            ImGui::EndChild();
            ImGui::SameLine();
            if (begin_region("##profile_details")) draw_profile_details(report);
            ImGui::EndChild();
        } else {
            draw_profile_overview(report, false);
            draw_profile_details(report);
        }
    }

    [[nodiscard]] static double profile_gpu_ms(const JsonValue::Object& report) {
        const auto* value = field(report, "gpu_ms");
        return value && value->number() ? *value->number() : -1.0;
    }

    // Controls, frame-time summary, verdict and the frame graph. A filling graph takes the height
    // left in its column.
    void draw_profile_overview(const JsonValue::Object* const report, const bool fill) {
        const auto& palette = editor_palette();
        // Controls.
        if (ImGui::Button(profile_paused ? "Resume" : "Pause")) {
            set_profiler_paused(!profile_paused);
            if (!profile_paused) profile_frame = 0U;
        }
        note_item("profiler:pause");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(profile_paused ? "Record new frames again"
                                             : "Stop recording to inspect the frames recorded so far");
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            (void)call("profiler.set", "\"clear\":true");
            profile_frame = 0U;
            profile_dirty = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0F * ui_scale);
        constexpr std::array windows{60, 120, 300, 600, 1200};
        const auto window_label = std::to_string(profile_window) + " frames";
        if (ImGui::BeginCombo("##profile_window", window_label.c_str())) {
            for (const auto frames : windows) {
                const auto label = "Last " + std::to_string(frames) + " frames";
                if (ImGui::Selectable(label.c_str(), frames == profile_window)) {
                    profile_window = frames;
                    profile_frame = 0U;
                    profile_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many recent frames the averages cover");
        ImGui::SameLine();
        if (editor_checkbox("Game frames only", &profile_game_only)) {
            profile_frame = 0U;
            profile_dirty = true;
        }
        if (profile_frame != 0U) {
            ImGui::SameLine();
            if (ImGui::Button("Back to average")) {
                profile_frame = 0U;
                profile_dirty = true;
            }
            note_item("profiler:average");
        }

        if (report == nullptr) {
            ImGui::TextDisabled("The profiler is not available in this session.");
            return;
        }
        if (profile_auto_paused && profile_paused)
            ImGui::TextColored(editor_color(palette.text_dim),
                               "Paused when the game stopped, keeping its frames. Running the game "
                               "again resumes recording.");

        const auto frames = number_or(*report, "frames", 0.0);
        const auto average = number_or(*report, "average_ms", 0.0);
        const auto cpu = number_or(*report, "cpu_ms", 0.0);
        const auto wait = number_or(*report, "wait_ms", 0.0);
        const auto gpu = profile_gpu_ms(*report);
        if (frames > 0.0) {
            ImGui::PushFont(fonts.heading, fonts.heading_size);
            if (profile_frame != 0U)
                ImGui::Text("Frame %llu  ·  %s", static_cast<unsigned long long>(profile_frame),
                            profile_ms(average).c_str());
            else
                ImGui::Text("%.0f FPS  ·  %s", number_or(*report, "fps", 0.0), profile_ms(average).c_str());
            ImGui::PopFont();
            if (profile_frame == 0U)
                ImGui::TextColored(editor_color(palette.text_dim), "p95 %s  ·  max %s  ·  %.0f frames",
                                   profile_ms(number_or(*report, "p95_ms", 0.0)).c_str(),
                                   profile_ms(number_or(*report, "maximum_ms", 0.0)).c_str(), frames);
            ImGui::TextColored(editor_color(palette.text_dim), "CPU busy %s  ·  GPU %s  ·  waiting %s",
                               profile_ms(cpu).c_str(), gpu >= 0.0 ? profile_ms(gpu).c_str() : "n/a",
                               profile_ms(wait).c_str());
            draw_profile_verdict(*report, average, cpu, gpu);
        } else if (profile_game_only) {
            ImGui::TextColored(editor_color(palette.text_dim),
                               "No game frames recorded. Run the game (F5) to profile it, or clear "
                               "\"Game frames only\" to see editor frames.");
        } else {
            ImGui::TextColored(editor_color(palette.text_dim), "No frames recorded yet.");
        }
        const float height = fill ? std::clamp(ImGui::GetContentRegionAvail().y, 64.0F * ui_scale,
                                               220.0F * ui_scale)
                                  : 84.0F * ui_scale;
        draw_profile_graph(*report, height);
    }

    void draw_profile_details(const JsonValue::Object* const report) {
        if (report == nullptr || number_or(*report, "frames", 0.0) <= 0.0) return;
        const auto average = number_or(*report, "average_ms", 0.0);
        if (!ImGui::BeginTabBar("##profiler_tabs")) return;
        if (ImGui::BeginTabItem("Hotspots")) {
            draw_profile_hotspots(*report, average);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Call tree")) {
            draw_profile_tree(*report, average);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GPU passes")) {
            draw_profile_gpu(*report, profile_gpu_ms(*report));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void draw_profile_verdict(const JsonValue::Object& report, const double average,
                              const double cpu, const double gpu) {
        const auto& palette = editor_palette();
        const auto bottleneck = string_or(report, "bottleneck");
        std::string text;
        ImU32 colour = palette.text;
        if (bottleneck == "gpu") {
            std::string pass;
            double pass_ms = -1.0;
            if (const auto* passes = field(report, "gpu_passes"); passes && passes->array())
                for (const auto& item : *passes->array())
                    if (const auto* entry = item.object(); entry && number_or(*entry, "average_ms", 0.0) > pass_ms) {
                        pass_ms = number_or(*entry, "average_ms", 0.0);
                        pass = string_or(*entry, "name");
                    }
            text = "GPU bound: the GPU needs " + profile_ms(gpu) + " of each " + profile_ms(average) + " frame.";
            if (!pass.empty()) text += " Most expensive pass: " + pass + " (" + profile_ms(pass_ms) + ").";
            colour = palette.warning;
        } else if (bottleneck == "cpu") {
            std::string hotspot;
            double hotspot_ms = 0.0;
            if (const auto* hotspots = field(report, "hotspots"); hotspots && hotspots->array())
                for (const auto& item : *hotspots->array())
                    if (const auto* entry = item.object(); entry && string_or(*entry, "kind") == "work") {
                        hotspot = string_or(*entry, "name");
                        hotspot_ms = number_or(*entry, "self_ms", 0.0);
                        break;
                    }
            text = "CPU bound: the CPU works " + profile_ms(cpu) + " of each " + profile_ms(average) + " frame.";
            if (!hotspot.empty()) text += " Biggest hotspot: " + hotspot + " (" + profile_ms(hotspot_ms) + ").";
            colour = palette.warning;
        } else {
            // Name the wait that fills the frame: vsync shows as Present or Acquire, the editor's
            // 250 FPS cap as Frame pacing and the game's limit as Frame rate limit.
            std::string waiting = "the display";
            if (const auto* hotspots = field(report, "hotspots"); hotspots && hotspots->array())
                for (const auto& item : *hotspots->array())
                    if (const auto* entry = item.object(); entry && string_or(*entry, "kind") == "wait") {
                        waiting = string_or(*entry, "name");
                        break;
                    }
            text = "Not limited by the CPU (" + profile_ms(cpu) + ") or GPU (" +
                   (gpu >= 0.0 ? profile_ms(gpu) : std::string("n/a")) +
                   "): both finish early and the rest of the frame waits in " + waiting + ".";
            colour = palette.success;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, colour);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    }

    // Recent frame times as bars, with 60 and 30 FPS guides. Clicking a bar inspects that frame.
    void draw_profile_graph(const JsonValue::Object& report, const float height) {
        const auto& palette = editor_palette();
        const auto* history = field(report, "history");
        if (!history || !history->array() || history->array()->empty()) return;
        const auto& samples = *history->array();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0F);
        const auto origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##profile_graph", ImVec2(width, height));
        note_item("profiler:graph");
        const bool hovered = ImGui::IsItemHovered();
        auto* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), palette.input);
        double peak = 0.0;
        for (const auto& sample : samples)
            if (const auto* entry = sample.object()) peak = std::max(peak, number_or(*entry, "ms", 0.0));
        const auto scale = static_cast<float>(std::clamp(peak * 1.15, 36.0, 250.0));
        const auto y_for = [&](const double milliseconds) {
            return origin.y + height - height * std::min(1.0F, static_cast<float>(milliseconds) / scale);
        };
        const auto slots = static_cast<float>(std::max<std::size_t>(samples.size(), profile_history));
        const float bar = width / slots;
        const float first = origin.x + width - bar * static_cast<float>(samples.size());
        const auto mouse = ImGui::GetIO().MousePos;
        std::optional<std::size_t> hovered_index;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto* entry = samples[index].object();
            if (!entry) continue;
            const auto milliseconds = number_or(*entry, "ms", 0.0);
            const auto frame = static_cast<std::uint64_t>(number_or(*entry, "frame", 0.0));
            const bool game = boolean_or(*entry, "game", false);
            const float left = first + bar * static_cast<float>(index);
            const float right = left + std::max(bar - 1.0F, 1.0F);
            const bool over = hovered && mouse.x >= left && mouse.x < left + bar;
            if (over) hovered_index = index;
            ImU32 colour = milliseconds > 33.4   ? palette.danger
                           : milliseconds > 16.8 ? palette.warning
                           : game               ? palette.accent_hovered
                                                : palette.surface_active;
            if (profile_game_only && !game) colour = palette.surface_hovered;
            if (frame == profile_frame || over) colour = palette.text;
            draw->AddRectFilled(ImVec2(left, y_for(milliseconds)), ImVec2(right, origin.y + height), colour);
            if (const auto* gpu = field(*entry, "gpu_ms"); gpu && gpu->number()) {
                const float y = y_for(*gpu->number());
                draw->AddLine(ImVec2(left, y), ImVec2(right, y), palette.success);
            }
        }
        for (const auto [milliseconds, label] : {std::pair{1000.0 / 60.0, "60 FPS"}, std::pair{1000.0 / 30.0, "30 FPS"}}) {
            if (milliseconds > scale) continue;
            const float y = y_for(milliseconds);
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), palette.text_faint);
            draw->AddText(ImVec2(origin.x + 4.0F * ui_scale, y - ImGui::GetTextLineHeight()),
                          palette.text_faint, label);
        }
        if (hovered_index) {
            const auto& entry = *samples[*hovered_index].object();
            const auto* gpu = field(entry, "gpu_ms");
            ImGui::SetTooltip("Frame %llu%s\n%s%s%s\nClick to inspect this frame. The green line is GPU time.",
                              static_cast<unsigned long long>(number_or(entry, "frame", 0.0)),
                              boolean_or(entry, "game", false) ? " (game)" : " (editor)",
                              profile_ms(number_or(entry, "ms", 0.0)).c_str(),
                              gpu && gpu->number() ? ", GPU " : "",
                              gpu && gpu->number() ? profile_ms(*gpu->number()).c_str() : "");
            if (ImGui::IsItemClicked()) {
                profile_frame = static_cast<std::uint64_t>(number_or(entry, "frame", 0.0));
                // Keep the frame from leaving the recording while it is inspected.
                if (!profile_paused) set_profiler_paused(true);
                profile_dirty = true;
            }
        }
    }

    void draw_profile_hotspots(const JsonValue::Object& report, const double average) {
        const auto& palette = editor_palette();
        const auto* hotspots = field(report, "hotspots");
        if (!hotspots || !hotspots->array()) return;
        constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##profile_hotspots", 5, flags, profile_table_size())) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 3.0F);
        ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Share of frame", ImGuiTableColumnFlags_WidthStretch, 2.0F);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 0.8F);
        ImGui::TableHeadersRow();
        for (const auto& item : *hotspots->array()) {
            const auto* entry = item.object();
            if (!entry) continue;
            const auto self = number_or(*entry, "self_ms", 0.0);
            if (self < 0.005) continue;
            const bool waiting = string_or(*entry, "kind") == "wait";
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const auto name = string_or(*entry, "name");
            if (waiting) ImGui::TextColored(editor_color(palette.text_dim), "%s (waiting)", name.c_str());
            else ImGui::TextUnformatted(name.c_str());
            note_item("profiler:hotspot:" + name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(self).c_str());
            ImGui::TableNextColumn();
            share_bar(self, average, waiting ? palette.surface_active : palette.accent_hovered);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(number_or(*entry, "total_ms", 0.0)).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3g", number_or(*entry, "calls", 0.0));
        }
        ImGui::EndTable();
    }

    void draw_profile_tree(const JsonValue::Object& report, const double average) {
        const auto& palette = editor_palette();
        const auto* scopes_value = field(report, "scopes");
        if (!scopes_value || !scopes_value->array() || scopes_value->array()->empty()) return;
        const auto& scopes = *scopes_value->array();
        constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##profile_tree", 6, flags, profile_table_size())) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 3.2F);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Share of frame", ImGuiTableColumnFlags_WidthStretch, 1.8F);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthStretch, 0.7F);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableHeadersRow();
        // Scopes arrive depth first with parent indices; a scope's children follow it directly.
        const auto parent_of = [&](const std::size_t index) {
            const auto* entry = scopes[index].object();
            return entry ? static_cast<std::int64_t>(number_or(*entry, "parent", -1.0)) : -2;
        };
        const auto draw_scope = [&](auto&& self, const std::size_t index) -> void {
            const auto* entry = scopes[index].object();
            if (!entry) return;
            const bool has_children = index + 1U < scopes.size() &&
                                      parent_of(index + 1U) == static_cast<std::int64_t>(index);
            const bool waiting = string_or(*entry, "kind") == "wait";
            const auto depth = number_or(*entry, "depth", 0.0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGuiTreeNodeFlags node_flags = ImGuiTreeNodeFlags_SpanFullWidth;
            if (!has_children) node_flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            if (depth < 2.0) node_flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (waiting) ImGui::PushStyleColor(ImGuiCol_Text, editor_color(palette.text_dim));
            const auto name = string_or(*entry, "name");
            const bool open = ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<std::uintptr_t>(index + 1U)),
                                                node_flags, "%s", name.c_str());
            if (waiting) ImGui::PopStyleColor();
            const auto total = number_or(*entry, "total_ms", 0.0);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(total).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(number_or(*entry, "self_ms", 0.0)).c_str());
            ImGui::TableNextColumn();
            share_bar(total, average, waiting ? palette.surface_active : palette.accent_hovered);
            ImGui::TableNextColumn();
            ImGui::Text("%.3g", number_or(*entry, "calls", 0.0));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(number_or(*entry, "max_ms", 0.0)).c_str());
            if (!has_children || !open) return;
            for (std::size_t child = index + 1U; child < scopes.size(); ++child) {
                const auto* child_entry = scopes[child].object();
                if (!child_entry || number_or(*child_entry, "depth", 0.0) <= depth) break;
                if (parent_of(child) == static_cast<std::int64_t>(index)) self(self, child);
            }
            ImGui::TreePop();
        };
        draw_scope(draw_scope, 0U);
        ImGui::EndTable();
    }

    void draw_profile_gpu(const JsonValue::Object& report, const double gpu) {
        const auto& palette = editor_palette();
        const auto* passes = field(report, "gpu_passes");
        if (!passes || !passes->array() || passes->array()->empty() || gpu < 0.0) {
            ImGui::TextColored(editor_color(palette.text_dim),
                               "No GPU timings for these frames. They need a device with timestamp "
                               "queries and arrive two frames after a frame is drawn.");
            return;
        }
        constexpr auto flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingStretchProp;
        if (!ImGui::BeginTable("##profile_gpu", 4, flags, profile_table_size())) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0F);
        ImGui::TableSetupColumn("Average", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("Share of GPU time", ImGuiTableColumnFlags_WidthStretch, 2.0F);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableHeadersRow();
        for (const auto& item : *passes->array()) {
            const auto* entry = item.object();
            if (!entry) continue;
            const auto milliseconds = number_or(*entry, "average_ms", 0.0);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(string_or(*entry, "name").c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(milliseconds).c_str());
            ImGui::TableNextColumn();
            share_bar(milliseconds, gpu, palette.accent_hovered);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(profile_ms(number_or(*entry, "max_ms", 0.0)).c_str());
        }
        ImGui::EndTable();
    }

    void draw_diagnostics() {
        const auto& palette = editor_palette();
        if (!status_message.empty()) {
            ImGui::TextColored(editor_color(status_is_error ? palette.danger : palette.success),
                               "%s", status_message.c_str());
            ImGui::Dummy(ImVec2(0.0F, 2.0F * ui_scale));
        }
        draw_script_diagnostics();
        // Log lines are data, so they get the monospaced face and a dimmed severity prefix.
        ImGui::PushFont(fonts.monospace, fonts.monospace_size);
        if (begin_region("##logs")) {
            for (const auto& line : log_lines) {
                const auto close = line.find(']');
                if (line.size() > 1U && line.front() == '[' && close != std::string::npos) {
                    const auto level = line.substr(1U, close - 1U);
                    const auto colour = level == "error"  ? palette.danger
                                        : level == "warn" ? palette.warning
                                                          : palette.text_faint;
                    ImGui::TextColored(editor_color(colour), "%s", level.c_str());
                    ImGui::SameLine(58.0F * ui_scale);
                    ImGui::TextUnformatted(line.c_str() + close + 2U);
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0F) ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndChild();
        ImGui::PopFont();
    }
};

EditorUi::EditorUi(RequestHandler request) : impl_(std::make_unique<Impl>(std::move(request))) {
    (void)impl_->call("session.auto_approval", "\"enabled\":true");
}

void EditorUi::set_attachment_picker(AttachmentPicker picker) { impl_->attachment_picker = std::move(picker); }

bool EditorUi::game_has_input() const { return impl_->game_input_focus; }

bool EditorUi::pointer_locked_for_game() const {
    return impl_->game_input_focus && impl_->capture_requested;
}

EditorUi::~EditorUi() {
    // Swapchain recreation preserves the ImGui context and layout; destruction retires them.
    impl_->capture_pointer(false, false);
    EditorUi::invalidate();
    if (impl_->sdl_backend_started) {
        ImGui_ImplSDL3_Shutdown();
        impl_->sdl_backend_started = false;
    }
    if (impl_->imgui_context_created) {
        impl_->layout.save();
        impl_->chat_media.clear();
        impl_->thumbnails.clear();
        if (impl_->preview_texture) ImGui::UnregisterUserTexture(impl_->preview_texture.get());
        impl_->preview_texture.reset();
        ImGui::DestroyContext();
        impl_->imgui_context_created = false;
    }
}

bool EditorUi::initialize_headless(std::string& error) {
    if (impl_->headless) return true;
    if (impl_->imgui_context_created || !impl_->request) {
        error = "headless editor needs a request handler and no existing window context";
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    impl_->imgui_context_created = impl_->headless = true;
    impl_->layout.initialize(".relay/headless-layout.ini");
    // Headless editors never reach the desktop; tests install their own handler to observe calls.
    impl_->file_browser = {};
    impl_->bind_preferences();
    impl_->fonts = load_editor_fonts(1.0F);
    apply_editor_theme(1.0F);
    ImGui::GetStyle().FontSizeBase = impl_->fonts.body_size;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::GetIO().Fonts->SetTexID(ImTextureID{1});
    ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
    error.clear();
    return true;
}

bool EditorUi::initialize(const OverlayContext& context, std::string& error) {
    if (impl_->headless) { error = "headless editor cannot attach a window"; return false; }
    if (impl_->vulkan_backend_started) return true;
    if (!impl_->request) {
        error = "editor UI has no request handler";
        return false;
    }
    if (context.device == VK_NULL_HANDLE || context.render_pass == VK_NULL_HANDLE) {
        error = "editor UI needs a live device and render pass";
        return false;
    }
    impl_->device = context.device;
    impl_->sdl_window = static_cast<SDL_Window*>(context.sdl_window);

    if (!impl_->imgui_context_created) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        impl_->layout.initialize();
        impl_->bind_preferences();
        impl_->fonts = load_editor_fonts(1.0F);
        apply_editor_theme(1.0F);
        ImGui::GetStyle().FontSizeBase = impl_->fonts.body_size;
        impl_->imgui_context_created = true;
    }
    if (!impl_->sdl_backend_started) {
        if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(context.sdl_window))) {
            error = "could not start the SDL3 ImGui backend";
            return false;
        }
        impl_->sdl_backend_started = true;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = context.api_version;
    info.Instance = context.instance;
    info.PhysicalDevice = context.physical_device;
    info.Device = context.device;
    info.QueueFamily = context.graphics_family;
    info.Queue = context.graphics_queue;
    info.RenderPass = context.render_pass;
    info.MinImageCount = std::max(context.frames_in_flight, 2U);
    info.ImageCount = std::max(context.image_count, info.MinImageCount);
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    // Letting the backend own a small descriptor pool keeps UI allocations out of the engine's
    // fixed bindless texture table.
    info.DescriptorPoolSize = 256U;
    if (!ImGui_ImplVulkan_Init(&info)) {
        error = "could not start the Vulkan ImGui backend";
        return false;
    }
    impl_->vulkan_backend_started = true;
    return true;
}

void EditorUi::process_actions() {
    // GPU capture submits its own frame. Calling it from build() would reuse the outer draw's
    // already-acquired swapchain image/semaphore. Drain only from the desktop loop after present.
    auto actions = std::move(impl_->deferred_actions);
    impl_->deferred_actions.clear();
    for (const auto& action : actions) impl_->mutate(action.method, action.fields, action.success);
}

void EditorUi::set_panel_visible(const std::string_view name, const bool visible) {
    for (std::size_t i = 0; i < panel_names.size(); ++i)
        if (panel_names[i] == name) impl_->panel_open[i] = visible;
}

void EditorUi::set_file_browser_handler(FileBrowserHandler handler) {
    impl_->file_browser = std::move(handler);
}

bool EditorUi::panel_visible(const std::string_view name) const {
    for (std::size_t i = 0; i < panel_names.size(); ++i)
        if (panel_names[i] == name) return impl_->panel_open[i];
    return false;
}

bool EditorUi::open_project(const std::string_view filename) {
    return impl_->open_or_create_project(false, std::string(filename));
}

void EditorUi::invalidate() {
    if (impl_->frame_open) {
        ImGui::EndFrame();
        impl_->frame_open = false;
    }
    if (impl_->vulkan_backend_started) {
        if (impl_->device != VK_NULL_HANDLE) vkDeviceWaitIdle(impl_->device);
        ImGui_ImplVulkan_Shutdown();
        impl_->vulkan_backend_started = false;
        // The docking renderer's shutdown destroys platform windows, including the main
        // viewport's SDL registration. Recreate both backends together so input still resolves
        // to the main viewport after swapchain recreation.
        if (impl_->sdl_backend_started) {
            ImGui_ImplSDL3_Shutdown();
            impl_->sdl_backend_started = false;
        }
    }
}

bool EditorUi::handle_event(const void* const sdl_event) {
    if (!impl_->imgui_context_created) return false;
    const auto* event = static_cast<const SDL_Event*>(sdl_event);
    if (event->type == SDL_EVENT_QUIT) {
        impl_->discarding_action(Impl::PendingAction::quit);
        return true;
    }
    // A binding capture in Game Configuration takes the next control before anything else sees it.
    if (impl_->binding_capture.active) {
        const auto type = event->type;
        if (type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE) {
            impl_->cancel_binding_capture();
            return true;
        }
        if (type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            const auto origin = ImGui::GetMainViewport()->Pos;
            const float x = event->button.x + origin.x, y = event->button.y + origin.y;
            const auto& zone = impl_->binding_capture.zone;
            if (x < zone[0] || y < zone[1] || x > zone[2] || y > zone[3]) {
                // Clicking elsewhere, including Cancel, abandons the capture and acts normally.
                impl_->cancel_binding_capture();
            } else if (const auto control = sdl_binding_control(*event)) {
                impl_->finish_binding_capture(*control);
                return true;
            }
        } else if (const auto control = sdl_binding_control(*event)) {
            impl_->finish_binding_capture(*control);
            return true;
        }
        if (impl_->binding_capture.active &&
            (type == SDL_EVENT_KEY_DOWN || type == SDL_EVENT_KEY_UP || type == SDL_EVENT_TEXT_INPUT))
            return true;
    }
    const bool playing = impl_->runtime_status.object() &&
                         string_or(*impl_->runtime_status.object(), "mode") == "game";
    if ((event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) &&
        (event->key.scancode == SDL_SCANCODE_F5 || event->key.scancode == SDL_SCANCODE_F8) &&
        !(event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT | SDL_KMOD_GUI))) {
        if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
            if (event->key.scancode == SDL_SCANCODE_F5 && !playing) {
                impl_->request_play();
                impl_->refresh_pending = true;
            } else if (event->key.scancode == SDL_SCANCODE_F8 && playing) {
                impl_->set_game_input_focus(false);
                impl_->mutate("runtime.stop", {}, "Game stopped");
            }
        }
        return true;
    }
    if (playing && impl_->game_input_focus) {
        if ((event->type == SDL_EVENT_KEY_DOWN && event->key.scancode == SDL_SCANCODE_ESCAPE) ||
            event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            impl_->set_game_input_focus(false);
            return event->type == SDL_EVENT_KEY_DOWN;
        }
        // While the game has input, the editor does not react to it.
        switch (event->type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return false;
        default:
            break;
        }
    }
    if (event->type == SDL_EVENT_MOUSE_MOTION && impl_->mouse_captured) {
        // ImGui stays at the viewport anchor while freelook uses unbounded relative motion.
        impl_->relative_delta.x += event->motion.xrel;
        impl_->relative_delta.y += event->motion.yrel;
        return true;
    }
    if (event->type >= SDL_EVENT_DROP_FILE && event->type <= SDL_EVENT_DROP_POSITION) {
        if (event->type == SDL_EVENT_DROP_POSITION || event->type == SDL_EVENT_DROP_FILE) {
            const auto& rect = impl_->composer_rect;
            const auto origin = ImGui::GetMainViewport()->Pos;
            const float x = event->drop.x + origin.x, y = event->drop.y + origin.y;
            impl_->drop_hover = !impl_->chat_busy && !impl_->chat_media.viewer_open() && rect && x >= (*rect)[0] && y >= (*rect)[1] && x <= (*rect)[2] && y <= (*rect)[3];
            if (event->type == SDL_EVENT_DROP_FILE && impl_->drop_hover && event->drop.data) {
                std::lock_guard lock(impl_->attachment_inbox->mutex);
                impl_->attachment_inbox->paths.emplace_back(event->drop.data);
            }
        }
        if (event->type == SDL_EVENT_DROP_COMPLETE) impl_->drop_hover = false;
        return impl_->drop_hover;
    }
    // A click on the viewport hands input to the game; until then the editor keeps it.
    if (playing && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && impl_->viewport_hovered &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
        impl_->set_game_input_focus(true);
        return false;
    }
    // During Run Game without input focus, keyboard and mouse stay with the editor.
    const bool withheld = playing && (event->type == SDL_EVENT_KEY_DOWN ||
                                      event->type == SDL_EVENT_KEY_UP ||
                                      event->type == SDL_EVENT_MOUSE_MOTION ||
                                      event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                      event->type == SDL_EVENT_MOUSE_BUTTON_UP ||
                                      event->type == SDL_EVENT_MOUSE_WHEEL);
    if (impl_->headless) return withheld;
    ImGui_ImplSDL3_ProcessEvent(event);
    const auto& io = ImGui::GetIO();
    const bool game = playing;
    if (withheld) return true;
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        // Editor-view pointer input belongs to navigation, selection and gizmos. Scene-camera mode
        // leaves viewport input available to the game while panels still capture their own events.
        return io.WantCaptureMouse || (impl_->camera_enabled && !game);
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const auto key = event->key.key;
        if (event->type == SDL_EVENT_KEY_UP && impl_->editor_keys.erase(key)) return true;
        // Editor view is an authoring surface. Scene-camera mode forwards gameplay keys;
        // track owned releases so switching modes mid-press cannot leave the game with half a key.
        const bool owned = impl_->camera_enabled && !game;
        if (owned && event->type == SDL_EVENT_KEY_DOWN) impl_->editor_keys.insert(key);
        return io.WantCaptureKeyboard || owned;
    }
    case SDL_EVENT_TEXT_INPUT:
        return io.WantCaptureKeyboard;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        impl_->freelook_latched = false;
        impl_->capture_pointer(false, false);
        impl_->editor_keys.clear();
        return false;
    default:
        return false;
    }
}

void EditorUi::build(const std::uint32_t width, const std::uint32_t height) {
    if (!impl_->vulkan_backend_started && !impl_->headless) return;

    impl_->preview_request = std::exchange(impl_->preview_wanted, {});
    // Thumbnails take the renderer's material preview when the Inspector does not need it.
    if (impl_->preview_request.empty()) {
        impl_->preview_request = impl_->thumbnails.material_request();
        if (!impl_->preview_request.empty() && impl_->preview_request == impl_->last_preview.path) {
            const auto& last = impl_->last_preview;
            impl_->thumbnails.store_material(last.path, last.width, last.height, last.rgba);
            impl_->preview_request = impl_->thumbnails.material_request();
        }
    }
    if (impl_->headless) {
        impl_->headless_items.clear();
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    } else {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
    }
    // SDL's display scale is in physical pixels. ImGui lays out in window coordinates,
    // and its renderer already applies framebuffer density (including fractional Wayland DPI).
    // Use only the remaining content scale here to avoid applying desktop scaling twice.
    const float density = impl_->headless ? 1.0F : SDL_GetWindowPixelDensity(impl_->sdl_window);
    const float display_scale = impl_->headless ? 1.0F : SDL_GetWindowDisplayScale(impl_->sdl_window);
    const float scale = density > 0.0F && display_scale > 0.0F
                            ? display_scale / density : 1.0F;
    if (std::abs(scale - impl_->ui_scale) > 0.001F) {
        impl_->ui_scale = scale;
        apply_editor_theme(scale);
        ImGui::GetStyle().FontSizeBase = impl_->fonts.body_size;
        ImGui::GetStyle().FontScaleDpi = scale;
    }
    impl_->composer_rect.reset();
    // A drag change the inspector chose not to send must not tag a later, unrelated edit.
    impl_->pending_gesture = 0;
    ImGui::NewFrame();
    impl_->thumbnails.begin_frame(impl_->headless);
    ImGuizmo::BeginFrame();
    impl_->frame_open = true;

    const auto delta = static_cast<double>(ImGui::GetIO().DeltaTime);
    impl_->seconds_since_refresh += delta;
    impl_->seconds_since_assets += delta;
    if (impl_->refresh_pending) impl_->refresh();
    else if (impl_->refresh_stage >= 0 || impl_->seconds_since_refresh >= impl_->refresh_interval())
        impl_->advance_refresh();

    (void)width;
    (void)height;
    impl_->update_window_title();
    impl_->draw_menu_bar();
    const auto toolbar_height = 28.0F * impl_->ui_scale +
                                2.0F * ImGui::GetStyle().WindowPadding.y;
    if (ImGui::BeginViewportSideBar("##EditorToolbar", ImGui::GetMainViewport(), ImGuiDir_Up,
                                  toolbar_height, ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse))
        impl_->draw_toolbar();
    ImGui::End();
    impl_->layout.build(impl_->ui_scale);
    // Panels appearing at startup would each take focus, and a focused docked panel becomes its
    // node's selected tab, overriding the saved one. Only panels opened later take focus.
    const ImGuiWindowFlags panel_flags =
        ImGuiWindowFlags_NoCollapse |
        (impl_->startup_frames < 2 ? ImGuiWindowFlags_NoFocusOnAppearing : ImGuiWindowFlags_None);
    if (impl_->startup_frames < 2) ++impl_->startup_frames;
    const auto panel = [&](const char* name, std::size_t index) {
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(160.0F * impl_->ui_scale, 80.0F * impl_->ui_scale), ImVec2(FLT_MAX, FLT_MAX));
        return ImGui::Begin(name, &impl_->panel_open[index], panel_flags);
    };
    impl_->update_slow_click();
    if (impl_->panel_open[0]) {
        if (panel("Hierarchy", 0)) {
            impl_->draw_hierarchy();
        }
        ImGui::End();
    }
    if (impl_->panel_open[1]) {
        if (panel("Inspector", 1)) {
            impl_->draw_inspector();
        }
        ImGui::End();
    }
    if (impl_->panel_open[2]) {
        if (panel("Assets", 2)) {
            impl_->draw_assets();
        }
        ImGui::End();
    }
    if (impl_->panel_open[3]) {
        if (panel("History", 3)) {
            impl_->draw_history();
        }
        ImGui::End();
    }
    if (impl_->panel_open[4]) {
        if (panel("Diagnostics", 4)) {
            impl_->draw_diagnostics();
        }
        ImGui::End();
    }

    if (impl_->panel_open[5]) {
        impl_->viewport_visible = ImGui::Begin("Viewport", &impl_->panel_open[5],
                                               panel_flags | ImGuiWindowFlags_NoScrollbar |
                                                   ImGuiWindowFlags_NoScrollWithMouse);
        impl_->viewport_hovered =
            impl_->viewport_visible &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (impl_->viewport_visible) {
            const auto origin = ImGui::GetCursorScreenPos();
            const auto available = ImGui::GetContentRegionAvail();
            const auto display = ImGui::GetIO().DisplaySize;
            impl_->viewport_min = {std::clamp(origin.x, 0.0F, display.x),
                                   std::clamp(origin.y, 0.0F, display.y)};
            impl_->viewport_max = {
                std::clamp(origin.x + available.x, impl_->viewport_min.x, display.x),
                std::clamp(origin.y + available.y, impl_->viewport_min.y, display.y)};
            impl_->viewport = {impl_->viewport_min.x / display.x, impl_->viewport_min.y / display.y,
                               (impl_->viewport_max.x - impl_->viewport_min.x) / display.x,
                               (impl_->viewport_max.y - impl_->viewport_min.y) / display.y};
            impl_->viewport_visible = impl_->viewport_max.x > impl_->viewport_min.x &&
                                      impl_->viewport_max.y > impl_->viewport_min.y;
            if (impl_->headless)
                impl_->headless_items["viewport"] = {impl_->viewport_min.x, impl_->viewport_min.y,
                                                     impl_->viewport_max.x, impl_->viewport_max.y};
            impl_->viewport_draw_list = ImGui::GetWindowDrawList();
            if (impl_->viewport_visible) {
                impl_->viewport_draw_list->AddCallback(Impl::draw_scene_callback, impl_.get());
                impl_->viewport_draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }
        impl_->draw_viewport_drop_target();
        impl_->update_camera_input();
        if (!impl_->chat_media.viewer_open()) impl_->update_shortcuts();
        impl_->update_view();
        impl_->draw_collider_wireframes();
        impl_->draw_audio_shapes();
        impl_->draw_scene_nodes();
        impl_->draw_game_input_hint();
        impl_->draw_gizmo();
        impl_->draw_viewport_fps();
        impl_->update_selection_input();
        ImGui::End();
    } else {
        impl_->viewport_visible = false;
        impl_->viewport_hovered = false;
        impl_->freelook_latched = false;
        impl_->capture_pointer(false);
        if (!impl_->chat_media.viewer_open()) impl_->update_shortcuts();
    }
    if (impl_->panel_open[6]) {
        if (panel("Timeline", 6)) impl_->draw_timeline();
        ImGui::End();
    }
    if (!impl_->panel_open[6]) impl_->timeline_scrub_targets.clear();
    if (impl_->panel_open[7]) {
        if (panel("Project", 7)) impl_->draw_project();
        ImGui::End();
    }
    if (impl_->panel_open[8]) {
        if (impl_->agent_expand_pending) {
            impl_->agent_expand_pending = false;
            const auto* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowDockID(0);
            const auto size = ImVec2(std::min(580 * impl_->ui_scale, viewport->WorkSize.x * .8F), viewport->WorkSize.y * .92F);
            ImGui::SetNextWindowSize(size);
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - size.x - 16 * impl_->ui_scale, viewport->WorkPos.y + 16 * impl_->ui_scale));
        }
        if (panel("Agent", 8)) impl_->draw_agent();
        ImGui::End();
    }
    impl_->update_profiler_session();
    if (impl_->panel_open[9]) {
        // Layouts saved before the profiler existed have no place for it; it joins Diagnostics.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Profiler")))
            if (const auto* diagnostics = ImGui::FindWindowByName("Diagnostics");
                diagnostics && diagnostics->DockId)
                ImGui::SetNextWindowDockID(diagnostics->DockId, ImGuiCond_FirstUseEver);
        if (panel("Profiler", 9)) impl_->draw_profiler();
        ImGui::End();
    }
    if (impl_->panel_open[11]) {
        // Node graphs need room: the shader editor opens as a tab beside the viewport the first time.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Shader Editor")))
            if (const auto* viewport_window = ImGui::FindWindowByName("Viewport");
                viewport_window && viewport_window->DockId)
                ImGui::SetNextWindowDockID(viewport_window->DockId, ImGuiCond_FirstUseEver);
        if (impl_->shader_editor_focus_pending) {
            ImGui::SetNextWindowFocus();
            impl_->shader_editor_focus_pending = false;
        }
        if (panel("Shader Editor", 11)) impl_->draw_shader_editor();
        else impl_->shader_editor_focused = false;
        ImGui::End();
    } else {
        impl_->shader_editor_focused = false;
    }
    if (impl_->panel_open[10]) {
        // Like the profiler, the mixer joins Diagnostics the first time it opens.
        if (!ImGui::FindWindowSettingsByID(ImHashStr("Mixer")))
            if (const auto* diagnostics = ImGui::FindWindowByName("Diagnostics");
                diagnostics && diagnostics->DockId)
                ImGui::SetNextWindowDockID(diagnostics->DockId, ImGuiCond_FirstUseEver);
        if (panel("Mixer", 10)) impl_->draw_mixer();
        ImGui::End();
    }
    impl_->draw_dialogs();
    impl_->draw_game_config();
    impl_->draw_asset_browser();

    impl_->chat_media.draw_viewer(impl_->headless);
    ImGui::Render();
    impl_->frame_open = false;
}

std::string EditorUi::handle_camera_request(std::string_view request) {
    JsonParser parser(request); const auto parsed = parser.parse();
    if (!parsed || !parsed->object()) return "{\"id\":0,\"ok\":false,\"error\":\"invalid camera request\"}";
    const auto& fields = *parsed->object();
    const auto id = number_or(fields, "id", 0);
    const auto prefix = "{\"id\":" + number_text(id);
    const auto method = string_or(fields, "method", "");
    if (method == "editor.camera.set") {
        auto& camera = impl_->navigation_camera;
        camera.target.x = number_or(fields, "target_x", camera.target.x);
        camera.target.y = number_or(fields, "target_y", camera.target.y);
        camera.target.z = number_or(fields, "target_z", camera.target.z);
        camera.yaw = number_or(fields, "yaw", camera.yaw); camera.pitch = number_or(fields, "pitch", camera.pitch);
        camera.distance = number_or(fields, "distance", camera.distance);
        impl_->camera_enabled = string_or(fields, "mode", "inspector") == "inspector";
        impl_->freelook_latched = false;
    } else if (method == "editor.camera.frame") {
        const auto bounds = impl_->call("scene.bounds", Impl::entity_field(string_or(fields, "entity", "")));
        if (!bounds || !bounds->object()) return prefix + ",\"ok\":false,\"error\":\"cannot frame invalid entity\"}";
        const auto lo = editor_vector(*bounds->object(), "minimum", {{0, 0, 0}}), hi = editor_vector(*bounds->object(), "maximum", {{0, 0, 0}});
        const double viewport_width = impl_->viewport_max.x - impl_->viewport_min.x, viewport_height = impl_->viewport_max.y - impl_->viewport_min.y;
        impl_->navigation_camera.frame({lo[0], lo[1], lo[2]}, {hi[0], hi[1], hi[2]}, boolean_or(*bounds->object(), "has_geometry", false), 60,
            viewport_height > 0 ? viewport_width / viewport_height : 1);
        impl_->camera_enabled = true; impl_->freelook_latched = false;
    }
    impl_->update_view();
    const auto& camera = impl_->navigation_camera; const auto eye = camera.position();
    return prefix + ",\"ok\":true,\"result\":{\"mode\":\"" + (impl_->camera_enabled ? "inspector" : "scene") +
        "\",\"target\":{\"x\":" + number_text(camera.target.x) + ",\"y\":" + number_text(camera.target.y) + ",\"z\":" + number_text(camera.target.z) +
        "},\"position\":{\"x\":" + number_text(eye.x) + ",\"y\":" + number_text(eye.y) + ",\"z\":" + number_text(eye.z) +
        "},\"yaw\":" + number_text(camera.yaw) + ",\"pitch\":" + number_text(camera.pitch) + ",\"distance\":" + number_text(camera.distance) + ",\"capture_source\":\"vulkan\"}}";
}

Entity EditorUi::selected_entity() const {
    return impl_->camera_enabled ? Entity::parse(impl_->selection).value_or(Entity{}) : Entity{};
}

std::optional<std::array<float, 4>> EditorUi::headless_item_rect(std::string_view key) const {
    const auto found = impl_->headless_items.find(key);
    return found == impl_->headless_items.end() ? std::nullopt : std::optional{found->second};
}

std::vector<Entity> EditorUi::selected_entities() const {
    std::vector<Entity> result;
    if (impl_->camera_enabled &&
        (!impl_->runtime_status.object() ||
         string_or(*impl_->runtime_status.object(), "mode") != "game"))
        for (const auto& handle : impl_->selections.handles)
            if (const auto entity = Entity::parse(handle)) result.push_back(*entity);
    return result;
}

std::string EditorUi::material_preview_request() const { return impl_->preview_request; }

void EditorUi::material_preview_ready(const std::string& path, const std::uint32_t width, const std::uint32_t height,
                                      std::vector<std::uint8_t> rgba) {
    if (impl_->imgui_context_created) impl_->material_preview_ready(path, width, height, std::move(rgba));
}

bool EditorUi::ground_grid_visible() const {
    return impl_->camera_enabled && impl_->grid_enabled &&
           (!impl_->runtime_status.object() ||
            string_or(*impl_->runtime_status.object(), "mode") != "game");
}

EditorViewport EditorUi::scene_viewport() const { return impl_->viewport; }

const ViewOverride* EditorUi::view_override() const {
    return impl_->camera_enabled &&
                   (!impl_->runtime_status.object() ||
                    string_or(*impl_->runtime_status.object(), "mode") != "game")
               ? &impl_->view : nullptr;
}

void EditorUi::record(const VkCommandBuffer commands, const std::function<void()>& draw_scene) {
    if (!impl_->vulkan_backend_started)
        return;
    auto* const draw_data = ImGui::GetDrawData();
    if (draw_data == nullptr)
        return;
    impl_->scene_recorder = &draw_scene;
    ImGui_ImplVulkan_RenderDrawData(draw_data, commands);
    impl_->scene_recorder = nullptr;
}

} // namespace relay
