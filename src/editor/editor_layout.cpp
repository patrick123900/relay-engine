#include "relay/editor/editor_layout.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <utility>

namespace relay {

std::string default_editor_layout_path() {
    const auto variable = [](const char* name) {
        const char* value = std::getenv(name);
        return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
    };
    std::filesystem::path directory;
#if defined(_WIN32)
    directory = variable("APPDATA");
    if (!directory.empty()) directory /= "Relay";
#elif defined(__APPLE__)
    directory = variable("HOME");
    if (!directory.empty()) directory /= "Library/Application Support/Relay";
#else
    directory = variable("XDG_CONFIG_HOME");
    if (directory.empty() && !variable("HOME").empty()) directory = variable("HOME") / ".config";
    if (!directory.empty()) directory /= "relay-engine";
#endif
    if (directory.empty()) return ".relay/editor-layout.ini";
    const auto path = directory / "editor-layout.ini";
    // Earlier builds kept the layout in the working directory's .relay folder.
    std::error_code error;
    if (!std::filesystem::exists(path, error) &&
        std::filesystem::is_regular_file(".relay/editor-layout.ini", error)) {
        std::filesystem::create_directories(directory, error);
        std::filesystem::copy_file(".relay/editor-layout.ini", path, error);
    }
    return path.string();
}

void EditorLayout::bind(std::string key, bool* value) {
    preferences_.push_back({std::move(key), value, *value});
}

void EditorLayout::read_preference(const std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) return;
    const auto key = line.substr(0, equals);
    const auto value = line.substr(equals + 1U);
    for (auto& preference : preferences_)
        if (preference.key == key && (value == "0" || value == "1")) {
            *preference.value = value == "1";
            preference.saved = *preference.value;
        }
    preferences_loaded = true;
}

void EditorLayout::write_preferences(std::string& output) const {
    output += "[Relay][Preferences]\n";
    for (const auto& preference : preferences_)
        output += preference.key + '=' + (*preference.value ? "1" : "0") + '\n';
    output += '\n';
}

void EditorLayout::initialize(std::string override_path) {
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // Keep all panels in this SDL/Vulkan surface; native multi-window presentation is a separate
    // backend capability. Docking, floating and resizing within the editor are fully available.
    const auto* configured = std::getenv("RELAY_EDITOR_LAYOUT_PATH");
    ini_path_ = !override_path.empty() ? std::move(override_path)
                : configured && *configured ? configured : default_editor_layout_path();
    std::error_code error;
    const auto parent = std::filesystem::path(ini_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) ini_path_.clear();
    io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();

    ImGuiSettingsHandler handler;
    handler.TypeName = "Relay";
    handler.TypeHash = ImHashStr("Relay");
    handler.UserData = this;
    handler.ReadOpenFn = [](ImGuiContext*, ImGuiSettingsHandler* self, const char* name) -> void* {
        return std::strcmp(name, "Preferences") == 0 ? self->UserData : nullptr;
    };
    handler.ReadLineFn = [](ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
        static_cast<EditorLayout*>(entry)->read_preference(line);
    };
    handler.WriteAllFn = [](ImGuiContext*, ImGuiSettingsHandler* self, ImGuiTextBuffer* output) {
        std::string text;
        static_cast<const EditorLayout*>(self->UserData)->write_preferences(text);
        output->append(text.c_str());
    };
    ImGui::AddSettingsHandler(&handler);
}

void EditorLayout::build(const float scale) {
    // Preference flags are not ImGui state, so changing one must schedule an ini save itself.
    for (auto& preference : preferences_)
        if (*preference.value != preference.saved) {
            preference.saved = *preference.value;
            ImGui::MarkIniSettingsDirty();
        }
    auto* viewport = ImGui::GetMainViewport();
    const auto id = ImGui::GetID("RelayDockspace");
    // Retire the old movable Controls panel without resetting the other saved panes.
    // Removing its dedicated leaf merges the sibling into the parent; shared tabs stay intact.
    if (!controls_migrated_) {
        controls_migrated_ = true;
        auto* settings = ImGui::FindWindowSettingsByID(ImHashStr("Controls"));
        if (settings) {
            const auto dock_id = settings->DockId;
            bool shared = false;
            auto& context = *ImGui::GetCurrentContext();
            for (auto* other = context.SettingsWindows.begin(); other;
                 other = context.SettingsWindows.next_chunk(other))
                if (other != settings && !other->WantDelete && other->DockId == dock_id)
                    shared = true;
            ImGui::ClearWindowSettings("Controls");
            auto* node = ImGui::DockBuilderGetNode(dock_id);
            if (dock_id && !shared && node && node->IsLeafNode())
                ImGui::DockBuilderRemoveNode(dock_id);
        }
    }
    if (reset_pending_ || ImGui::DockBuilderGetNode(id) == nullptr) {
        reset_pending_ = false;
        ImGui::DockBuilderRemoveNode(id);
        ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(id, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(id, viewport->WorkSize);
        ImGuiID main = id, bottom = 0, left = 0, right = 0;
        const auto height = std::max(viewport->WorkSize.y, 1.0F);
        ImGui::DockBuilderSplitNode(
            main, ImGuiDir_Down,
            std::clamp(214.0F * scale / height, .20F, .40F), &bottom,
            &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, .21F, &left, &main);
        ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, .21F / .79F, &right, &main);
        ImGuiID assets = 0;
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, .21F, &assets, &bottom);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Assets", assets);
        ImGui::DockBuilderDockWindow("History", bottom);
        ImGui::DockBuilderDockWindow("Agent", right);
        ImGui::DockBuilderDockWindow("Timeline", bottom);
        ImGui::DockBuilderDockWindow("Project", assets);
        ImGui::DockBuilderDockWindow("Diagnostics", bottom);
        ImGui::DockBuilderDockWindow("Profiler", bottom);
        ImGui::DockBuilderDockWindow("Viewport", main);
        ImGui::DockBuilderFinish(id);
    }
    // Layouts saved before panels and preferences were persisted lack these docked panels. Newer
    // layouts keep deliberately floating panels where the user left them.
    if (!optional_migrated_) {
        optional_migrated_ = true;
        if (!preferences_loaded) {
            for (const auto& pair : {std::pair{"Timeline", "Diagnostics"}, std::pair{"History", "Diagnostics"}, std::pair{"Agent", "Inspector"}, std::pair{"Project", "Assets"}, std::pair{"Profiler", "Diagnostics"}}) {
                const auto* optional = ImGui::FindWindowSettingsByID(ImHashStr(pair.first));
                const auto* anchor = ImGui::FindWindowSettingsByID(ImHashStr(pair.second));
                if ((!optional || !optional->DockId) && anchor && anchor->DockId)
                    ImGui::DockBuilderDockWindow(pair.first, anchor->DockId);
            }
        }
    }
    ImGui::DockSpaceOverViewport(id, viewport);
}

void EditorLayout::save() const {
    if (!ini_path_.empty()) ImGui::SaveIniSettingsToDisk(ini_path_.c_str());
}

} // namespace relay
