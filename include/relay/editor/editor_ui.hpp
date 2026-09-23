#pragma once

// The human editor interface.
//
// The editor issues exactly the same newline-delimited JSON requests an agent sends over
// --agent-stdio or MCP, through the same ControlProtocol instance. It holds no reference to Scene,
// SceneHistory or AssetRegistry, so there is no second mutation path to keep in sync: a human drag
// in the inspector and an agent's scene.set_transform are the same native operation, land in the
// same undo history and are recorded in the same deterministic trace.

#include "relay/editor/editor_overlay.hpp"

#include <filesystem>
#include <functional>
#include <array>
#include <optional>
#include <memory>
#include <string>
#include <string_view>

namespace relay {

class EditorUi : public EditorOverlay {
public:
    // Handles one request line and returns one response line, matching ControlProtocol::handle.
    using RequestHandler = std::function<std::string(std::string_view)>;
    // Shows a project file (or opens a folder) in the OS file manager. Headless editors start with
    // none installed.
    using FileBrowserHandler = std::function<void(const std::filesystem::path&, bool directory)>;

    using AttachmentPicker = std::function<void(std::function<void(std::vector<std::string>)>)>;
    explicit EditorUi(RequestHandler request);
    // Background test seam; the normal picker is opened only by a human button press.
    void set_attachment_picker(AttachmentPicker picker);
    ~EditorUi() override;

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    bool initialize(const OverlayContext& context, std::string& error) override;
    // CPU-only ImGui frames for background interaction tests. No SDL window, GPU or OS input.
    bool initialize_headless(std::string& error);
    [[nodiscard]] std::optional<std::array<float, 4>> headless_item_rect(std::string_view key) const;
    // True while Run Game owns keyboard and mouse input, after a click on the viewport.
    [[nodiscard]] bool game_has_input() const;
    // True while the game has input and the editor keeps the pointer locked for it.
    [[nodiscard]] bool pointer_locked_for_game() const;
    // Changes editor view state without mutating the scene.
    void set_panel_visible(std::string_view name, bool visible);
    [[nodiscard]] bool panel_visible(std::string_view name) const;
    void set_file_browser_handler(FileBrowserHandler handler);
    // Opens a workspace-relative project and its startup scene, as the Project panel does.
    bool open_project(std::string_view filename);
    // Host seam: invoked only after native protocol validation and agent authorization.
    [[nodiscard]] std::string handle_camera_request(std::string_view request);
    void invalidate() override;
    // Execute render-related requests only after the current frame has been presented.
    void process_actions();
    bool handle_event(const void* sdl_event) override;
    void build(std::uint32_t width, std::uint32_t height) override;
    void record(VkCommandBuffer commands, const std::function<void()>& draw_scene) override;
    [[nodiscard]] EditorViewport scene_viewport() const override;
    [[nodiscard]] const ViewOverride* view_override() const override;
    [[nodiscard]] Entity selected_entity() const override;
    [[nodiscard]] std::vector<Entity> selected_entities() const override;
    [[nodiscard]] bool ground_grid_visible() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
