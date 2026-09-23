#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_selection.hpp"
#include "relay/editor/editor_timeline.hpp"
#include "relay/physics/collision.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/scene_edit.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}


relay::JsonValue request(relay::ControlProtocol& protocol, const std::string& method,
                         const std::string& fields = {}, bool succeeds = true) {
    const auto text = protocol.handle("{\"id\":1,\"method\":\"" + method + "\"" +
                                      (fields.empty() ? "" : "," + fields) + "}");
    relay::JsonParser parser(text);
    const auto value = parser.parse();
    check(value && value->object(), "protocol response must be JSON");
    const auto* ok = relay::field(*value->object(), "ok");
    if (!ok || !ok->boolean() || *ok->boolean() != succeeds)
        throw std::runtime_error("unexpected response: " + text);
    if (!succeeds) return *value;
    return *relay::field(*value->object(), "result");
}

std::string entities(std::initializer_list<relay::Entity> handles) {
    std::string result = "\"entities\":[";
    for (const auto entity : handles) {
        if (result.back() != '[') result += ',';
        result += '"' + entity.to_string() + '"';
    }
    return result + ']';
}

std::vector<relay::Entity> roots(const relay::JsonValue& value) {
    std::vector<relay::Entity> result;
    for (const auto& entry : *relay::field(*value.object(), "roots")->array())
        result.push_back(*relay::Entity::parse(*entry.string()));
    return result;
}

void auto_approval() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    const auto invoke = [&](const std::string& request) { return protocol.handle_agent(request); };
    check(invoke(R"({"id":1,"method":"session.auto_approval","enabled":true})").find("\"ok\":false") != std::string::npos, "agent cannot enable Auto approval");
    (void)request(protocol, "session.auto_approval", "\"enabled\":true");
    check(invoke(R"({"id":2,"method":"scene.create","name":"Automatic"})").find("\"ok\":true") != std::string::npos, "Auto approval permits mutation without grant");
    check(invoke(R"({"id":3,"method":"session.request","scope":"trace.replay"})").find("\"pending\":false") != std::string::npos, "automatic access requests do not wait");
    check(invoke(R"({"id":4,"method":"project.create","filename":"projects/automatic/project.relayproject","name":"Automatic"})").find("\"ok\":true") != std::string::npos, "Auto approval permits project switch");
    check(invoke(R"({"id":5,"method":"scene.clear"})").find("\"ok\":true") != std::string::npos, "Auto approval persists across project switch");
    check(invoke(R"({"id":6,"method":"scene.save","filename":"../escape.relayscene"})").find("\"ok\":false") != std::string::npos, "automatic permissions retain native path validation");
    check(invoke(R"({"id":7,"method":"session.audit"})").find("\"auto_approval\":true") != std::string::npos, "audit records automatic scope");
    std::filesystem::create_directories("traces");
    relay::TraceRecorder trace;
    std::string error;
    check(trace.start("traces/automatic.relay-trace.jsonl", engine.status().frame_index,
                      engine.fixed_delta_seconds(), engine.random_seed(), error), "create replay fixture");
    trace.record(engine.status().frame_index, "command", R"({"id":9,"method":"session.auto_approval","enabled":false})");
    trace.record(engine.status().frame_index, "command", R"({"id":10,"method":"scene.create","name":"Replayed"})");
    trace.record(engine.status().frame_index, "command", R"({"id":11,"method":"trace.replay","filename":"automatic.relay-trace.jsonl"})");
    check(trace.stop(error), "persist replay fixture");
    check(invoke(R"({"id":12,"method":"trace.replay","filename":"automatic.relay-trace.jsonl"})").find("\"ok\":true") != std::string::npos, "Auto approval permits bounded replay");
    check(engine.scene().entities().size() == 1, "replay executes public command and rejects recursive replay");
    check(invoke(R"({"id":13,"method":"session.status"})").find("\"auto_approval\":true") != std::string::npos, "replay cannot invoke host permission administration");
    check(invoke(R"({"id":14,"method":"session.audit"})").find("scene.create") != std::string::npos, "nested replay commands are audited");
    (void)request(protocol, "session.revoke");
    check(invoke(R"({"id":8,"method":"scene.clear"})").find("\"ok\":false") != std::string::npos, "revoke disables automatic access");
}

void session_capabilities() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    const auto revision = engine.scene_history().revision();
    auto agent = [&](const std::string& method, const std::string& fields = {}) {
        return protocol.handle_agent("{\"id\":7,\"method\":\"" + method + "\"" +
            (fields.empty() ? "" : "," + fields) + "}");
    };
    auto succeeds = [](const std::string& response) {
        relay::JsonParser parser(response);
        const auto value = parser.parse();
        check(value && value->object(), "agent response JSON");
        return *relay::field(*value->object(), "ok")->boolean();
    };
    check(!succeeds(agent("scene.create")), "default deny mutations");
    check(!succeeds(agent("scene.list")), "inspection also requires explicit grant");
    check(succeeds(agent("session.status")), "bootstrap grant inspection");
    check(engine.scene_history().revision() == revision, "denial cannot change revision");
    std::string error;
    check(protocol.set_agent_grants({"scene.create", "scene.list", "scene.save"}, error), "host grants exact methods");
    check(succeeds(agent("scene.create")), "approved mutation succeeds");
    check(!succeeds(agent("scene.clear")), "destructive method needs its own grant");
    check(!succeeds(agent("scene.save", "\"filename\":\"../escape.relay.json\"")), "grants cannot bypass path validation");
    check(!std::filesystem::exists("escape.relay.json"), "invalid save cannot write");
    check(!protocol.set_agent_grants({"scene.create", "*"}, error), "wildcards fail closed");
    check(!succeeds(agent("scene.create")), "invalid configuration revokes all grants");
    check(!protocol.set_agent_grants({"trace.replay"}, error), "compound replay not grantable");
    check(!succeeds(agent("trace.replay", "\"filename\":\"agent.relay-trace.jsonl\"")), "replay denied before effects");
    check(protocol.set_agent_grants({"scene.list"}, error), "read only scope granted");
    (void)request(protocol, "trace.start", "\"filename\":\"session.relay-trace.jsonl\"");
    check(succeeds(agent("scene.list")), "approved read succeeds");
    check(!succeeds(agent("scene.create")), "denied mutation under active trace");
    check(engine.trace().event_count() == 0, "reads and denials stay untraced");
    (void)request(protocol, "trace.stop");
    for (int i = 0; i < 270; ++i) (void)agent("scene.clear");
    const auto audit_text = agent("session.audit");
    relay::JsonParser parser(audit_text);
    const auto audit = parser.parse();
    const auto* result = relay::field(*audit->object(), "result");
    const auto* entries = relay::field(*result->object(), "entries")->array();
    check(entries->size() == 256, "audit is bounded");
    check(*relay::field(*result->object(), "oldest_sequence")->number() > 1, "eviction visible");
    check(!*relay::field(*entries->back().object(), "allowed")->boolean(), "denied action audited");
    check(succeeds(protocol.handle("{\"id\":1,\"method\":\"scene.create\"}")), "trusted UI retains protocol dispatch");
    relay::ControlProtocol another_session(engine);
    check(!succeeds(another_session.handle_agent("{\"id\":1,\"method\":\"scene.list\"}")), "grants never leak to another session");
}

void scoped_session_workflows() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    const auto first = engine.scene().create("Scoped first");
    const auto second = engine.scene().create("Scoped second");
    auto agent = [&](const std::string& method, const std::string& fields, bool ok) {
        const auto response = protocol.handle_agent("{\"id\":8,\"method\":\"" + method + "\"" + (fields.empty() ? "" : "," + fields) + "}");
        relay::JsonParser parser(response);
        const auto result = parser.parse();
        check(result && result->object(), "agent workflow JSON");
        if (*relay::field(*result->object(), "ok")->boolean() != ok) throw std::runtime_error(response);
        return ok ? *relay::field(*result->object(), "result") : *result;
    };
    const auto fields = "\"scope\":\"scene.set_transform\",\"kind\":\"entity\",\"target\":\"" + first.to_string() + "\"";
    const auto pending = agent("session.request", fields, true);
    const auto number = static_cast<unsigned>(*relay::field(*pending.object(), "request")->number());
    (void)agent("scene.set_transform", "\"entity\":\"" + first.to_string() + "\",\"px\":5", false);
    (void)agent("session.decide", "\"request\":" + std::to_string(number) + ",\"allow\":true", false);
    (void)request(protocol, "session.decide", "\"request\":" + std::to_string(number) + ",\"allow\":true");
    const auto depth = engine.scene_history().undo_depth();
    (void)agent("scene.set_transform", "\"entity\":\"" + first.to_string() + "\",\"px\":5,\"gesture\":77", true);
    (void)agent("scene.set_transform", "\"entity\":\"" + first.to_string() + "\",\"px\":7,\"gesture\":77", true);
    check(engine.scene_history().undo_depth() == depth + 1, "scoped agent gestures preserve one undo entry");
    (void)agent("scene.set_transform", "\"entity\":\"" + second.to_string() + "\",\"px\":9", false);
    (void)agent("scene.clear", {}, false);
    (void)request(protocol, "session.revoke", "\"scope\":\"scene.set_transform\"");
    (void)agent("scene.set_transform", "\"entity\":\"" + first.to_string() + "\",\"px\":8", false);
    (void)request(protocol, "session.grant", fields);
    (void)request(protocol, "scene.undo");
    (void)agent("scene.set_transform", "\"entity\":\"" + first.to_string() + "\",\"px\":8", false);
    (void)request(protocol, "session.grant", "\"scope\":\"scene.save\",\"kind\":\"file\",\"target\":\"scoped.relay.json\"");
    (void)agent("scene.save", "\"filename\":\"scoped.relay.json\"", true);
    (void)agent("scene.save", "\"filename\":\"other.relay.json\"", false);
    check(!std::filesystem::exists("scenes/other.relay.json"), "file scope cannot write another filename");
    (void)request(protocol, "session.grant", "\"scope\":\"runtime.step\",\"kind\":\"entity\",\"target\":\"" + first.to_string() + "\"", false);
    const auto stale = agent("session.request", "\"scope\":\"scene.create\"", true);
    (void)request(protocol, "project.create", "\"filename\":\"projects/scoped/project.relayproject\",\"name\":\"Scoped project\"");
    (void)agent("scene.save", "\"filename\":\"scoped.relay.json\"", false);
    (void)request(protocol, "session.decide", "\"request\":" + std::to_string(static_cast<unsigned>(*relay::field(*stale.object(), "request")->number())) + ",\"allow\":true", false);
    (void)request(protocol, "session.decide", "\"request\":" + std::to_string(static_cast<unsigned>(*relay::field(*stale.object(), "request")->number())) + ",\"allow\":false");
    (void)request(protocol, "session.export_audit", "\"filename\":\"scoped-audit.jsonl\"");
    check(std::filesystem::file_size(".relay/audits/scoped-audit.jsonl") > 0, "audit exported in bounded native directory");
    const auto bytes = std::filesystem::file_size(".relay/audits/scoped-audit.jsonl");
    (void)request(protocol, "session.export_audit", "\"filename\":\"scoped-audit.jsonl\"", false);
    check(std::filesystem::file_size(".relay/audits/scoped-audit.jsonl") == bytes, "existing audit cannot be overwritten");
    (void)agent("session.export_audit", "\"filename\":\"agent-audit.jsonl\"", false);
    (void)request(protocol, "session.export_audit", "\"filename\":\"../escape.jsonl\"", false);
    std::filesystem::create_directory("outside-audit");
    std::filesystem::rename(".relay/audits", ".relay/audits-backup");
    std::filesystem::create_directory_symlink(std::filesystem::absolute("outside-audit"), ".relay/audits");
    (void)request(protocol, "session.export_audit", "\"filename\":\"escape.jsonl\"", false);
    check(!std::filesystem::exists("outside-audit/escape.jsonl"), "audit export refuses symlink ancestors");
    std::filesystem::remove(".relay/audits");
    std::filesystem::rename(".relay/audits-backup", ".relay/audits");
    (void)agent("bridge.poll", {}, false);
    (void)agent("chat.submit", "\"message\":\"Hello\"", false);
    (void)request(protocol, "chat.submit", "\"message\":\"Hello\"", false);
}

void clipboard_and_groups() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    auto& scene = engine.scene();
    scene.clear();
    const auto parent = scene.create("Group");
    const auto child = scene.create("Child", parent);
    const auto other = scene.create("Other");
    scene.get(parent)->model_node = relay::ModelNode{parent, 0};
    scene.get(child)->model_node = relay::ModelNode{parent, 1};
    check(scene.set_camera(parent, relay::Camera{}), "attach camera");
    const auto depth = engine.scene_history().undo_depth();
    const auto revision = engine.scene_history().revision();
    (void)request(protocol, "scene.copy", entities({child, parent, other, child}));
    check(engine.clipboard().nodes.size() == 3 && engine.clipboard().roots.size() == 2,
          "overlapping selections copy each entity exactly once");
    check(engine.scene_history().revision() == revision && engine.scene_history().undo_depth() == depth,
          "copy does not dirty the scene or add undo history");
    (void)request(protocol, "scene.destroy_many", entities({parent, child, other}));
    check(scene.entities().empty() && engine.scene_history().undo_depth() == depth + 1,
          "group deletion is one undo entry");
    const auto pasted = roots(request(protocol, "scene.paste"));
    check(pasted.size() == 2 && scene.entities().size() == 3, "clipboard survives source deletion");
    check(scene.get(pasted[0])->model_node->root == pasted[0] &&
              scene.get(pasted[0])->camera && !scene.get(pasted[0])->camera->active,
          "paste remaps model instance and does not take the active camera");
    for (const auto entity : scene.entities())
        if (scene.get(entity)->parent == pasted[0])
            check(scene.get(entity)->model_node->root == pasted[0], "paste rebinds descendant");
    (void)request(protocol, "scene.undo");
    check(scene.entities().empty(), "one undo removes the whole paste");
    (void)request(protocol, "scene.undo");
    check(scene.entities().size() == 3 && scene.contains(parent), "one undo restores group deletion");
    (void)request(protocol, "scene.cut", entities({parent, child}));
    check(!scene.contains(parent) && scene.contains(other) && engine.clipboard().nodes.size() == 2,
          "cut copies before removing the selected forest");
    const auto cut_copy = roots(request(protocol, "scene.paste", "\"parent\":\"" + other.to_string() + "\""));
    check(scene.get(cut_copy.front())->parent == other, "paste into specified parent");
    (void)request(protocol, "scene.undo");
    (void)request(protocol, "scene.undo");
    const auto before = scene.serialize_json();
    const auto before_revision = engine.scene_history().revision();
    (void)request(protocol, "scene.cut", entities({parent, {999999, 1}}), false);
    check(scene.serialize_json() == before && engine.scene_history().revision() == before_revision,
          "invalid group operations have no partial effects");
    check(!engine.scene_history().execute("Failed edit", [&](relay::Scene& edited) {
              (void)edited.destroy(parent);
              return false;
          }) && scene.serialize_json() == before,
          "history restores scene when an operation fails after a partial mutation");
    const auto duplicates = roots(request(protocol, "scene.duplicate_many", entities({child, parent, other})));
    check(duplicates.size() == 2 && scene.entities().size() == 6,
          "group duplication is a forest copy with no overlapping descendants");
    (void)request(protocol, "scene.undo");
    const std::string delta = ",\"delta\":[1,0,0,0,0,1,0,0,0,0,1,0,2,0,0,1],\"gesture\":42";
    const auto group_depth = engine.scene_history().undo_depth();
    (void)request(protocol, "scene.transform_many", entities({parent, child, other}) + delta);
    (void)request(protocol, "scene.transform_many", entities({parent, child, other}) + delta);
    check(scene.get(parent)->transform.position.x == 4 && scene.get(other)->transform.position.x == 4 &&
              scene.get(child)->transform.position.x == 0 && engine.scene_history().undo_depth() == group_depth + 1,
          "group gesture transforms roots once and folds all updates into one undo");
    (void)request(protocol, "scene.undo");
    check(scene.get(parent)->transform.position.x == 0, "undo restores entire group gesture");
    (void)request(protocol, "scene.transform_many", entities({parent}) + ",\"delta\":[1,2]", false);
    (void)request(protocol, "scene.copy", "\"entities\":[\"broken\"]", false);
    (void)request(protocol, "scene.copy", "\"entities\":[]", false);
    (void)request(protocol, "scene.copy", "\"entities\":[1]", false);
    const auto escaped = request(protocol, "scene.create", "\"name\":\"Unicode \\u263a \\u0001\"");
    check(relay::field(*escaped.object(), "entity")->string(), "shared request parser supports escaped Unicode names");
    (void)request(protocol, "scene.copy", "\"entities\":[\"4294967296:1\"]", false);
    (void)request(protocol, "scene.set_animations", "\"entities\":[\"4294967296:1\"],\"playing\":true", false);
    (void)request(protocol, "scene.paste", "\"parent\":\"99999:1\"", false);
    const auto rotated = scene.create("Rotated parent");
    const auto nested = scene.create("Nested", rotated);
    check(scene.set_transform(rotated, relay::Transform{{}, {0, 0, 90}, {2, 3, 4}}), "rotated parent transform");
    (void)request(protocol, "scene.transform_many", entities({nested}) + delta);
    check(std::abs(scene.get(nested)->transform.position.y + 2.0 / 3.0) < 1e-5,
          "world group translation converts through rotated nonuniform parent scale");
    (void)request(protocol, "scene.undo");
    check(scene.set_transform(rotated, relay::Transform{{}, {}, {0, 1, 1}}), "singular parent fixture");
    const auto singular_before = scene.serialize_json();
    (void)request(protocol, "scene.transform_many", entities({parent, nested}) + delta, false);
    check(scene.serialize_json() == singular_before, "singular member prevents partially moving the rest of a group");
    (void)request(protocol, "scene.copy", entities({child}));
    check(!engine.clipboard().nodes.front().record.model_node,
          "partial model copy drops external instance binding before changing scenes");
}

void selections_and_timeline() {
    relay::EditorSelection selection;
    const std::vector<std::string> visible{"a", "b", "c", "d"};
    selection.assign("c");
    selection.click("a", false, true, visible);
    check(selection.handles.size() == 3 && selection.primary_handle == "a",
          "reverse range selection includes both endpoints");
    selection.click("b", true, false);
    check(!selection.contains("b") && selection.contains("a") && selection.contains("c"), "toggle removes one row");
    selection.prune([](const auto& handle) { return handle == "c"; });
    check(selection.primary_handle == "c" && selection.handles.size() == 1, "stale active handle falls back to survivor");
    check(relay::timeline_time(1.5, 2, 30, false) == 2 && relay::timeline_time(-1, 2, 30, true) == 0,
          "ruler clamps both drag endpoints");
    check(std::abs(relay::timeline_time(.255, 2, 30, true) - 0.5) < 1e-8, "ruler snaps to frame grid");
    check(relay::timeline_step(.1, 2, 30, 1) > .1 && relay::timeline_step(.1, 2, 30, -1) < .1 &&
              relay::timeline_step(2, 2, 30, 1) == 2 && relay::timeline_step(0, 2, 30, -1) == 0,
          "frame stepping advances at exact frames and clamps endpoints");
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    relay::ModelAsset model;
    model.name = "timeline.model";
    model.nodes = {"Bone"};
    relay::AnimationClip clip;
    clip.name = "Move";
    clip.duration_seconds = 2;
    relay::NodeTrack track;
    track.node = 0;
    track.positions = {{0, {}}, {2, {1, 0, 0}}};
    clip.tracks.push_back(track);
    model.clips = {clip};
    auto dense = clip;
    dense.name = "Dense";
    dense.tracks.front().positions.clear();
    for (int i = 0; i < 300; ++i)
        dense.tracks.front().positions.push_back(relay::VectorKey{2.0 * i / 299.0, {}});
    model.clips.push_back(dense);
    check(engine.assets().register_model(model), "timeline model registered");
    const auto first = engine.scene().create("First");
    const auto second = engine.scene().create("Second");
    check(engine.scene().set_animator(first, relay::Animator{model.name}) &&
              engine.scene().set_animator(second, relay::Animator{model.name}), "timeline animators attached");
    const auto depth = engine.scene_history().undo_depth();
    const auto channel = request(protocol, "animation.clip", "\"model\":\"timeline.model\",\"clip\":0");
    check(relay::field(*channel.object(), "channels")->array()->size() == 1 &&
              engine.scene_history().undo_depth() == depth, "key markers are readable without a scene edit");
    const auto dense_keys = request(protocol, "animation.clip", "\"model\":\"timeline.model\",\"clip\":1");
    const auto& dense_channel = *relay::field(*dense_keys.object(), "channels")->array()->front().object();
    const auto& times = *relay::field(dense_channel, "times")->array();
    check(times.size() == 256 && *times.front().number() == 0 && *times.back().number() == 2 &&
              *relay::field(dense_channel, "key_count")->number() == 300,
          "bounded key-marker sampling retains endpoints and reports the full count");
    const auto group = entities({first, second});
    (void)request(protocol, "scene.set_animations", group + ",\"time_seconds\":0.2,\"gesture\":8");
    (void)request(protocol, "scene.set_animations", group + ",\"time_seconds\":0.6,\"gesture\":8");
    check(engine.scene().get(first)->animator->time_seconds == .6 &&
              engine.scene().get(second)->animator->time_seconds == .6 &&
              engine.scene_history().undo_depth() == depth + 1,
          "timeline scrub synchronizes tracks in one undo entry");
    (void)request(protocol, "scene.undo");
    check(engine.scene().get(first)->animator->time_seconds == 0 &&
              engine.scene().get(second)->animator->time_seconds == 0, "timeline undo restores every track");
    (void)request(protocol, "scene.set_animations", group + ",\"time_seconds\":9,\"playing\":true");
    check(engine.scene().get(first)->animator->time_seconds == 2, "timeline seeks clamp to clip duration");
    const auto before = engine.scene().serialize_json();
    (void)request(protocol, "scene.set_animations", entities({first, {9999, 1}}) + ",\"playing\":false", false);
    check(engine.scene().serialize_json() == before, "invalid timeline track cannot partially change playback");
}

void projects() {
    relay::Engine engine;
    relay::ControlProtocol protocol(engine);
    (void)request(protocol, "project.create", "\"filename\":\"projects/demo/project.relayproject\",\"name\":\"Demo\"");
    check(engine.project() && engine.scene().entities().empty(), "new project starts empty");
    std::filesystem::create_directories("projects/demo/models/nested");
    std::ofstream("projects/demo/models/nested/probe.obj") << "# project-local model";
    std::ofstream("projects/demo/readme.txt") << "project file";
    std::filesystem::create_directories("assets");
    std::ofstream("assets/outside.obj") << "# outside";
    const auto available = request(protocol, "assets.available");
    const auto contains_file = [&](const char* list, const char* name) {
        for (const auto& entry : *relay::field(*available.object(), list)->array())
            if (entry.string() && *entry.string() == name) return true;
        return false;
    };
    check(contains_file("models", "models/nested/probe.obj") && contains_file("files", "readme.txt") &&
          !contains_file("models", "outside.obj"), "asset discovery is recursive and project-local");

    (void)engine.scene().create("First scene");
    (void)request(protocol, "scene.save", "\"filename\":\"one.relay.json\"");
    (void)request(protocol, "project.add_scene", "\"scene_file\":\"one.relay.json\"");
    check(engine.project()->startup_scene == "one.relay.json", "first project member becomes startup");
    engine.scene().clear();
    (void)engine.scene().create("Second scene");
    (void)request(protocol, "scene.save", "\"filename\":\"two.relay.json\"");
    (void)request(protocol, "project.add_scene", "\"scene_file\":\"two.relay.json\"");
    (void)request(protocol, "project.set_startup", "\"scene_file\":\"two.relay.json\"");
    (void)request(protocol, "project.close");
    check(!engine.project() && engine.scene().entities().size() == 1, "closing project keeps authored scene");
    (void)request(protocol, "project.open", "\"filename\":\"projects/demo/project.relayproject\"");
    check(engine.project()->scenes.size() == 2 &&
              engine.scene().get(engine.scene().entities().front())->name == "Second scene",
          "project reopening restores membership and loads selected startup scene");
    const auto before = engine.scene().serialize_json();
    (void)request(protocol, "project.create", "\"filename\":\"projects/demo/project.relayproject\",\"name\":\"Overwrite\"", false);
    check(engine.scene().serialize_json() == before && engine.project()->name == "Demo",
          "new project refuses overwrite without changing active state");
    (void)request(protocol, "project.add_scene", "\"scene_file\":\"missing.relay.json\"", false);
    (void)request(protocol, "project.set_startup", "\"scene_file\":\"missing.relay.json\"", false);
    (void)request(protocol, "project.open", "\"filename\":\"../demo.relayproject\"", false);
    auto broken = *engine.project();
    broken.filename = "projects/broken/project.relayproject";
    broken.scenes = {"missing.relay.json"};
    broken.startup_scene = broken.scenes.front();
    std::string error;
    check(relay::save_project(broken, error), "missing startup fixture saved");
    (void)request(protocol, "project.open", "\"filename\":\"projects/broken/project.relayproject\"", false);
    check(engine.scene().serialize_json() == before && engine.project()->name == "Demo",
          "missing startup leaves scene and active project unchanged");
    (void)request(protocol, "project.remove_scene", "\"scene_file\":\"two.relay.json\"");
    check(engine.project()->startup_scene == "one.relay.json" && std::filesystem::exists("projects/demo/scenes/two.relay.json"),
          "removing startup chooses another member without deleting files");
    (void)request(protocol, "project.remove_scene", "\"scene_file\":\"one.relay.json\"");
    check(engine.project()->startup_scene.empty(), "removing last member clears startup");
    (void)request(protocol, "project.open", "\"filename\":\"projects/demo/project.relayproject\"");
    check(engine.scene().entities().empty(), "empty project opens an empty scene");
    std::filesystem::create_directory("outside");
    std::filesystem::create_directory_symlink(std::filesystem::absolute("outside"), "projects-link");
    check(!relay::workspace_file("projects-link", "escape.relayproject", ".relayproject"),
          "project path refuses symlink directory");
    std::filesystem::create_symlink(std::filesystem::absolute("projects/demo/project.relayproject"),
                                    "projects/linked.relayproject");
    (void)request(protocol, "project.open", "\"filename\":\"projects/linked.relayproject\"", false);
}

void asset_files() {
    relay::Engine engine({1280, 720, 1.0 / 60.0, 0x52454C4159ULL, true});
    relay::ControlProtocol protocol(engine);
    (void)request(protocol, "project.create",
                  "\"filename\":\"projects/files/project.relayproject\",\"name\":\"Files\"");
    (void)engine.scene().create("Member");
    (void)request(protocol, "scene.save", "\"filename\":\"main.relay.json\"");
    (void)request(protocol, "project.add_scene", "\"scene_file\":\"main.relay.json\"");
    std::ofstream("projects/files/robot.obj") << "o robot\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    std::ofstream("projects/files/notes.txt") << "notes";
    std::ofstream("projects/files/.hidden") << "hidden";
    const auto names = [&](const std::string& directory) {
        const auto listing = request(protocol, "assets.browse",
                                     "\"directory\":\"" + directory + "\"");
        std::vector<std::string> result;
        for (const auto& entry : *relay::field(*listing.object(), "entries")->array()) {
            const auto& object = *entry.object();
            auto label = *relay::field(object, "name")->string();
            if (*relay::field(object, "type")->string() == "folder") label += '/';
            if (*relay::field(object, "importable")->boolean()) label += '*';
            if (*relay::field(object, "protected")->boolean()) label += '!';
            result.push_back(label);
        }
        return result;
    };
    check(names("") == std::vector<std::string>{"scenes/!", "notes.txt", "project.relayproject!",
                                                "robot.obj*"},
          "browse lists folders first, marks models, protects project-owned files, hides dot files");
    check(names("scenes") == std::vector<std::string>{"main.relay.json!"},
          "browse marks project member scenes as protected");
    (void)request(protocol, "assets.create_folder", "\"path\":\"models\"");
    (void)request(protocol, "assets.create_folder", "\"path\":\"models\"", false);
    (void)request(protocol, "assets.create_folder", "\"path\":\"missing/child\"", false);
    (void)request(protocol, "assets.create_folder", "\"path\":\"../escape\"", false);
    (void)request(protocol, "assets.import_model",
                  "\"filename\":\"robot.obj\",\"instantiate\":false");
    (void)request(protocol, "assets.move", "\"from\":\"robot.obj\",\"to\":\"models/robot.obj\"");
    relay::ImportManifest manifest;
    std::string manifest_error;
    check(manifest.load("projects/files", manifest_error) && manifest.entries().size() == 1U &&
              manifest.entries().front().source == "models/robot.obj",
          "moving a model re-points its import record");
    (void)request(protocol, "assets.move", "\"from\":\"models\",\"to\":\"meshes\"");
    check(manifest.load("projects/files", manifest_error) &&
              manifest.entries().front().source == "meshes/robot.obj" &&
              names("meshes") == std::vector<std::string>{"robot.obj*"},
          "renaming a folder moves its contents and their import records");
    (void)request(protocol, "assets.move", "\"from\":\"meshes\",\"to\":\"meshes/inner\"", false);
    (void)request(protocol, "assets.move", "\"from\":\"notes.txt\",\"to\":\"meshes/robot.obj\"", false);
    (void)request(protocol, "assets.move",
                  "\"from\":\"project.relayproject\",\"to\":\"renamed.relayproject\"", false);
    (void)request(protocol, "assets.move", "\"from\":\"scenes\",\"to\":\"levels\"", false);
    (void)request(protocol, "assets.delete", "\"path\":\"scenes/main.relay.json\"", false);
    const auto deleted = request(protocol, "assets.delete", "\"path\":\"notes.txt\"");
    const auto trash = *relay::field(*deleted.object(), "trash")->string();
    check(!std::filesystem::exists("projects/files/notes.txt") &&
              std::filesystem::exists("projects/files/" + trash) && trash.starts_with(".relay-trash/"),
          "deleting moves the file into the hidden project trash");
    check(names("") == std::vector<std::string>{"meshes/", "scenes/!", "project.relayproject!"},
          "trash stays hidden from the browser");
    (void)request(protocol, "assets.browse", "\"directory\":\"gone\"", false);
    std::filesystem::create_directories("projects/files/textures/wood");
    std::ofstream("projects/files/textures/wood/Oak.PNG") << "png";
    std::ofstream("projects/files/meshes/robot-notes.md") << "notes";
    const auto search = [&](const std::string& fields) {
        const auto found = request(protocol, "assets.search", fields);
        std::vector<std::string> result;
        for (const auto& entry : *relay::field(*found.object(), "entries")->array())
            result.push_back(*relay::field(*entry.object(), "path")->string() + ':' +
                             *relay::field(*entry.object(), "kind")->string());
        return result;
    };
    check(search("\"query\":\"ROBOT\"") ==
              std::vector<std::string>{"meshes/robot-notes.md:text", "meshes/robot.obj:model"},
          "search matches names case-insensitively in nested folders");
    check(search("\"kinds\":[\"image\"]") == std::vector<std::string>{"textures/wood/Oak.PNG:image"},
          "a kind filter alone lists every asset of that kind");
    check(search("\"query\":\"o\",\"kinds\":[\"folder\",\"scene\"]") ==
              std::vector<std::string>{"scenes/main.relay.json:scene", "textures/wood:folder"},
          "query and kinds combine, sorted by path");
    check(search("\"query\":\"notes.txt\"").empty(), "search skips the hidden trash");
    (void)request(protocol, "assets.search", "\"kinds\":[\"bogus\"]", false);
    check(engine.run_game(), "asset test game session starts");
    (void)request(protocol, "assets.create_folder", "\"path\":\"during-game\"", false);
    check(engine.stop_game(), "asset test game session stops");
}

void demo_project() {
    // The committed dev-build demo must open, resolve every asset and simulate.
    std::filesystem::create_directories("examples");
    std::filesystem::copy(std::filesystem::path{RELAY_TEST_SOURCE_DIR} / "examples/demo",
                          "examples/demo", std::filesystem::copy_options::recursive);
    relay::Engine engine({1280, 720, 1.0 / 60.0, 0x52454C4159ULL, true});
    relay::ControlProtocol protocol(engine);
    const auto opened = request(protocol, "project.open",
                                "\"filename\":\"examples/demo/demo.relayproject\"");
    check(*relay::field(*opened.object(), "scene_file")->string() == "showcase.relay.json",
          "demo project opens its showcase startup scene");
    std::size_t renderers = 0, colliders = 0, bodies = 0;
    relay::Entity wrecking_ball{};
    for (const auto entity : engine.scene().entities()) {
        const auto& record = *engine.scene().get(entity);
        if (record.mesh_renderer) {
            ++renderers;
            check(engine.assets().find_mesh(record.mesh_renderer->mesh) &&
                      engine.assets().find_material(record.mesh_renderer->material),
                  "every demo renderer resolves its imported mesh and material");
        }
        colliders += record.collider.has_value();
        bodies += record.physics_body.has_value();
        if (record.name == "Wrecking ball") wrecking_ball = entity;
    }
    check(renderers >= 25U && bodies >= 10U && wrecking_ball.valid(),
          "demo scene showcases rendering and physics");
    check(relay::collision_debug_boxes(engine.scene(), true, &engine.assets()).boxes.size() == colliders,
          "every demo collider, including convex and mesh shapes, builds");
    const double start = engine.scene().get(wrecking_ball)->transform.position.y;
    check(engine.run_game(), "demo scene runs");
    engine.step(120);
    check(engine.scene().get(wrecking_ball)->transform.position.y < start - 2.0 &&
              !engine.physics().contact_events().events.empty(),
          "demo bodies fall and collide during Run Game");
    check(engine.stop_game(), "demo game session stops");
}
} // namespace

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    int result = 0;
    try {
        auto_approval();
        session_capabilities();
        scoped_session_workflows();
        clipboard_and_groups();
        selections_and_timeline();
        projects();
        asset_files();
        demo_project();
        std::cout << "Background editor workflow tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
