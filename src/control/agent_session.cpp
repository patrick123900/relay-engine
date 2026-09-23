#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/control/session_auth.hpp"
#include "relay/core/engine.hpp"
#include "relay/scene/project.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>

namespace relay {
namespace {
using J = JsonValue;
using O = J::Object;
using A = J::Array;
J number(std::uint64_t value) { return J{static_cast<double>(value)}; }
std::string text(const O& object, std::string_view key, std::string fallback = {}) {
    const auto* value = field(object, key);
    return value && value->string() ? *value->string() : fallback;
}
std::uint64_t integer(const O& object, std::string_view key) {
    const auto* value = field(object, key);
    return value && value->number() && *value->number() >= 0 &&
        *value->number() < static_cast<double>(std::numeric_limits<std::uint64_t>::max())
        ? static_cast<std::uint64_t>(*value->number()) : 0;
}
std::string reply(std::uint64_t id, J result) {
    return json_stringify(J{O{{"id", number(id)}, {"ok", J{true}}, {"result", std::move(result)}}});
}
std::string failure(std::uint64_t id, std::string error) {
    return json_stringify(J{O{{"id", number(id)}, {"ok", J{false}}, {"error", J{std::move(error)}}}});
}
std::string_view file_parameter(std::string_view method) {
    if (method == "scene.save" || method == "scene.load" || method == "assets.import_model" ||
        method == "video.start" || method == "trace.start") return "filename";
    if (method == "render.capture" || method == "render.capture_async" ||
        method == "scripts.read" || method == "scripts.write") return "path";
    return {};
}
bool supports_entity(std::string_view method) {
    static const std::set<std::string_view> supported{"scene.inspect", "scene.bounds", "scene.set_transform",
        "scene.set_morph", "scene.set_light", "scene.set_renderer", "scene.rename", "scene.set_script",
        "scene.set_script_property", "component.add", "component.remove",
        "scene.set_first_person_controller"};
    return supported.contains(method);
}
} // namespace

std::string ControlProtocol::project_scope() const {
    return engine_.project() ? engine_.project()->filename : "<standalone>";
}

void ControlProtocol::policy_audit(std::string_view method, bool succeeded, std::uint64_t request_id) {
    if (audit_.size() == 256) audit_.erase(audit_.begin());
    audit_.push_back({++audit_sequence_, request_id, std::string(method), true, succeeded,
                     agent_grants_, project_scope(), scoped_grants_, auto_approval_});
}

bool ControlProtocol::set_agent_grants(const std::vector<std::string>& methods, std::string& error) {
    auto_approval_ = false;
    agent_grants_.clear();
    scoped_grants_.clear();
    policy_project_ = project_scope();
    if (methods.size() > 128) error = "too many agent grants";
    else {
        error.clear();
        for (const auto& method : methods) {
            const auto* spec = find_protocol_method(method);
            if (!spec || spec->host_only || spec->bridge_only || method == "trace.replay" || method.starts_with("session.")) {
                error = "invalid or unsupported agent grant: " + method;
                break;
            }
        }
    }
    if (error.empty()) {
        agent_grants_ = methods;
        std::sort(agent_grants_.begin(), agent_grants_.end());
        agent_grants_.erase(std::unique(agent_grants_.begin(), agent_grants_.end()), agent_grants_.end());
    }
    policy_audit("session.grants", error.empty());
    return error.empty();
}

void ControlProtocol::configure_agent_from_environment() {
    bridge_token_.clear();
    if (const auto* token = std::getenv("RELAY_BRIDGE_TOKEN"); token && session_token_matches(token, token))
        bridge_token_ = token;
    std::vector<std::string> methods;
    if (const auto* configured = std::getenv("RELAY_AGENT_GRANTS")) {
        std::string_view remaining{configured};
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            methods.emplace_back(remaining.substr(0, comma));
            if (comma == std::string_view::npos) break;
            remaining.remove_prefix(comma + 1);
            if (remaining.empty()) methods.emplace_back();
        }
    }
    std::string error;
    if (!set_agent_grants(methods, error)) std::cerr << "Agent grants denied: " << error << '\n';
}

bool ControlProtocol::validate_scope(const ScopedGrant& grant, std::string& error) const {
    error.clear();
    const auto* spec = find_protocol_method(grant.method);
    if (!spec || spec->host_only || spec->bridge_only || grant.method == "trace.replay" || grant.method.starts_with("session."))
        error = "unsupported method scope";
    else if (grant.project != project_scope()) error = "request belongs to a different project";
    else if (scoped_grants_.size() + agent_grants_.size() >= 128) error = "grant capacity reached";
    else if (grant.kind == "method" && !grant.target.empty()) error = "method grants cannot have a target";
    else if (grant.kind == "entity") {
        const auto entity = Entity::parse(grant.target);
        if (!supports_entity(grant.method) || !entity || !engine_.scene().get(*entity))
            error = "entity scope requires a live entity and a supported per-entity method";
    } else if (grant.kind == "file") {
        if (file_parameter(grant.method).empty() || !workspace_file(".", grant.target, ""))
            error = "file scope requires a safe exact filename and a supported file method";
    } else if (grant.kind != "method") error = "unknown scope kind";
    return error.empty();
}

bool ControlProtocol::approve_scope(const ScopedGrant& grant, std::string& error) {
    if (!validate_scope(grant, error)) return false;
    if (policy_project_ != project_scope()) {
        agent_grants_.clear();
        scoped_grants_.clear();
        policy_project_ = project_scope();
    }
    if (std::none_of(scoped_grants_.begin(), scoped_grants_.end(), [&](const ScopedGrant& item) {
        return item.method == grant.method && item.kind == grant.kind && item.target == grant.target && item.project == grant.project;
    })) scoped_grants_.push_back(grant);
    return true;
}

bool ControlProtocol::permits(std::string_view method, const O& fields) const {
    if (auto_approval_) return true;
    if (policy_project_ != project_scope()) return false;
    if (std::binary_search(agent_grants_.begin(), agent_grants_.end(), std::string(method))) return true;
    return std::any_of(scoped_grants_.begin(), scoped_grants_.end(), [&](const ScopedGrant& grant) {
        if (grant.method != method || grant.project != project_scope()) return false;
        if (grant.kind == "method") return true;
        if (grant.kind == "entity") return text(fields, "entity") == grant.target;
        std::string target = text(fields, file_parameter(method));
        if (file_parameter(method) == "path" && target.starts_with("captures/")) target.erase(0, 9);
        return target == grant.target;
    });
}

std::string ControlProtocol::audit_json(std::uint64_t after) const {
    A entries;
    for (const auto& entry : audit_) {
        if (entry.sequence <= after) continue;
        const auto* spec = find_protocol_method(entry.method);
        A grants, scopes;
        for (const auto& grant : entry.grants) grants.push_back(J{grant});
        for (const auto& grant : entry.scoped_grants)
            scopes.push_back(J{O{{"scope", J{grant.method}}, {"kind", J{grant.kind}},
                {"target", J{grant.target}}, {"project", J{grant.project}}}});
        entries.push_back(J{O{{"sequence", number(entry.sequence)}, {"request_id", number(entry.request_id)},
            {"scope", J{entry.method}}, {"allowed", J{entry.allowed}}, {"succeeded", J{entry.succeeded}},
            {"read_only", J{spec && spec->read_only}}, {"destructive", J{spec && spec->destructive}},
            {"auto_approval", J{entry.auto_approval}}, {"grants", J{std::move(grants)}}, {"scoped_grants", J{std::move(scopes)}}, {"project", J{entry.project}}}});
    }
    return json_stringify(J{O{{"entries", J{std::move(entries)}}, {"last_sequence", number(audit_sequence_)},
        {"oldest_sequence", number(audit_.empty() ? audit_sequence_ + 1 : audit_.front().sequence)}}});
}

std::string ControlProtocol::handle_agent(std::string_view request) {
    JsonParser parser(request);
    auto value = parser.parse();
    const auto* object = value ? value->object() : nullptr;
    const auto id = object ? integer(*object, "id") : 0;
    const auto method = object ? text(*object, "method") : std::string{};
    const auto* spec = find_protocol_method(method);
    bool bridge = false;
    std::string normalized;
    if (object && spec && spec->bridge_only && session_token_matches(bridge_token_, text(*object, "bridge_token"))) {
        auto fields = *object;
        fields.erase("bridge_token");
        normalized = json_stringify(J{std::move(fields)});
        request = normalized;
        bridge = true;
    }
    std::string error;
    const bool valid = validate_protocol_request(request, method, error);
    const bool bootstrap = method == "session.status" || method == "session.audit" || method == "session.request";
    if (valid && (bootstrap || bridge)) return session_dispatch(request);
    const bool allowed = valid && spec && !spec->host_only && !spec->bridge_only && object && permits(method, *object);
    const auto granted_methods = allowed ? agent_grants_ : std::vector<std::string>{};
    const auto granted_scopes = allowed ? scoped_grants_ : std::vector<ScopedGrant>{};
    const auto context = project_scope();
    const auto response = [&]() {
        if (!allowed) return failure(id, valid ? "session capability denied: " + method : error);
        struct DispatchContext { bool& flag; bool previous; ~DispatchContext() { flag = previous; } } context_guard{agent_dispatch_, agent_dispatch_};
        agent_dispatch_ = true;
        return handle(request);
    }();
    JsonParser response_parser(response);
    const auto response_value = response_parser.parse();
    const auto* ok = response_value && response_value->object() ? field(*response_value->object(), "ok") : nullptr;
    if (audit_.size() == 256) audit_.erase(audit_.begin());
    audit_.push_back({++audit_sequence_, id, spec ? method : "invalid.request", allowed,
                     ok && ok->boolean() && *ok->boolean(), granted_methods, context, granted_scopes, auto_approval_});
    return response;
}

std::string ControlProtocol::session_dispatch(std::string_view request) {
    JsonParser parser(request);
    const auto value = parser.parse();
    const auto& fields = *value->object(); // caller has performed strict generated validation
    const auto id = integer(fields, "id");
    const auto method = text(fields, "method");
    if (method == "session.status") {
        A grants, scopes;
        if (policy_project_ == project_scope()) {
            for (const auto& grant : agent_grants_) grants.push_back(J{grant});
            for (const auto& grant : scoped_grants_) if (grant.project == project_scope())
                scopes.push_back(J{O{{"scope", J{grant.method}}, {"kind", J{grant.kind}}, {"target", J{grant.target}}}});
        }
        return reply(id, J{O{{"auto_approval", J{auto_approval_}}, {"grants", J{std::move(grants)}}, {"scoped_grants", J{std::move(scopes)}},
            {"project", J{project_scope()}}, {"audit_capacity", number(256)}, {"last_sequence", number(audit_sequence_)}}});
    }
    if (method == "session.auto_approval") {
        auto_approval_ = *field(fields, "enabled")->boolean();
        access_requests_.clear();
        policy_audit("session.auto_approval", true, id);
        return reply(id, J{O{{"auto_approval", J{auto_approval_}}}});
    }
    if (method == "session.audit") return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":" + audit_json(integer(fields, "after")) + "}";
    if (method == "session.review") {
        A pending, methods;
        for (const auto& pending_request : access_requests_)
            pending.push_back(J{O{{"request", number(pending_request.id)}, {"scope", J{pending_request.grant.method}},
                {"kind", J{pending_request.grant.kind}}, {"target", J{pending_request.grant.target}},
                {"project", J{pending_request.grant.project}}}});
        for (const auto& spec : protocol_methods()) {
            if (spec.host_only || spec.bridge_only || spec.method.starts_with("session.") || spec.method == "trace.replay") continue;
            methods.push_back(J{O{{"scope", J{std::string(spec.method)}}, {"description", J{std::string(spec.description)}},
                {"read_only", J{spec.read_only}}, {"destructive", J{spec.destructive}},
                {"entity_scope", J{supports_entity(spec.method)}}, {"file_scope", J{!file_parameter(spec.method).empty()}}}});
        }
        // Keep the serialized status alive while parsing it.
        const auto status_text = session_dispatch("{\"id\":0,\"method\":\"session.status\"}");
        JsonParser status_reader(status_text);
        const auto status = status_reader.parse();
        return reply(id, J{O{{"pending", J{std::move(pending)}}, {"methods", J{std::move(methods)}},
            {"session", *field(*status->object(), "result")}}});
    }
    if (method == "session.request" || method == "session.grant") {
        ScopedGrant grant{text(fields, "scope"), text(fields, "kind", "method"), text(fields, "target"), project_scope()};
        std::string error;
        if (method == "session.request") {
            const auto* requested = find_protocol_method(grant.method);
            if (auto_approval_ && requested && !requested->host_only && !requested->bridge_only && !grant.method.starts_with("session.")) {
                policy_audit("session.request", true, id);
                return reply(id, J{O{{"pending", J{false}}, {"approved", J{true}}}});
            }
            const bool supported = validate_scope(grant, error);
            if (!supported) { policy_audit("session.request", false, id); return failure(id, error); }
            for (const auto& existing : access_requests_)
                if (existing.grant.method == grant.method && existing.grant.kind == grant.kind &&
                    existing.grant.target == grant.target && existing.grant.project == grant.project)
                    return reply(id, J{O{{"request", number(existing.id)}, {"pending", J{true}}}});
            if (access_requests_.size() >= 32) { policy_audit("session.request", false, id); return failure(id, "approval request capacity reached"); }
            access_requests_.push_back({++access_sequence_, std::move(grant)});
            policy_audit("session.request", true, id);
            return reply(id, J{O{{"request", number(access_sequence_)}, {"pending", J{true}}}});
        }
        const bool approved = approve_scope(grant, error);
        policy_audit("session.grant", approved, id);
        if (!approved) return failure(id, error);
        return reply(id, J{O{{"approved", J{true}}}});
    }
    if (method == "session.decide") {
        const auto requested = integer(fields, "request");
        const auto found = std::find_if(access_requests_.begin(), access_requests_.end(), [&](const AccessRequest& item) { return item.id == requested; });
        if (found == access_requests_.end()) { policy_audit("session.decide", false, id); return failure(id, "approval request not found"); }
        const bool allow = *field(fields, "allow")->boolean();
        std::string error;
        if (allow && !approve_scope(found->grant, error)) {
            policy_audit("session.decide", false, id);
            return failure(id, error);
        }
        access_requests_.erase(found);
        policy_audit(allow ? "session.approved" : "session.denied", true, id);
        return reply(id, J{O{{"approved", J{allow}}}});
    }
    if (method == "session.revoke") {
        const auto scope = text(fields, "scope");
        if (scope.empty()) {
            auto_approval_ = false;
            access_requests_.clear();
            agent_grants_.clear();
            scoped_grants_.clear();
        } else {
            std::erase(agent_grants_, scope);
            std::erase_if(scoped_grants_, [&](const ScopedGrant& grant) { return grant.method == scope; });
        }
        policy_audit("session.revoke", true, id);
        return reply(id, J{O{{"revoked", J{true}}}});
    }
    if (method == "session.export_audit") {
        const auto path = workspace_file(".relay/audits", text(fields, "filename"), ".jsonl");
        if (!path) return failure(id, "unsafe audit filename or directory");
        std::error_code ec;
        std::filesystem::create_directories(path->parent_path(), ec);
        if (ec) return failure(id, "cannot create audit directory");
        static std::atomic<std::uint64_t> serial{};
        const auto temporary = path->string() + ".tmp-" + std::to_string(++serial);
        if (std::filesystem::exists(temporary, ec)) return failure(id, "audit temporary path exists");
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        const auto audit_text = audit_json(0);
        JsonParser audit_parser(audit_text);
        const auto audit = audit_parser.parse();
        for (const auto& entry : *field(*audit->object(), "entries")->array()) stream << json_stringify(entry) << '\n';
        stream.close();
        if (!stream) { std::filesystem::remove(temporary, ec); return failure(id, "audit write failed"); }
        // Hard-link publication is atomic and refuses existing destinations; never overwrite an audit.
        std::filesystem::create_hard_link(temporary, *path, ec);
        const bool published = !ec;
        std::filesystem::remove(temporary, ec);
        policy_audit("session.export_audit", published, id);
        if (!published) return failure(id, "audit destination exists or atomic publication is unavailable");
        return reply(id, J{O{{"path", J{path->generic_string()}}}});
    }
    if (method == "chat.control") {
        if (bridge_token_.empty()) return failure(id, "external bridge is not connected");
        if (chat_submissions_.size() >= 8) return failure(id, "chat submission queue is full");
        O control = fields;
        control.erase("id"); control.erase("method");
        chat_submissions_.push_back(J{O{{"control", J{std::move(control)}}}});
        return reply(id, J{O{{"queued", J{true}}}});
    }
    if (method == "chat.configure") {
        if (bridge_token_.empty()) return failure(id, "external bridge is not connected");
        if (chat_submissions_.size() >= 8) return failure(id, "chat submission queue is full");
        O settings = fields;
        settings.erase("id"); settings.erase("method");
        chat_submissions_.push_back(J{O{{"configure", J{std::move(settings)}}}});
        return reply(id, J{O{{"queued", J{true}}}});
    }
    if (method == "chat.submit") {
        if (bridge_token_.empty()) return failure(id, "external bridge is not connected; launch the editor through the bridge");
        if (const auto* object = bridge_view_.object())
            if (const auto* busy = field(*object, "busy"); busy && busy->boolean() && *busy->boolean())
                return failure(id, "chat is busy; stop it before submitting another message");
        if (chat_submissions_.size() >= 8) return failure(id, "chat submission queue is full");
        O submission{{"id", number(++chat_sequence_)}, {"message", J{text(fields, "message")}}};
        const auto* attachments = field(fields, "attachments");
        if (attachments && attachments->array()) {
            for (const auto& attachment : *attachments->array())
                if (!attachment.string() || attachment.string()->size() > 4096) return failure(id, "invalid attachment path");
            submission.emplace("attachments", *attachments);
        }
        if (text(fields, "message").empty() && (!attachments || !attachments->array() || attachments->array()->empty())) return failure(id, "message or attachment required");
        chat_submissions_.push_back(J{std::move(submission)});
        return reply(id, J{O{{"submission", number(chat_sequence_)}}});
    }
    if (method == "chat.cancel") {
        // Stop chat work without discarding trusted account/provider changes queued alongside it.
        std::erase_if(chat_submissions_, [](const J& submission) {
            const auto* object = submission.object();
            return object && (field(*object, "message") || field(*object, "cancel"));
        });
        chat_submissions_.push_back(J{O{{"id", number(++chat_sequence_)}, {"cancel", J{true}}}});
        return reply(id, J{O{{"cancel_requested", J{true}}}});
    }
    if (method == "chat.status") return reply(id, J{O{{"connected", J{!bridge_token_.empty()}},
        {"pending", number(chat_submissions_.size())}, {"view", bridge_view_}}});
    if (method == "bridge.poll") {
        A submissions(chat_submissions_.begin(), chat_submissions_.end());
        chat_submissions_.clear();
        return reply(id, J{O{{"submissions", J{std::move(submissions)}}}});
    }
    if (method == "bridge.publish") {
        const auto view_text = text(fields, "view");
        JsonParser view_parser(view_text);
        const auto view = view_parser.parse();
        if (!view || !view->object()) return failure(id, "bridge view must be a JSON object");
        bridge_view_ = *view;
        return reply(id, J{O{{"published", J{true}}}});
    }
    return failure(id, "unsupported session method");
}
} // namespace relay
