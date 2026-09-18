#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <optional>
#include <deque>
#include "relay/core/json.hpp"

namespace relay {

class Engine;

class ControlProtocol {
public:
    using CaptureHandler = std::function<bool(const std::filesystem::path&, std::string&)>;
    using RenderInspectionHandler = std::function<std::string(std::string_view)>;

    explicit ControlProtocol(Engine& engine, CaptureHandler capture_handler = {},
                             RenderInspectionHandler render_inspection_handler = {});

    using EditorCameraHandler = std::function<std::string(std::string_view)>;
    void set_editor_camera_handler(EditorCameraHandler handler) { editor_camera_handler_ = std::move(handler); }

    [[nodiscard]] std::string handle(std::string_view request);

    // Host-only approval/revocation. Never exposed as an agent tool. Exact method names,
    // no wildcard grants; invalid lists revoke all grants.
    [[nodiscard]] bool set_agent_grants(const std::vector<std::string>& methods, std::string& error);
    void configure_agent_from_environment();
    [[nodiscard]] std::string handle_agent(std::string_view request);

private:
    struct ScopedGrant { std::string method, kind, target, project; };
    struct AccessRequest { std::uint64_t id; ScopedGrant grant; };
    [[nodiscard]] std::string session_dispatch(std::string_view request);
    [[nodiscard]] std::string project_scope() const;
    [[nodiscard]] bool validate_scope(const ScopedGrant& grant, std::string& error) const;
    [[nodiscard]] bool approve_scope(const ScopedGrant& grant, std::string& error);
    [[nodiscard]] bool permits(std::string_view method, const JsonValue::Object& fields) const;
    void policy_audit(std::string_view method, bool succeeded, std::uint64_t request_id = 0);
    [[nodiscard]] std::string audit_json(std::uint64_t after) const;
    std::vector<ScopedGrant> scoped_grants_;
    std::deque<AccessRequest> access_requests_;
    std::uint64_t access_sequence_{};
    std::string policy_project_, bridge_token_;
    bool auto_approval_{}, agent_dispatch_{}, replay_active_{};
    std::deque<JsonValue> chat_submissions_;
    std::uint64_t chat_sequence_{};
    JsonValue bridge_view_{JsonValue::Object{}};
    struct AuditEntry {
        std::uint64_t sequence;
        std::uint64_t request_id;
        std::string method;
        bool allowed;
        bool succeeded;
        std::vector<std::string> grants;
        std::string project;
        std::vector<ScopedGrant> scoped_grants;
        bool auto_approval{};
    };
    std::vector<std::string> agent_grants_;
    std::vector<AuditEntry> audit_;
    std::uint64_t audit_sequence_{};
    Engine& engine_;
    CaptureHandler capture_handler_;
    EditorCameraHandler editor_camera_handler_;
    RenderInspectionHandler render_inspection_handler_;
};

} // namespace relay
