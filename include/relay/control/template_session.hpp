#pragma once

// Editing a project template in isolation. A session owns a private engine, scene, undo history and
// control protocol on the same project, with the template's node tree as the whole scene, so the
// editor can show it in its own window without touching the scene being edited in the main
// viewport. The editor talks to it through the same protocol as everything else; saving writes the
// template file back, and nothing else changes on disk.

#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace relay {

class TemplateSession {
public:
    // Handles a request in another engine; the session forwards script queries there, because
    // only that engine has built the project's scripts.
    using Forwarder = std::function<std::string(std::string_view)>;

    // Opens template `name` of the project file `project_filename` (as project.open takes it).
    // `shared_assets`, when given, is another engine's asset registry (imported meshes, materials
    // and textures) that this one draws on, so the window's renderer can draw both scenes.
    TemplateSession(const std::string& project_filename, std::string name, Forwarder scripts = {},
                    AssetRegistry* shared_assets = nullptr);
    ~TemplateSession();
    TemplateSession(const TemplateSession&) = delete;
    TemplateSession& operator=(const TemplateSession&) = delete;

    [[nodiscard]] bool ok() const { return error_.empty(); }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] const std::string& name() const { return name_; }

    // One protocol request. Methods that would write the project's scenes, switch projects or run
    // the game are refused, and scripts.* is answered by the forwarding engine.
    [[nodiscard]] std::string handle(std::string_view request);

    [[nodiscard]] Engine& engine() { return *engine_; }
    [[nodiscard]] const Engine& engine() const { return *engine_; }
    // The template's root node.
    [[nodiscard]] Entity root() const { return root_; }
    // Unsaved edits, by the undo history's revision.
    [[nodiscard]] bool dirty() const;
    // Writes the template file (replacing it) and marks the session saved.
    [[nodiscard]] bool save(std::string& error);
    // Discards the edits and reads the file again, clearing the undo history.
    [[nodiscard]] bool revert(std::string& error);
    // Keeps assets and materials loaded for drawing; call once per frame.
    void tick();

private:
    [[nodiscard]] bool load(std::string& error);
    void adopt_strays();

    std::string name_;
    std::string error_;
    Forwarder scripts_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<ControlProtocol> protocol_;
    Entity root_{};
    std::uint64_t saved_revision_{};
};

} // namespace relay
