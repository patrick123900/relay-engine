#include "relay/control/template_session.hpp"

#include "relay/core/json.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/templates.hpp"

namespace relay {
namespace {

// The request with its parent set to `root` unless it already names one: how a node created, pasted
// or instantiated in the template window ends up inside the template.
std::string with_parent(const std::string_view request, const std::string& root) {
    JsonParser parser(request);
    const auto parsed = parser.parse();
    const auto* fields = parsed ? parsed->object() : nullptr;
    if (!fields) return std::string{request};
    const auto* parent = field(*fields, "parent");
    if (parent && parent->string() && !parent->string()->empty()) return std::string{request};
    std::string text{request};
    const auto quoted = "\"" + root + "\"";
    for (const auto* empty : {"\"parent\":null", "\"parent\": null", "\"parent\":\"\"", "\"parent\": \"\""}) {
        if (const auto at = text.find(empty); at != std::string::npos) {
            text.replace(at, std::string_view{empty}.size(), "\"parent\":" + quoted);
            return text;
        }
    }
    const auto end = text.rfind('}');
    if (end == std::string::npos) return text;
    text.insert(end, ",\"parent\":" + quoted);
    return text;
}

std::string method_of(const std::string_view request) {
    JsonParser parser(request);
    const auto parsed = parser.parse();
    if (!parsed || !parsed->object()) return {};
    const auto* method = field(*parsed->object(), "method");
    return method && method->string() ? *method->string() : std::string{};
}

std::string refusal(const std::string_view request, const std::string_view message) {
    JsonParser parser(request);
    const auto parsed = parser.parse();
    const auto* id = parsed && parsed->object() ? field(*parsed->object(), "id") : nullptr;
    return "{\"id\":" + std::to_string(id && id->number() ? static_cast<long long>(*id->number()) : 0LL) + ",\"ok\":false,\"error\":\"" + json_escape(std::string{message}) + "\"}";
}

} // namespace

TemplateSession::TemplateSession(const std::string& project_filename, std::string name, Forwarder scripts,
                                 AssetRegistry* shared_assets)
    : name_(std::move(name)), scripts_(std::move(scripts)) {
    EngineConfig config;
    config.editor_mode = true;
    config.width = 64;
    config.height = 48;
    engine_ = std::make_unique<Engine>(config, shared_assets);
    protocol_ = std::make_unique<ControlProtocol>(*engine_);
    const auto opened = protocol_->handle("{\"id\":1,\"method\":\"project.open\",\"filename\":\"" +
                                          json_escape(project_filename) + "\"}");
    if (opened.find("\"ok\":true") == std::string::npos) {
        error_ = "cannot open the project: " + opened;
        return;
    }
    (void)protocol_->handle("{\"id\":2,\"method\":\"session.auto_approval\",\"enabled\":true}");
    std::string error;
    if (!load(error)) error_ = error;
}

TemplateSession::~TemplateSession() = default;

bool TemplateSession::load(std::string& error) {
    if (!engine_->project()) {
        error = "no project is open";
        return false;
    }
    auto loaded = load_template(engine_->project()->root(), "project:" + name_, error);
    if (!loaded) return false;
    engine_->scene().clear();
    const auto placed = place_template(engine_->scene(), *loaded, {}, {}, error);
    if (!placed) return false;
    root_ = *placed;
    // The template is the baseline: nothing to undo, nothing unsaved.
    engine_->scene_history().clear();
    saved_revision_ = engine_->scene_history().revision();
    return true;
}

std::string TemplateSession::handle(const std::string_view request) {
    const auto method = method_of(request);
    // A template is exactly one tree: its root cannot be deleted, copied beside itself or moved, and
    // anything that would add a second top-level node is adopted under the root afterwards.
    if (root_.valid() && (method == "scene.destroy" || method == "scene.destroy_many" || method == "scene.cut" ||
                          method == "scene.duplicate" || method == "scene.duplicate_many" || method == "scene.set_parent")) {
        JsonParser parser(request);
        const auto parsed = parser.parse();
        const auto* fields = parsed ? parsed->object() : nullptr;
        bool touches_root = false;
        if (fields) {
            const auto handle = root_.to_string();
            if (const auto* one = field(*fields, "entity"); one && one->string() && *one->string() == handle) touches_root = true;
            if (const auto* many = field(*fields, "entities"); many && many->array())
                for (const auto& item : *many->array())
                    if (item.string() && *item.string() == handle) touches_root = true;
            if (const auto* parent = field(*fields, "parent");
                method == "scene.set_parent" && (!parent || parent->is_null() || (parent->string() && parent->string()->empty())))
                touches_root = true;
        }
        if (touches_root)
            return refusal(request, "the template's root node cannot be deleted, duplicated or moved; edit its children instead");
    }
    if (method.starts_with("scripts.") && scripts_ && method != "scripts.write" && method != "scripts.create" &&
        method != "scripts.build" && method != "scripts.trust")
        return scripts_(request);
    if (method == "scene.save" || method == "scene.load" || method == "scene.clear" ||
        (method.starts_with("project.") && method != "project.status" && method != "project.list") ||
        (method.starts_with("runtime.") && method != "runtime.status") || method.starts_with("trace.") || method.starts_with("scripts."))
        return refusal(request, "not available while editing a template");
    if (root_.valid() && (method == "scene.create" || method == "scene.paste" || method == "templates.instantiate"))
        return protocol_->handle(with_parent(request, root_.to_string()));
    auto reply = protocol_->handle(request);
    // An imported model arrives at the top level; it joins the template under its root.
    if (root_.valid() && method == "assets.import_model" && reply.find("\"ok\":true") != std::string::npos)
        adopt_strays();
    return reply;
}

// Moves any top-level node other than the root (an imported model, a paste) under the root.
void TemplateSession::adopt_strays() {
    auto& scene = engine_->scene();
    std::vector<Entity> strays;
    for (const auto entity : scene.entities())
        if (entity != root_ && !scene.get(entity)->parent.valid()) strays.push_back(entity);
    for (const auto stray : strays)
        (void)protocol_->handle("{\"id\":3,\"method\":\"scene.set_parent\",\"entity\":\"" + stray.to_string() +
                                "\",\"parent\":\"" + root_.to_string() + "\"}");
}

bool TemplateSession::dirty() const { return engine_->scene_history().revision() != saved_revision_; }

bool TemplateSession::save(std::string& error) {
    if (!engine_->project() || !root_.valid() || !engine_->scene().contains(root_)) {
        error = "the template has no root node";
        return false;
    }
    for (const auto entity : engine_->scene().entities())
        if (entity != root_ && !engine_->scene().get(entity)->parent.valid()) {
            error = "\"" + engine_->scene().get(entity)->name + "\" is outside the template's root node";
            return false;
        }
    if (!save_template(engine_->scene(), root_, engine_->project()->root(), name_, true, error)) return false;
    saved_revision_ = engine_->scene_history().revision();
    return true;
}

bool TemplateSession::revert(std::string& error) { return load(error); }

void TemplateSession::tick() { engine_->tick(); }

} // namespace relay
