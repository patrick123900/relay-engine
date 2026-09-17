#include "relay/editor/editor_layout.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace relay {

void EditorLayout::initialize() {
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // Keep all panels in this SDL/Vulkan surface; native multi-window presentation is a separate
    // backend capability. Docking, floating and resizing within the editor are fully available.
    const auto* configured = std::getenv("RELAY_EDITOR_LAYOUT_PATH");
    ini_path_ = configured && *configured ? configured : ".relay/editor-layout.ini";
    std::error_code error;
    const auto parent = std::filesystem::path(ini_path_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) ini_path_.clear();
    io.IniFilename = ini_path_.empty() ? nullptr : ini_path_.c_str();
}

void EditorLayout::build(const float scale) {
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
        ImGuiID assets = 0, history = 0;
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, .21F, &assets, &bottom);
        ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, .21F / .79F, &history, &bottom);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Assets", assets);
        ImGui::DockBuilderDockWindow("History", history);
        ImGui::DockBuilderDockWindow("Diagnostics", bottom);
        ImGui::DockBuilderDockWindow("Viewport", main);
        ImGui::DockBuilderFinish(id);
    }
    ImGui::DockSpaceOverViewport(id, viewport);
}

void EditorLayout::save() const {
    if (!ini_path_.empty()) ImGui::SaveIniSettingsToDisk(ini_path_.c_str());
}

} // namespace relay
