#include "relay/editor/editor_ui.hpp"

#include "relay/core/json.hpp"
#include "relay/editor/editor_camera.hpp"
#include "relay/editor/editor_layout.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_state.hpp"
#include "relay/editor/editor_theme.hpp"
#include "relay/render/scene_render.hpp"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <vector>

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

    // Panel chrome. ImGui's own title bars are switched off so headings can use the heading face
    // and a consistent accent marker instead of the default centred window caption.
    void panel_header(const char* const title) {
        const auto& palette = editor_palette();
        auto* list = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetCursorScreenPos();
        const auto height = ImGui::GetTextLineHeight();
        // A short accent bar reads as a section marker without adding another border.
        list->AddRectFilled(ImVec2(origin.x, origin.y + height * 0.12F),
                            ImVec2(origin.x + 3.0F * ui_scale, origin.y + height * 0.88F),
                            palette.accent, 1.5F * ui_scale);
        ImGui::Indent(9.0F * ui_scale);
        ImGui::PushFont(fonts.heading, fonts.heading_size);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Unindent(9.0F * ui_scale);
        ImGui::Spacing();
        const auto width = ImGui::GetContentRegionAvail().x;
        const auto rule = ImGui::GetCursorScreenPos();
        list->AddLine(ImVec2(rule.x, rule.y), ImVec2(rule.x + width, rule.y), palette.border_soft,
                      1.0F);
        ImGui::Dummy(ImVec2(0.0F, 3.0F * ui_scale));
    }

    enum class ToolIcon { play, pause, step, undo, redo, camera, focus, move, rotate, scale, local, world };
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
        if (icon == ToolIcon::pause) {
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
            draw->AddCircle(point(16, 14), 2.0F * ui_scale, color);
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
            draw->PathArcTo(point(16, 14), 8.0F * ui_scale, 0.2F, 5.2F, 20);
            draw->PathStroke(color, 0, 1.6F * ui_scale);
            line(20, 7, 20, 3);
            line(20, 7, 25, 7);
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

    bool drag_scalar(const char* label, double& current, float speed, const char* format = "%.6f") {
        auto& draft = drafts[ImGui::GetID(label)];
        ImGui::DragScalar(label, ImGuiDataType_Double, &draft.begin(current), speed, nullptr,
                          nullptr, format);
        const bool commit = ImGui::IsItemDeactivatedAfterEdit();
        current = draft.value;
        draft.finish(ImGui::IsItemActive());
        return commit;
    }

    // Keep the draft stable across refreshes, but apply each change for live pose scrubbing.
    bool slider_scalar(const char* label, double& current, const double minimum,
                       const double maximum, const char* format) {
        auto& draft = drafts[ImGui::GetID(label)];
        const bool changed = ImGui::SliderScalar(
            label, ImGuiDataType_Double, &draft.begin(current), &minimum, &maximum, format);
        if (ImGui::IsItemActivated()) ++animation_gesture;
        current = std::clamp(draft.value, minimum, maximum);
        draft.finish(ImGui::IsItemActive());
        return changed;
    }

    unsigned drag_vector3(const char* label, std::array<double, 3>& value, float speed,
                          float label_width) {
        constexpr std::array<const char*, 3> names{"X", "Y", "Z"};
        constexpr std::array<ImU32, 3> tints{IM_COL32(226, 109, 109, 255),
                                             IM_COL32(125, 200, 125, 255),
                                             IM_COL32(109, 156, 226, 255)};
        ImGui::PushID(label);
        row_label(label, label_width);
        const auto& style = ImGui::GetStyle();
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
    bool sdl_backend_started{false};
    bool vulkan_backend_started{false};
    bool frame_open{false};
    VkDevice device{};

    // Cached runtime state. The editor never touches Scene, SceneHistory or AssetRegistry directly.
    JsonValue scene_list;
    std::vector<const JsonValue::Object*> entities;
    std::map<std::string, std::vector<std::size_t>, std::less<>> children;
    std::vector<std::size_t> roots;
    std::map<std::string, std::size_t, std::less<>> entity_index;
    JsonValue runtime_status;
    std::vector<std::string> mesh_names;
    std::vector<std::string> material_names;
    std::deque<std::string> log_lines;
    std::uint64_t last_log_sequence{0};

    std::string selection;
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
    // never enters the undo history and is never sent over the protocol, so navigating the viewport
    // at mouse rate produces no trace entries.
    bool camera_enabled{true};
    bool grid_enabled{true};
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
    // fold into an earlier, unrelated edit of the same entity.
    std::uint64_t gizmo_gesture{0};
    std::uint64_t animation_gesture{0};
    ImGuizmo::OPERATION gizmo_operation{ImGuizmo::TRANSLATE};
    ImGuizmo::MODE gizmo_mode{ImGuizmo::LOCAL};
    // Screen-space bounds of the area left clear for the scene, in the window's pixel coordinates.
    ImVec2 viewport_min{}, viewport_max{};

    std::array<char, 129> scene_filename{"main.relay.json"};
    // The scene's current content revision and the one last written to or read from a file. They
    // only differ when there is unsaved authoring work, because undoing back to a saved state
    // restores that state's revision. Playback time never enters the history and so never counts.
    std::uint64_t scene_revision{0}, saved_revision{0};
    // False until this scene has actually been written to or opened from `scene_filename`, so the
    // title can distinguish a named file from the untitled scene the editor starts with.
    bool scene_has_file{false};
    std::string window_title;
    [[nodiscard]] bool scene_modified() const { return scene_revision != saved_revision; }

    std::array<char, 129> model_filename{"relay-pbr-golden.glb"};
    std::array<char, 129> create_name{"Entity"};
    std::string renaming;
    std::array<char, 129> rename_buffer{};
    std::vector<std::string> available_models;
    std::vector<std::string> undo_labels, redo_labels;
    int import_preset{0};
    std::array<bool, 6> panel_open{true, true, true, true, true, true};
    enum class FileAction { none, open, save_as, import, screenshot, recording };
    FileAction file_action{FileAction::none};
    // What to carry out once the user has answered the unsaved-work prompt.
    enum class PendingAction { none, new_scene, open_scene, quit };
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

    std::optional<JsonValue> call(const std::string_view method,
                                  const std::string_view fields = {}) {
        std::string line = "{\"id\":" + std::to_string(next_request_id++) + ",\"method\":\"" +
                           std::string(method) + '"';
        if (!fields.empty()) {
            line += ',';
            line += fields;
        }
        line += '}';
        const auto response = request(line);
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
            set_status(std::string(method) + ": " + string_or(*object, "error", "failed"), true);
            return std::nullopt;
        }
        const auto* result = field(*object, "result");
        if (result) {
            if (const auto revision = response_revision(*result)) scene_revision = *revision;
        }
        return result == nullptr ? JsonValue{} : *result;
    }

    // Every mutation goes through this helper so the panels cannot accidentally grow a second,
    // untraced path into the scene.
    bool mutate(const std::string_view method, const std::string_view fields,
                const std::string_view success) {
        if (!call(method, fields).has_value()) return false;
        set_status(std::string(success), false);
        refresh_pending = true;
        if (method == "scene.load" || method == "scene.undo" || method == "scene.redo")
            assets_pending = true;
        return true;
    }

    void set_status(std::string message, const bool error) {
        status_message = std::move(message);
        status_is_error = error;
    }

    static std::string entity_field(const std::string_view handle) {
        return "\"entity\":\"" + std::string(handle) + '"';
    }

    void refresh() {
        refresh_pending = false;
        seconds_since_refresh = 0.0;
        const bool periodic = seconds_since_assets >= refresh_interval_seconds;
        if (periodic) seconds_since_assets = 0.0;
        if (auto status = call("runtime.status")) runtime_status = std::move(*status);
        if (auto list = call("scene.list")) {
            scene_list = std::move(*list);
            rebuild_index();
        }
        animator_playing = false;
        if (const auto* entity = selection.empty() ? nullptr : find_entity(selection)) {
            if (const auto* animator = component(*entity, "animator"))
                animator_playing = boolean_or(*animator, "playing", false);
        }
        if (auto logs = call("logs.read", "\"after\":" + std::to_string(last_log_sequence))) {
            append_logs(*logs);
        }
        // Assets are refreshed on the same cadence rather than only after a UI-driven import,
        // because an agent sharing this runtime can import a model at any time and the human's
        // mesh and material lists must reflect that.
        if (periodic || assets_pending) refresh_assets();
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
        if (periodic || assets_pending) {
            if (auto models = call("assets.available")) {
                available_models.clear();
                if (const auto* object = models->object()) {
                    if (const auto* value = field(*object, "models"); value && value->array()) {
                        for (const auto& item : *value->array()) {
                            if (const auto* text = item.string()) available_models.push_back(*text);
                        }
                    }
                }
            }
            assets_pending = false;
        }
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
        if (!selection.empty() && find_entity(selection) == nullptr) select({});
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
        selection = handle;
    }

    void update_view() {
        view.position = navigation_camera.position();
        view.target = navigation_camera.target;
        view.camera = Camera{};
        view.camera.near_plane = 0.05;
        view.camera.far_plane = 5000.0;
    }

    [[nodiscard]] bool pointer_in_viewport() const {
        const auto pointer = ImGui::GetIO().MousePos;
        return pointer.x >= viewport_min.x && pointer.x < viewport_max.x &&
               pointer.y >= viewport_min.y && pointer.y < viewport_max.y;
    }

    void capture_pointer(bool capture, bool restore = true) {
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
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !gizmo_active && !ImGuizmo::IsOver()) {
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
        const auto minimum = editor_vector(*object, "minimum", {{0.0, 0.0, 0.0}});
        const auto maximum = editor_vector(*object, "maximum", {{0.0, 0.0, 0.0}});
        const auto* geometry = field(*object, "has_geometry");
        const bool has_geometry = geometry && geometry->boolean() && *geometry->boolean();
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
    void pick_at(const ImVec2 pointer) {
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
            selection.clear();
            set_status("Nothing under the pointer", false);
            return;
        }
        select(entity);
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
        if (selection.empty() || !camera_enabled || !viewport_visible || width <= 0.0F ||
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
        if (using_gizmo && !gizmo_active) ++gizmo_gesture;
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
            mutate("scene.set_transform", fields, "Gizmo transform");
        }
        gizmo_active = using_gizmo;
    }

    void draw_tree_node(const std::size_t index) {
        const auto* entity = entities[index];
        const auto handle = string_or(*entity, "entity");
        const auto name = string_or(*entity, "name", "Entity");
        const auto found = children.find(handle);
        const bool has_children = found != children.end() && !found->second.empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (!has_children) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (handle == selection) flags |= ImGuiTreeNodeFlags_Selected;

        const bool open = ImGui::TreeNodeEx(handle.c_str(), flags, "%s", name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) select(handle);

        // Dragging one row onto another reparents it. The drop target rejects its own subtree
        // implicitly: scene.set_parent refuses cycles, and the failure surfaces as a status
        // message.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
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
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            select(handle);
            if (ImGui::MenuItem("Add child")) {
                mutate("scene.create", "\"name\":\"Entity\",\"parent\":\"" + handle + '"',
                       "Entity created");
            }
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicate_selection();
            if (ImGui::MenuItem("Move to root")) {
                mutate("scene.set_parent", entity_field(handle) + ",\"parent\":null", "Reparented");
            }
            if (ImGui::MenuItem("Rename")) {
                renaming = handle;
                const auto length = std::min(name.size(), rename_buffer.size() - 1U);
                std::copy_n(name.begin(), length, rename_buffer.begin());
                rename_buffer[length] = '\0';
            }
            if (ImGui::MenuItem("Destroy")) {
                mutate("scene.destroy", entity_field(handle), "Entity destroyed");
            }
            ImGui::EndPopup();
        }
        if (open && has_children) {
            for (const auto child : found->second)
                draw_tree_node(child);
            ImGui::TreePop();
        }
    }

    void draw_hierarchy() {
        ImGui::SetNextItemWidth(-88.0F * ui_scale);
        ImGui::InputTextWithHint("##createname", "new entity name", create_name.data(),
                                 create_name.size());
        ImGui::SameLine();
        if (ImGui::Button("Create", ImVec2(80.0F * ui_scale, 0.0F))) {
            std::string fields = "\"name\":\"" + json_escape(create_name.data()) + '"';
            if (!selection.empty()) fields += ",\"parent\":\"" + selection + '"';
            if (const auto created = call("scene.create", fields)) {
                set_status("Entity created", false);
                refresh_pending = true;
                refresh();
                if (const auto* object = created->object()) {
                    const auto handle = string_or(*object, "entity");
                    if (!handle.empty()) select(handle);
                }
            }
        }
        if (!renaming.empty()) {
            ImGui::Separator();
            ImGui::SetNextItemWidth(-90.0F);
            const bool submitted =
                ImGui::InputText("##rename", rename_buffer.data(), rename_buffer.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool confirmed = ImGui::Button("Rename", ImVec2(80.0F, 0.0F));
            if (submitted || confirmed) {
                mutate("scene.rename",
                       entity_field(renaming) + ",\"name\":\"" + json_escape(rename_buffer.data()) +
                           '"',
                       "Entity renamed");
                renaming.clear();
            }
        }
        ImGui::Dummy(ImVec2(0.0F, 2.0F * ui_scale));
        if (begin_region("##tree")) {
            for (const auto root : roots)
                draw_tree_node(root);
        }
        ImGui::EndChild();
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
        if (!ImGui::CollapsingHeader("Camera",
                                     camera != nullptr ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            return;
        }
        if (camera == nullptr) {
            if (ImGui::Button("Add camera")) {
                mutate("scene.set_camera", entity_field(selection) + ",\"enabled\":true",
                       "Camera added");
            }
            return;
        }
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
        if (commit) mutate("scene.set_camera", fields, "Camera updated");
        if (!active && ImGui::Button("Make active")) {
            mutate("scene.set_camera", entity_field(selection) + ",\"active\":true",
                   "Camera activated");
        } else if (active) {
            ImGui::TextDisabled("Active camera");
        }
        ImGui::SameLine();
        if (ImGui::Button("Remove camera")) {
            mutate("scene.set_camera", entity_field(selection) + ",\"enabled\":false",
                   "Camera removed");
        }
    }

    void draw_renderer_section(const JsonValue::Object& entity) {
        const auto* renderer = component(entity, "mesh_renderer");
        if (!ImGui::CollapsingHeader("Mesh renderer",
                                     renderer != nullptr ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            return;
        }
        const auto mesh = renderer != nullptr ? string_or(*renderer, "mesh") : std::string{};
        const auto material =
            renderer != nullptr ? string_or(*renderer, "material") : std::string{};

        const auto combo = [&](const char* const label, const std::vector<std::string>& options,
                               const std::string& current) -> std::optional<std::string> {
            std::optional<std::string> chosen;
            if (ImGui::BeginCombo(label, current.empty() ? "<none>" : current.c_str())) {
                for (const auto& option : options) {
                    if (ImGui::Selectable(option.c_str(), option == current)) chosen = option;
                }
                ImGui::EndCombo();
            }
            return chosen;
        };

        if (const auto chosen = combo("Mesh", mesh_names, mesh)) {
            mutate("scene.set_renderer",
                   entity_field(selection) + ",\"enabled\":true,\"mesh\":\"" + *chosen +
                       "\",\"material\":\"" + (material.empty() ? "builtin.orange" : material) +
                       '"',
                   "Renderer updated");
        }
        if (const auto chosen = combo("Material", material_names, material)) {
            mutate("scene.set_renderer",
                   entity_field(selection) + ",\"enabled\":true,\"mesh\":\"" +
                       (mesh.empty() ? "builtin.triangle" : mesh) + "\",\"material\":\"" + *chosen +
                       '"',
                   "Renderer updated");
        }
        if (renderer != nullptr && ImGui::Button("Remove renderer")) {
            mutate("scene.set_renderer", entity_field(selection) + ",\"enabled\":false",
                   "Renderer removed");
        }
    }

    void draw_animator_section(const JsonValue::Object& entity) {
        const auto* animator = component(entity, "animator");
        if (animator == nullptr) return;
        if (!ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen)) return;

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
        } else if (ImGui::BeginCombo("Clip", clip_label(static_cast<std::size_t>(clip)).c_str())) {
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
                                ? slider_scalar("Time", time_seconds, 0.0, duration, "%.3f s")
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
        if (drag_scalar("Speed", speed, 0.01F)) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"speed\":" + number_text(speed), "Speed updated");
        }
    }

    void draw_light_section(const JsonValue::Object& entity) {
        const auto* light = component(entity, "light");
        if (!ImGui::CollapsingHeader("Light",
                                     light != nullptr ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            return;
        }
        if (light == nullptr) {
            // Adding a light is a normal editor action, not something only an importer can do.
            if (ImGui::Button("Add directional")) {
                mutate("scene.set_light",
                       entity_field(selection) + ",\"enabled\":true,\"type\":\"directional\"",
                       "Light added");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add point")) {
                mutate("scene.set_light",
                       entity_field(selection) + ",\"enabled\":true,\"type\":\"point\"",
                       "Light added");
            }
            ImGui::SameLine();
            if (ImGui::Button("Add spot")) {
                mutate("scene.set_light",
                       entity_field(selection) + ",\"enabled\":true,\"type\":\"spot\"",
                       "Light added");
            }
            return;
        }

        const auto type = static_cast<unsigned>(number_or(*light, "type", 1));
        static constexpr std::array<const char*, 3> type_names{"directional", "point", "spot"};
        int type_index = static_cast<int>(std::min(type, 2U));
        ImGui::SetNextItemWidth(160.0F);
        if (ImGui::Combo("Type", &type_index, "directional\0point\0spot\0")) {
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

        if (ImGui::Button("Remove light")) {
            mutate("scene.set_light", entity_field(selection) + ",\"enabled\":false",
                   "Light removed");
        }
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

    void draw_inspector() {
        if (selection.empty()) {
            ImGui::Dummy(ImVec2(0.0F, 6.0F * ui_scale));
            ImGui::TextColored(editor_color(editor_palette().text_faint), "Nothing selected.");
            ImGui::TextColored(editor_color(editor_palette().text_faint),
                               "Pick an object in the viewport or");
            ImGui::TextColored(editor_color(editor_palette().text_faint),
                               "choose one from the hierarchy.");
            return;
        }
        const auto* entity = find_entity(selection);
        if (entity == nullptr) {
            ImGui::TextDisabled("The selected entity no longer exists.");
            return;
        }
        const auto& palette = editor_palette();
        ImGui::PushFont(fonts.heading, fonts.heading_size * 1.05F);
        ImGui::TextUnformatted(string_or(*entity, "name", "Entity").c_str());
        ImGui::PopFont();
        const auto parent = string_or(*entity, "parent");
        ImGui::PushFont(fonts.monospace, fonts.monospace_size);
        ImGui::TextColored(editor_color(palette.text_faint), "%s   parent %s", selection.c_str(),
                           parent.empty() ? "root" : parent.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0F, 4.0F * ui_scale));

        draw_transform_section(*entity);
        draw_camera_section(*entity);
        draw_renderer_section(*entity);
        draw_animator_section(*entity);
        draw_morph_section(*entity);
        draw_light_section(*entity);
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
        set_status((opening ? "Opened " : "Saved ") + filename, false);
        return true;
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
    void duplicate_selection() {
        if (selection.empty()) return;
        const auto result = call("scene.duplicate", entity_field(selection));
        if (!result) return;
        set_status("Duplicated entity", false);
        refresh_pending = true;
        if (const auto* object = result->object()) {
            if (auto copy = string_or(*object, "entity"); !copy.empty()) {
                refresh();
                select(copy);
            }
        }
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
        auto title = scene_title() + " - Relay Editor";
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

    void create_node(const char* name, const char* component = nullptr, const char* fields = "") {
        auto created = call("scene.create", "\"name\":\"" + json_escape(name) + '\"');
        if (!created || !created->object())
            return;
        const auto handle = string_or(*created->object(), "entity");
        const bool component_ok =
            !component || mutate(component, entity_field(handle) + fields, "Component added");
        refresh_pending = true;
        select(handle);
        if (component_ok)
            set_status(std::string("Created ") + name, false);
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
            future_action("New project...");
            future_action("Open project...");
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
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !undo_labels.empty()))
                mutate("scene.undo", {}, "Undone");
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !redo_labels.empty()))
                mutate("scene.redo", {}, "Redone");
            ImGui::Separator();
            future_action("Cut");
            future_action("Copy");
            future_action("Paste");
            if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection.empty()))
                duplicate_selection();
            if (ImGui::MenuItem("Delete selection", "Delete", false, !selection.empty())) {
                if (mutate("scene.destroy", entity_field(selection), "Entity destroyed"))
                    select({});
            }
            if (ImGui::MenuItem("Clear selection", nullptr, false, !selection.empty()))
                select({});
            ImGui::Separator();
            future_action("Editor preferences...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scene")) {
            if (ImGui::BeginMenu("Add node")) {
                if (ImGui::MenuItem("Empty node"))
                    create_node("Empty");
                if (ImGui::MenuItem("Quad"))
                    create_node("Quad", "scene.set_renderer",
                                ",\"mesh\":\"builtin.quad\",\"material\":\"builtin.azure\"");
                if (ImGui::MenuItem("Triangle"))
                    create_node("Triangle", "scene.set_renderer",
                                ",\"mesh\":\"builtin.triangle\",\"material\":\"builtin.azure\"");
                if (ImGui::MenuItem("Camera"))
                    create_node("Camera", "scene.set_camera", ",\"active\":false");
                if (ImGui::MenuItem("Sun light"))
                    create_node("Sun Light", "scene.set_light", ",\"type\":\"directional\"");
                if (ImGui::MenuItem("Point light"))
                    create_node("Point Light", "scene.set_light", ",\"type\":\"point\"");
                if (ImGui::MenuItem("Spot light"))
                    create_node("Spot Light", "scene.set_light", ",\"type\":\"spot\"");
                ImGui::Separator();
                future_action("Cube");
                future_action("Audio source");
                future_action("Physics body");
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Frame selection", "F", false, !selection.empty()))
                focus_selection();
            ImGui::Separator();
            future_action("Attach script...");
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
            constexpr const char* names[]{"Hierarchy", "Inspector", "Assets",
                                          "History", "Diagnostics", "Viewport"};
            for (std::size_t i = 0; i < panel_open.size(); ++i)
                ImGui::MenuItem(names[i], nullptr, &panel_open[i]);
            ImGui::Separator();
            ImGui::MenuItem("Ground grid", nullptr, &grid_enabled);
            future_action("Wireframe");
            future_action("Lighting debug");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Run")) {
            const auto* status = runtime_status.object();
            const bool paused = status && boolean_or(*status, "paused", false);
            if (ImGui::MenuItem(paused ? "Resume simulation" : "Pause simulation"))
                mutate(paused ? "runtime.resume" : "runtime.pause", {},
                       paused ? "Resumed" : "Paused");
            if (ImGui::MenuItem("Step one frame"))
                mutate("runtime.step", "\"frames\":1", "Stepped one frame");
            ImGui::Separator();
            future_action("Run standalone game");
            future_action("Build project...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
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
            future_action("Shader editor...");
            future_action("Animation timeline...");
            future_action("Agent workspace...");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            if (ImGui::MenuItem("Reset layout")) {
                panel_open.fill(true);
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

    void draw_dialogs() {
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
            const char* title = file_action == FileAction::open         ? "Open scene"
                                : file_action == FileAction::save_as    ? "Save scene as"
                                : file_action == FileAction::import     ? "Import model"
                                : file_action == FileAction::screenshot ? "Capture GPU screenshot"
                                                                        : "Record GPU WebM";
            ImGui::TextUnformatted(title);
            ImGui::TextDisabled("Project folder: %s", scene_action ? "scenes/"
                                                      : file_action == FileAction::import
                                                          ? "assets/"
                                                          : "captures/");
            ImGui::SetNextItemWidth(360.0F * ui_scale);
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();
            const bool submitted =
                ImGui::InputText("Filename", action_filename.data(), action_filename.size(),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
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
                if (scene_action) {
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
                    "duplicate | Delete: delete selection\nCtrl+N: new scene | Ctrl+O: open | "
                    "Ctrl+S: save | Ctrl+Shift+S: save as\nDrag panel tabs to dock; Shift-drag "
                    "to float.");
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
        const auto* status = runtime_status.object();
        const bool paused = status && boolean_or(*status, "paused", false);
        if (toolbar_button("##simulation", paused ? ToolIcon::play : ToolIcon::pause, paused,
                           paused ? "Resume simulation" : "Pause simulation"))
            mutate(paused ? "runtime.resume" : "runtime.pause", {}, paused ? "Resumed" : "Paused");
        if (toolbar_button("##step", ToolIcon::step, false, "Advance exactly one simulation frame"))
            mutate("runtime.step", "\"frames\":1", "Stepped one frame");
        ImGui::TextColored(editor_color(editor_palette().text_faint), "%.0f FPS",
                           ImGui::GetIO().Framerate);
        ImGui::SameLine();
        toolbar_divider();
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
        ImGui::NewLine();
    }

    // Keyboard shortcuts, ignored whenever a text field has focus so typing a name never switches
    // the gizmo or deletes the selection.
    void update_shortcuts() {
        if (ImGui::GetIO().WantTextInput || navigating ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            return;
        const auto& shortcuts = ImGui::GetIO();
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
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) gizmo_operation = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            gizmo_operation = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false))
            gizmo_operation = ImGuizmo::SCALE;
        if (!ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, false))
            focus_selection();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selection.empty()) {
            mutate("scene.destroy", entity_field(selection), "Entity destroyed");
            selection.clear();
        }
        const auto& io = ImGui::GetIO();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            mutate(io.KeyShift ? "scene.redo" : "scene.undo", {},
                   io.KeyShift ? "Redone" : "Undone");
        }
    }

    void draw_assets() {
        // Preset and import share one row so the list itself gets the panel's height.
        ImGui::SetNextItemWidth(-116.0F * ui_scale);
        ImGui::Combo("##preset", &import_preset, "scene\0static_mesh\0");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("scene keeps animation, cameras and lights; static_mesh strips them");
        }
        ImGui::SameLine();
        if (ImGui::Button("Import", ImVec2(-1.0F, 0.0F)))
            import_model(model_filename.data());
        ImGui::Dummy(ImVec2(0.0F, 2.0F * ui_scale));

        const float footer = ImGui::GetTextLineHeightWithSpacing();
        if (begin_region("##models", ImVec2(0.0F, -footer))) {
            if (available_models.empty()) {
                ImGui::TextColored(editor_color(editor_palette().text_faint),
                                   "No importable models in assets/.");
            }
            for (const auto& model : available_models) {
                const bool selected = model == std::string(model_filename.data());
                if (ImGui::Selectable(model.c_str(), selected)) {
                    const auto length = std::min(model.size(), model_filename.size() - 1U);
                    std::copy_n(model.begin(), length, model_filename.begin());
                    model_filename[length] = '\0';
                }
                // Double-clicking imports straight away, which is the common case.
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    import_model(model);
                }
            }
        }
        ImGui::EndChild();
        ImGui::TextColored(editor_color(editor_palette().text_faint), "%zu meshes   %zu materials",
                           mesh_names.size(), material_names.size());
    }

    void import_model(const std::string& filename) {
        const std::string preset = import_preset == 0 ? "scene" : "static_mesh";
        const auto imported =
            call("assets.import_model", "\"filename\":\"" + json_escape(filename) +
                                            "\",\"instantiate\":true,\"preset\":\"" + preset + '"');
        if (!imported)
            return;
        set_status("Imported " + filename, false);
        assets_pending = true;
        refresh_pending = true;
        refresh();
        // Select and frame the imported root, so an import is immediately editable.
        if (const auto* object = imported->object()) {
            if (const auto* imported_roots = field(*object, "roots");
                imported_roots && imported_roots->array() && !imported_roots->array()->empty()) {
                if (const auto* handle = imported_roots->array()->front().string()) {
                    select(*handle);
                    focus_selection();
                }
            }
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

    void draw_diagnostics() {
        const auto& palette = editor_palette();
        if (!status_message.empty()) {
            ImGui::TextColored(editor_color(status_is_error ? palette.danger : palette.success),
                               "%s", status_message.c_str());
            ImGui::Dummy(ImVec2(0.0F, 2.0F * ui_scale));
        }
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

EditorUi::EditorUi(RequestHandler request) : impl_(std::make_unique<Impl>(std::move(request))) {}

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
        ImGui::DestroyContext();
        impl_->imgui_context_created = false;
    }
}

bool EditorUi::initialize(const OverlayContext& context, std::string& error) {
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
    info.DescriptorPoolSize = 16U;
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
    if (event->type == SDL_EVENT_MOUSE_MOTION && impl_->mouse_captured) {
        // ImGui stays at the viewport anchor while freelook uses unbounded relative motion.
        impl_->relative_delta.x += event->motion.xrel;
        impl_->relative_delta.y += event->motion.yrel;
        return true;
    }
    ImGui_ImplSDL3_ProcessEvent(event);
    const auto& io = ImGui::GetIO();
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        // Editor-view pointer input belongs to navigation, selection and gizmos. Scene-camera mode
        // leaves viewport input available to the game while panels still capture their own events.
        return io.WantCaptureMouse || impl_->camera_enabled;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const auto key = event->key.key;
        if (event->type == SDL_EVENT_KEY_UP && impl_->editor_keys.erase(key)) return true;
        // Editor view is an authoring surface. Scene-camera mode forwards gameplay keys;
        // track owned releases so switching modes mid-press cannot leave the game with half a key.
        const bool owned = impl_->camera_enabled;
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
    if (!impl_->vulkan_backend_started) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    // SDL's display scale is in physical pixels. ImGui lays out in window coordinates,
    // and its renderer already applies framebuffer density (including fractional Wayland DPI).
    // Use only the remaining content scale here to avoid applying desktop scaling twice.
    const float density = SDL_GetWindowPixelDensity(impl_->sdl_window);
    const float display_scale = SDL_GetWindowDisplayScale(impl_->sdl_window);
    const float scale = density > 0.0F && display_scale > 0.0F
                            ? display_scale / density : 1.0F;
    if (std::abs(scale - impl_->ui_scale) > 0.001F) {
        impl_->ui_scale = scale;
        apply_editor_theme(scale);
        ImGui::GetStyle().FontSizeBase = impl_->fonts.body_size;
        ImGui::GetStyle().FontScaleDpi = scale;
    }
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
    impl_->frame_open = true;

    const auto delta = static_cast<double>(ImGui::GetIO().DeltaTime);
    impl_->seconds_since_refresh += delta;
    impl_->seconds_since_assets += delta;
    if (impl_->refresh_pending || impl_->seconds_since_refresh >= impl_->refresh_interval()) {
        impl_->refresh();
    }

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
    constexpr ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoCollapse;
    const auto panel = [&](const char* name, std::size_t index) {
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(160.0F * impl_->ui_scale, 80.0F * impl_->ui_scale), ImVec2(FLT_MAX, FLT_MAX));
        return ImGui::Begin(name, &impl_->panel_open[index], panel_flags);
    };
    if (impl_->panel_open[0]) {
        if (panel("Hierarchy", 0)) {
            impl_->panel_header("Hierarchy");
            impl_->draw_hierarchy();
        }
        ImGui::End();
    }
    if (impl_->panel_open[1]) {
        if (panel("Inspector", 1)) {
            impl_->panel_header("Inspector");
            impl_->draw_inspector();
        }
        ImGui::End();
    }
    if (impl_->panel_open[2]) {
        if (panel("Assets", 2)) {
            impl_->panel_header("Assets");
            impl_->draw_assets();
        }
        ImGui::End();
    }
    if (impl_->panel_open[3]) {
        if (panel("History", 3)) {
            impl_->panel_header("History");
            impl_->draw_history();
        }
        ImGui::End();
    }
    if (impl_->panel_open[4]) {
        if (panel("Diagnostics", 4)) {
            impl_->panel_header("Diagnostics");
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
            impl_->viewport_draw_list = ImGui::GetWindowDrawList();
            if (impl_->viewport_visible) {
                impl_->viewport_draw_list->AddCallback(Impl::draw_scene_callback, impl_.get());
                impl_->viewport_draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }
        impl_->update_camera_input();
        impl_->update_shortcuts();
        impl_->update_view();
        impl_->draw_gizmo();
        impl_->update_selection_input();
        ImGui::End();
    } else {
        impl_->viewport_visible = false;
        impl_->viewport_hovered = false;
        impl_->freelook_latched = false;
        impl_->capture_pointer(false);
        impl_->update_shortcuts();
    }
    impl_->draw_dialogs();

    ImGui::Render();
    impl_->frame_open = false;
}

Entity EditorUi::selected_entity() const {
    return impl_->camera_enabled ? Entity::parse(impl_->selection).value_or(Entity{}) : Entity{};
}

bool EditorUi::ground_grid_visible() const { return impl_->camera_enabled && impl_->grid_enabled; }

EditorViewport EditorUi::scene_viewport() const { return impl_->viewport; }

const ViewOverride* EditorUi::view_override() const {
    return impl_->camera_enabled ? &impl_->view : nullptr;
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
