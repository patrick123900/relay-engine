#include "relay/editor/editor_layout.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace relay {

void EditorLayout::initialize(std::string override_path) {
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    // Keep all panels in this SDL/Vulkan surface; native multi-window presentation is a separate
    // backend capability. Docking, floating and resizing within the editor are fully available.
    const auto* configured = std::getenv("RELAY_EDITOR_LAYOUT_PATH");
    ini_path_ = !override_path.empty() ? std::move(override_path)
                : configured && *configured ? configured : ".relay/editor-layout.ini";
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
        ImGui::DockBuilderDockWindow("Viewport", main);
        ImGui::DockBuilderFinish(id);
    }
    if (!optional_migrated_) {
        optional_migrated_ = true;
        for (const auto& pair : {std::pair{"Timeline", "Diagnostics"}, std::pair{"History", "Diagnostics"}, std::pair{"Agent", "Inspector"}, std::pair{"Project", "Assets"}}) {
            const auto* optional = ImGui::FindWindowSettingsByID(ImHashStr(pair.first));
            const auto* anchor = ImGui::FindWindowSettingsByID(ImHashStr(pair.second));
            if ((!optional || !optional->DockId) && anchor && anchor->DockId)
                ImGui::DockBuilderDockWindow(pair.first, anchor->DockId);
        }
    }
    ImGui::DockSpaceOverViewport(id, viewport);
}

void EditorLayout::save() const {
    if (!ini_path_.empty()) ImGui::SaveIniSettingsToDisk(ini_path_.c_str());
}

} // namespace relay
