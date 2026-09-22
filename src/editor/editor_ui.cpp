#include "relay/editor/editor_ui.hpp"
#include "relay/editor/chat_media.hpp"

#include "relay/core/json.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/editor/editor_camera.hpp"
#include "relay/editor/wrapped_input.hpp"
#include "relay/editor/editor_layout.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_state.hpp"
#include "relay/editor/editor_selection.hpp"
#include "relay/editor/editor_timeline.hpp"
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
#include <tuple>
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

    enum class ToolIcon { previous, restart, loop, snap, play, pause, step, undo, redo, camera, focus, move, rotate, scale, local, world };
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
    bool headless{false};
    std::map<std::string, std::array<float, 4>, std::less<>> headless_items;
    void note_item(const std::string& key) {
        if (!headless) return;
        const auto minimum = ImGui::GetItemRectMin(), maximum = ImGui::GetItemRectMax();
        headless_items[key] = {minimum.x, minimum.y, maximum.x, maximum.y};
    }
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
    JsonValue project_status;
    std::vector<std::string> project_files;
    std::array<char, 129> project_name{"My project"};
    std::string pending_scene_filename, pending_project_filename;
    std::vector<std::string> mesh_names;
    std::vector<std::string> material_names;
    std::deque<std::string> log_lines;
    std::uint64_t last_log_sequence{0};

    EditorSelection selections;
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
    std::uint64_t timeline_gesture{0};
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

    std::array<char, 129> model_filename{"relay-pbr-golden.glb"};
    std::array<char, 129> create_name{"Entity"};
    std::string renaming;
    std::array<char, 129> rename_buffer{};
    std::vector<std::string> available_models, available_files;
    std::string assets_root = "assets";
    std::vector<std::string> undo_labels, redo_labels;
    int import_preset{0};
    std::array<bool, 9> panel_open{true, true, true, false, true, true, false, false, false};
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
                ImGui::Checkbox("Remove saved credential", &provider_clear);
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
            if (ImGui::Checkbox("Allow all actions", &automatic))
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

    void refresh() {
        refresh_pending = false;
        seconds_since_refresh = 0.0;
        const bool periodic = seconds_since_assets >= refresh_interval_seconds;
        if (periodic) seconds_since_assets = 0.0;
        if (panel_open[8]) {
            if (auto result = call("session.review")) agent_review = std::move(*result);
            if (auto result = call("session.audit")) agent_audit = std::move(*result);
            if (auto result = call("chat.status")) chat_status = std::move(*result);
        }
        if (auto status = call("runtime.status")) runtime_status = std::move(*status);
        if (auto project = call("project.status")) project_status = std::move(*project);
        if (auto clipboard = call("scene.clipboard"); clipboard && clipboard->object())
            clipboard_ready = number_or(*clipboard->object(), "entities", 0) > 0;
        if (auto list = call("scene.list")) {
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
        // Assets are refreshed on the same cadence rather than only after a UI-driven import,
        // because an agent sharing this runtime can import a model at any time and the human's
        // mesh and material lists must reflect that.
        if (periodic || assets_pending) {
            refresh_assets();
            if (auto projects = call("project.list"); projects && projects->object()) {
                project_files.clear();
                if (const auto* list = field(*projects->object(), "projects"); list && list->array())
                    for (const auto& file : *list->array())
                        if (file.string()) project_files.push_back(*file.string());
            }
        }
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
                available_files.clear();
                if (const auto* object = models->object()) {
                    assets_root = string_or(*object, "root");
                    if (const auto* files = field(*object, "files"); files && files->array())
                        for (const auto& item : *files->array())
                            if (item.string()) available_files.push_back(*item.string());
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
        drawing_rows.push_back(handle);

        if (selections.contains(handle)) flags |= ImGuiTreeNodeFlags_Selected;

        // Hierarchy highlights meet edge-to-edge; other controls keep normal spacing.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0F));
        const bool open = ImGui::TreeNodeEx(handle.c_str(), flags, "%s", name.c_str());
        ImGui::PopStyleVar();
        note_item("entity:" + handle);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) click_selection(handle, true);

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
            if (!selections.contains(handle)) select(handle);
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

    void draw_hierarchy() {
        drawing_rows.clear();
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
        visible_rows = drawing_rows;
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
        if (drag_scalar("Speed", speed, 0.01F, "%.2fx")) {
            mutate("scene.set_animation",
                   entity_field(selection) + ",\"speed\":" + number_text(speed), "Speed updated");
        }
    }

    void draw_keyframes_section(const JsonValue::Object& entity) {
        const auto* animation = component(entity, "transform_animation");
        if (!ImGui::CollapsingHeader("Transform keyframes")) return;
        const auto entity_request = entity_field(selection);
        if (!animation) {
            if (ImGui::Button("Add first key"))
                mutate("scene.keyframe.set", entity_request + ",\"time_seconds\":0",
                       "Transform key added");
            return;
        }
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
        if (slider_scalar("Key time", time, 0.0, duration, "%.3f s"))
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
            ++timeline_gesture;
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
        if (selections.handles.size() > 1) {
            ImGui::Text("%zu selected", selections.handles.size());
            ImGui::TextDisabled("Gizmo transforms all selected objects.");
            ImGui::TextDisabled("Properties below edit the active object.");
        }
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
        draw_keyframes_section(*entity);
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
        if (ImGui::BeginMenu("Edit")) {
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
                                          "History", "Diagnostics", "Viewport", "Timeline", "Project", "Agent"};
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
            if (ImGui::MenuItem("Animation timeline")) panel_open[6] = true;
            if (ImGui::MenuItem("Agent workspace...", "Ctrl+Shift+A")) { panel_open[8] = true; agent_expand_pending = true; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Layout")) {
            if (ImGui::MenuItem("Reset layout")) {
                panel_open = {true, true, true, false, true, true, false, false, false};
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
        if (ImGui::GetIO().WantTextInput || (ImGui::GetActiveID() != 0 && ImGui::GetInputTextState(ImGui::GetActiveID())) || navigating ||
            ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
            return;
        const auto& shortcuts = ImGui::GetIO();
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
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !selection.empty()) {
            group_operation("scene.destroy_many", "Deleted selection");
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
            if (available_files.empty()) {
                ImGui::TextColored(editor_color(editor_palette().text_faint),
                                   "No files in the project folder.");
            }
            for (const auto& model : available_files) {
                const bool importable = std::find(available_models.begin(), available_models.end(), model) != available_models.end();
                if (!importable) { ImGui::TextDisabled("%s", model.c_str()); continue; }
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

EditorUi::EditorUi(RequestHandler request) : impl_(std::make_unique<Impl>(std::move(request))) {
    (void)impl_->call("session.auto_approval", "\"enabled\":true");
}

void EditorUi::set_attachment_picker(AttachmentPicker picker) { impl_->attachment_picker = std::move(picker); }

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
    constexpr std::array<std::string_view, 9> names{"Hierarchy", "Inspector", "Assets", "History", "Diagnostics", "Viewport", "Timeline", "Project", "Agent"};
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i] == name) impl_->panel_open[i] = visible;
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
    if (impl_->headless) return false;
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
    if (!impl_->vulkan_backend_started && !impl_->headless) return;

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
            impl_->viewport_draw_list = ImGui::GetWindowDrawList();
            if (impl_->viewport_visible) {
                impl_->viewport_draw_list->AddCallback(Impl::draw_scene_callback, impl_.get());
                impl_->viewport_draw_list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }
        impl_->update_camera_input();
        if (!impl_->chat_media.viewer_open()) impl_->update_shortcuts();
        impl_->update_view();
        impl_->draw_gizmo();
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
    impl_->draw_dialogs();

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
    if (impl_->camera_enabled)
        for (const auto& handle : impl_->selections.handles)
            if (const auto entity = Entity::parse(handle)) result.push_back(*entity);
    return result;
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
