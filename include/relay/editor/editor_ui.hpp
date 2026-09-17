#pragma once

// The human editor interface.
//
// The editor issues exactly the same newline-delimited JSON requests an agent sends over
// --agent-stdio or MCP, through the same ControlProtocol instance. It holds no reference to Scene,
// SceneHistory or AssetRegistry, so there is no second mutation path to keep in sync: a human drag
// in the inspector and an agent's scene.set_transform are the same native operation, land in the
// same undo history and are recorded in the same deterministic trace.

#include "relay/editor/editor_overlay.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace relay {

class EditorUi : public EditorOverlay {
public:
    // Handles one request line and returns one response line, matching ControlProtocol::handle.
    using RequestHandler = std::function<std::string(std::string_view)>;

    explicit EditorUi(RequestHandler request);
    ~EditorUi() override;

    EditorUi(const EditorUi&) = delete;
    EditorUi& operator=(const EditorUi&) = delete;

    bool initialize(const OverlayContext& context, std::string& error) override;
    void invalidate() override;
    // Execute render-related requests only after the current frame has been presented.
    void process_actions();
    bool handle_event(const void* sdl_event) override;
    void build(std::uint32_t width, std::uint32_t height) override;
    void record(VkCommandBuffer commands, const std::function<void()>& draw_scene) override;
    [[nodiscard]] EditorViewport scene_viewport() const override;
    [[nodiscard]] const ViewOverride* view_override() const override;
    [[nodiscard]] Entity selected_entity() const override;
    [[nodiscard]] bool ground_grid_visible() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
