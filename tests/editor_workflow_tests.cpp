#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/input.hpp"
#include "relay/core/json.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_selection.hpp"
#include "relay/editor/editor_timeline.hpp"
#include "relay/physics/collision.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/scene/components.hpp"
#include "relay/scene/node_types.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/scene_io.hpp"
#include "relay/scene/templates.hpp"
#include "relay/scene/scene_edit.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

std::string node_type_of(relay::ControlProtocol& protocol, relay::Entity entity) {
    const auto inspected = request(protocol, "scene.inspect", "\"entity\":\"" + entity.to_string() + '"');
    const auto* object = inspected.object();
    const auto* node = object ? relay::field(*object, "entity") : nullptr;
    const auto* fields = node && node->object() ? node->object() : object;
    const auto* type = fields ? relay::field(*fields, "type") : nullptr;
    return type && type->string() ? *type->string() : std::string{};
}

relay::Entity created_entity(const relay::JsonValue& result) {
    return *relay::Entity::parse(*relay::field(*result.object(), "entity")->string());
}

void components_and_templates() {
    relay::Engine engine({64, 48, 1.0 / 60.0, 0x52454c4159ULL, true});
    relay::ControlProtocol protocol(engine);
    (void)request(protocol, "project.create",
                  "\"filename\":\"projects/prefabs/project.relayproject\",\"name\":\"Prefabs\"");
    const auto node = engine.scene().create("Thing");
    const auto field = "\"entity\":\"" + node.to_string() + '"';
    check(node_type_of(protocol, node) == "Node", "a node with only a Transform is a Node");

    request(protocol, "component.add", field + ",\"component\":\"mesh_renderer\"");
    check(node_type_of(protocol, node) == "StaticMesh", "a mesh renderer makes a StaticMesh");
    request(protocol, "component.add", field + ",\"component\":\"mesh_renderer\"", false);
    request(protocol, "component.remove", field + ",\"component\":\"transform\"", false);
    request(protocol, "component.add", field + ",\"component\":\"camera\"");
    check(node_type_of(protocol, node) == "Camera" && engine.scene().get(node)->camera->active,
          "a camera outranks the mesh, and the scene's first camera becomes active");
    request(protocol, "component.remove", field + ",\"component\":\"mesh_renderer\"");
    request(protocol, "component.remove", field + ",\"component\":\"camera\"");
    check(node_type_of(protocol, node) == "Node", "removing components returns the node to Node");
    for (const std::string id : {"light", "collider", "physics_body", "keyframes"})
        request(protocol, "component.add", field + ",\"component\":\"" + id + '"');
    const auto* record = engine.scene().get(node);
    check(record->light && record->collider && record->physics_body &&
              record->transform_animation && record->transform_animation->keys.size() == 1U,
          "every addable engine component can be added with defaults");
    check(node_type_of(protocol, node) == "RigidBody",
          "a dynamic body outranks a light, so the node is a RigidBody");
    request(protocol, "component.remove", field + ",\"component\":\"physics_body\"");
    check(node_type_of(protocol, node) == "StaticBody", "a collider without a dynamic body is static");
    request(protocol, "component.remove", field + ",\"component\":\"collider\"");
    check(node_type_of(protocol, node) == "PointLight", "light kinds are subtypes of Light");
    request(protocol, "component.add", field + ",\"component\":\"script\"", false);
    request(protocol, "component.add", field + ",\"component\":\"script\",\"behaviour\":\"First\"");
    request(protocol, "component.add", field + ",\"component\":\"script\",\"behaviour\":\"Second\"");
    request(protocol, "component.remove", field + ",\"component\":\"script\",\"index\":0");
    check(engine.scene().get(node)->scripts.size() == 1U &&
              engine.scene().get(node)->scripts[0].behaviour == "Second",
          "script components are removed by index");
    request(protocol, "scene.undo");
    check(engine.scene().get(node)->scripts.size() == 2U, "component removal is undoable");
    const auto types = request(protocol, "component.types");
    check(relay::field(*types.object(), "engine")->array()->size() == relay::engine_components().size(),
          "component.types lists every engine component");

    // Node types form a tree; each creatable type creates a node that reports that type.
    const auto tree = request(protocol, "nodes.types");
    const auto& type_list = *relay::field(*tree.object(), "types")->array();
    check(type_list.size() == relay::node_types().size(), "nodes.types lists the whole tree");
    const auto inherited = relay::node_type_components("RigidBody");
    check(inherited == std::vector<std::string_view>{"transform", "collider", "physics_body"},
          "a type inherits its ancestors' components");
    for (const auto& entry : type_list) {
        const auto* type = entry.object();
        const auto id = *relay::field(*type, "id")->string();
        if (!*relay::field(*type, "creatable")->boolean()) {
            request(protocol, "scene.create", "\"type\":\"" + id + '"', false);
            continue;
        }
        const auto created = created_entity(request(protocol, "scene.create", "\"type\":\"" + id + '"'));
        check(node_type_of(protocol, created) == id, ("a created node reports its type: " + id).c_str());
        check(engine.scene().get(created)->name ==
                  (id == "Node" ? std::string("Entity") : std::string(relay::find_node_type(id)->name)),
              "a typed node is named after its type");
    }
    check(relay::field(*request(protocol, "templates.list").object(), "templates")->array()->empty(),
          "a project starts without templates");

    // A saved node tree is a prefab: instantiating it copies the whole subtree.
    const auto enemy = created_entity(request(protocol, "scene.create",
                                              "\"type\":\"RigidBody\",\"name\":\"Enemy\""));
    const auto weapon = engine.scene().create("Weapon", enemy);
    (void)engine.scene().set_light(weapon, relay::Light{});
    request(protocol, "component.add", "\"entity\":\"" + enemy.to_string() +
                                          "\",\"component\":\"script\",\"behaviour\":\"EnemyAi\"");
    request(protocol, "scene.set_script_property", "\"entity\":\"" + enemy.to_string() +
                                                      "\",\"index\":0,\"property\":\"speed\",\"number\":4");
    request(protocol, "templates.save", "\"entity\":\"" + enemy.to_string() + "\",\"name\":\"Enemy\"");
    check(std::filesystem::exists("projects/prefabs/templates/Enemy.relay-template.json"),
          "templates are saved into the project's templates folder");
    request(protocol, "templates.save",
            "\"entity\":\"" + enemy.to_string() + "\",\"name\":\"Enemy\"", false);
    request(protocol, "templates.save",
            "\"entity\":\"" + enemy.to_string() + "\",\"name\":\"Enemy\",\"replace\":true");
    request(protocol, "templates.save", "\"entity\":\"" + enemy.to_string() + "\",\"name\":\"../x\"",
            false);
    const auto project_listing = request(protocol, "templates.list");
    const auto& with_project = *relay::field(*project_listing.object(), "templates")->array();
    check(with_project.size() == 1U &&
              *relay::field(*with_project.back().object(), "id")->string() == "project:Enemy" &&
              *relay::field(*with_project.back().object(), "type")->string() == "RigidBody",
          "saved templates are listed with their root's type");
    const auto parent = engine.scene().create("Spawner");
    const auto copy = created_entity(request(
        protocol, "templates.instantiate",
        "\"template\":\"project:Enemy\",\"parent\":\"" + parent.to_string() + "\",\"name\":\"Enemy 2\""));
    const auto* copied = engine.scene().get(copy);
    check(copy != enemy && copied->name == "Enemy 2" && copied->parent == parent &&
              copied->scripts.size() == 1U && copied->scripts[0].properties.size() == 1U &&
              copied->scripts[0].properties[0].number == 4.0,
          "a project template recreates the root with its components and script values");
    bool copied_child = false;
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->parent == copy &&
            engine.scene().get(entity)->name == "Weapon" && engine.scene().get(entity)->light)
            copied_child = entity != weapon;
    check(copied_child, "a project template recreates the root's children as new nodes");
    (void)engine.scene().set_name(enemy, "Changed");
    check(engine.scene().get(copy)->name == "Enemy 2", "template copies are independent");
    request(protocol, "scene.undo");
    check(!engine.scene().contains(copy), "template instantiation is one undoable step");
    request(protocol, "templates.instantiate", "\"template\":\"project:Missing\"", false);

    // Scripts would need a trusted project; the edit guard is what is under test here.
    for (const auto entity : engine.scene().entities()) (void)engine.scene().set_scripts(entity, {});
    check(engine.run_game(), "prefab scene runs");
    request(protocol, "component.add", field + ",\"component\":\"camera\"", false);
    request(protocol, "templates.instantiate", "\"template\":\"project:Enemy\"", false);
    request(protocol, "scene.create", "\"type\":\"Camera\"", false);
    check(engine.stop_game(), "prefab scene stops");

    // Version 12 held at most one script per entity; it migrates to the first script component.
    relay::Scene legacy;
    const auto scripted = legacy.create("Legacy");
    auto document = legacy.serialize_json();
    const auto version = "\"version\":" + std::to_string(relay::scene_file_version);
    document.replace(document.find(version), version.size(), "\"version\":12");
    document.replace(document.find("\"scripts\":[]"), 12,
                     "\"script\":{\"behaviour\":\"Mover\",\"enabled\":false}");
    std::ofstream("legacy-script.relay.json") << document;
    const auto migrated = relay::load_scene_file("legacy-script.relay.json");
    check(migrated && migrated.migrated &&
              migrated.state->slots[scripted.index].record.scripts.size() == 1U &&
              migrated.state->slots[scripted.index].record.scripts[0].behaviour == "Mover" &&
              !migrated.state->slots[scripted.index].record.scripts[0].enabled,
          "a version 12 script becomes the first script component");
}

void input_mapping() {
    using Query = relay::InputState::Query;
    relay::InputState input;
    // A tap shorter than one step still reads as one press, held for that step.
    input.apply("key:down:space");
    input.apply("key:up:space");
    input.begin_step();
    check(input.action("jump", Query::pressed) && input.action("jump", Query::held) &&
              input.action("jump", Query::released),
          "a tap within one step is pressed, held and released that step");
    input.begin_step();
    check(!input.action("jump", Query::pressed) && !input.action("jump", Query::held),
          "the tap is gone on the next step");
    input.apply("key:down:space");
    input.apply("key:down:space"); // A second down without an up is not a second press.
    input.begin_step();
    check(input.action("jump", Query::pressed), "holding space presses jump");
    input.begin_step();
    check(input.action("jump", Query::held) && !input.action("jump", Query::pressed),
          "a held key is held but not pressed again");
    input.apply("gamepad_button:down:0:a");
    input.apply("key:up:space");
    input.begin_step();
    check(input.action("jump", Query::held) && !input.action("jump", Query::released),
          "releasing one binding while another holds the action is not a release");

    input.apply("key:down:d");
    input.begin_step();
    check(input.axis("move_x") == 1.0, "a positive key drives an axis to +1");
    input.apply("key:down:a");
    input.begin_step();
    check(input.axis("move_x") == 0.0, "opposing keys cancel");
    input.apply("key:up:a");
    input.apply("key:up:d");
    input.apply("gamepad_axis:0:leftx:3276");
    input.begin_step();
    check(input.axis("move_x") == 0.0, "stick travel inside the deadzone reads as zero");
    input.apply("gamepad_axis:0:leftx:19660");
    input.begin_step();
    check(std::abs(input.axis("move_x") - 0.5) < 1e-3, "travel past the deadzone is rescaled");
    input.apply("gamepad_axis:0:lefty:-32767");
    input.begin_step();
    check(input.axis("move_y") == 1.0, "pushing the stick forward is +1: stick Y is inverted");
    input.apply("key:down:d");
    input.begin_step();
    check(input.axis("move_x") == 1.0, "the strongest binding wins over a half-pushed stick");
    input.apply("key:up:d");
    input.apply("gamepad_axis:0:right_trigger:30000");
    input.begin_step();
    check(input.action("fire", Query::pressed), "a trigger past half travel presses an action");
    input.apply("mouse_motion:10:20:3:4");
    input.apply("mouse_motion:11:22:1:2");
    input.begin_step();
    check(input.mouse_dx() == 4.0 && input.mouse_dy() == 6.0 && input.mouse_x() == 11.0,
          "mouse movement accumulates over one step");
    input.begin_step();
    check(input.mouse_dx() == 0.0, "mouse movement resets each step");
    input.apply("input:reset");
    input.begin_step();
    check(!input.action("jump", Query::held) && input.axis("move_x") == 0.0 &&
              input.axis("move_y") == 0.0,
          "releasing input clears held buttons and sticks");
    input.apply("key:down:32");
    input.begin_step();
    check(!input.action("jump", Query::held),
          "old keycode events (32 was Space) match no physical key binding");

    std::string error;
    auto parsed = relay::parse_input_map(relay::input_map_json(relay::default_input_map()), error);
    check(parsed && relay::input_map_json(*parsed) == relay::input_map_json(relay::default_input_map()),
          "input maps round-trip through their file format");
    check(!relay::parse_input_map(R"({"format":"relay.input","version":1,"actions":[{"name":"a","bindings":[]},{"name":"a","bindings":[]}],"axes":[]})", error),
          "duplicate input names are rejected");
    check(!relay::parse_input_map(R"({"format":"relay.input","version":1,"actions":[{"name":"a","bindings":["keyboard:w"]}],"axes":[]})", error),
          "unknown devices are rejected");
    check(!relay::parse_input_map(R"({"format":"relay.input","version":1,"actions":[],"axes":[{"name":"x","bindings":[{"analog":"key:w"}]}]})", error),
          "only sticks and triggers can be analog axis bindings");

    relay::Engine engine({64, 48, 1.0 / 60.0, 0x52454c4159ULL, true});
    relay::ControlProtocol protocol(engine);
    const auto defaults = request(protocol, "input.map");
    check(!*relay::field(*defaults.object(), "saved")->boolean(), "a session starts with the default map");
    auto map = relay::default_input_map();
    map.actions.push_back({"dash", {"key:k"}});
    const auto document = "\"map\":\"" + relay::json_escape(relay::input_map_json(map)) + '"';
    request(protocol, "input.set_map", document, false);
    (void)request(protocol, "project.create",
                  "\"filename\":\"projects/controls/project.relayproject\",\"name\":\"Controls\"");
    request(protocol, "input.set_map", document);
    check(std::filesystem::exists("projects/controls/input.relay-input.json"),
          "the input map is saved beside the project file");
    request(protocol, "input.set_map", "\"map\":\"{}\"", false);
    request(protocol, "project.create",
            "\"filename\":\"projects/other/project.relayproject\",\"name\":\"Other\"");
    check(!engine.input().action("dash", Query::held) &&
              engine.input().map().actions.size() == relay::default_input_map().actions.size(),
          "another project uses its own map");
    request(protocol, "project.open", "\"filename\":\"projects/controls/project.relayproject\"");
    check(engine.input().map().actions.size() == map.actions.size(),
          "reopening a project loads its saved map");
    request(protocol, "input.simulate", "\"name\":\"dash\"", false);
    check(engine.run_game(), "input test game starts");
    request(protocol, "input.simulate", "\"name\":\"dash\",\"frames\":2");
    request(protocol, "input.simulate", "\"name\":\"move_x\",\"value\":-0.5,\"frames\":1");
    request(protocol, "input.simulate", "\"name\":\"missing\"", false);
    engine.step(1);
    check(engine.input().action("dash", Query::pressed) && engine.input().axis("move_x") == -0.5,
          "simulated input reaches the first step");
    engine.step(1);
    check(engine.input().action("dash", Query::held) && !engine.input().action("dash", Query::pressed) &&
              engine.input().axis("move_x") == 0.0,
          "simulated input lasts the requested steps");
    engine.step(1);
    check(engine.input().action("dash", Query::released) && !engine.input().action("dash", Query::held),
          "a simulated action is released afterwards");
    const auto state = request(protocol, "input.state");
    check(relay::field(*state.object(), "axes")->array()->size() == map.axes.size(),
          "input.state reports every axis");
    check(engine.stop_game(), "input test game stops");
}

// The demo's First Person Controller is a custom template driven by a project script, not a
// native node type: it sits under Rigid Body in the type tree and is refused in untrusted projects.
void demo_first_person_template() {
    check(!relay::find_node_type("FirstPersonController"), "there is no native first person node type");
    relay::Engine engine({64, 48, 1.0 / 60.0, 0x52454c4159ULL, true});
    relay::ControlProtocol protocol(engine);
    request(protocol, "project.open", "\"filename\":\"examples/demo/demo.relayproject\"");
    const auto listed = request(protocol, "templates.list");
    const relay::JsonValue::Object* controller = nullptr;
    for (const auto& value : *relay::field(*listed.object(), "templates")->array())
        if (*relay::field(*value.object(), "name")->string() == "First Person Controller")
            controller = value.object();
    check(controller && *relay::field(*controller, "type")->string() == "RigidBody",
          "the demo lists its First Person Controller template under Rigid Body");
    const auto& behaviours = *relay::field(*controller, "behaviours")->array();
    const auto& components = *relay::field(*controller, "components")->array();
    check(behaviours.size() == 1U && *behaviours.front().string() == "FirstPersonController" &&
              components.size() == 3U && *components[0].string() == "transform" &&
              *components[1].string() == "collider" && *components[2].string() == "physics_body",
          "templates.list reports the template root's components and script");
    check(std::filesystem::is_regular_file("examples/demo/scripts/FirstPersonController.cpp"),
          "the controller script is editable project source");

    const auto player = created_entity(request(
        protocol, "templates.instantiate", "\"template\":\"project:First Person Controller\""));
    const auto* record = engine.scene().get(player);
    check(record->collider->type == relay::BoxCollider::Type::capsule &&
              record->physics_body->type == relay::PhysicsBody::Type::dynamic &&
              record->physics_body->lock_rotation && record->scripts.size() == 1U &&
              record->scripts.front().behaviour == "FirstPersonController" &&
              record->transform.position.y == 1.0 && record->name == "First Person Controller",
          "the template is an upright capsule body running the controller script");
    relay::Entity camera{};
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->parent == player) camera = entity;
    check(camera.valid() && engine.scene().get(camera)->name == "Camera" &&
              engine.scene().get(camera)->camera && !engine.scene().get(camera)->camera->active,
          "the template has a child camera, inactive until the script activates it");
    check(engine.input().map().lock_mouse, "the demo's input map locks the mouse for looking");
    const auto refused = protocol.handle(R"({"id":1,"method":"runtime.play"})");
    check(refused.find("\"ok\":false") != std::string::npos,
          "a scene using the controller script needs a trusted, built project");
}

// Scene v15 stored a native first person controller; loading turns it into the script component.
void first_person_migration() {
    relay::Scene scene;
    const auto player = scene.create("Player");
    std::string error;
    check(relay::save_scene_file_atomic(scene, "player.relay.json", error), "save player scene");
    std::ifstream input("player.relay.json");
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    const auto replace = [&](const std::string& from, const std::string& to) {
        const auto at = text.find(from);
        check(at != std::string::npos, "the v16 file has the expected shape");
        text.replace(at, from.size(), to);
    };
    replace("\"version\":16", "\"version\":15");
    replace("\"scripts\":[]}", "\"scripts\":[],\"first_person_controller\":{\"walk_speed\":5,"
                               "\"sprint_speed\":8,\"jump_speed\":4,\"mouse_sensitivity\":0.2,"
                               "\"stick_look_speed\":120,\"invert_y\":true,\"ground_distance\":1.1,"
                               "\"camera\":\"Eyes\"}}");
    std::ofstream("player.relay.json", std::ios::trunc) << text;
    const auto loaded = relay::load_scene_file("player.relay.json");
    check(loaded && loaded.migrated, "a v15 scene with a native controller loads");
    const auto& scripts = loaded.state->slots[player.index].record.scripts;
    check(scripts.size() == 1U && scripts.front().behaviour == "FirstPersonController" &&
              scripts.front().enabled && scripts.front().properties.size() == 8U,
          "the native controller becomes a FirstPersonController script component");
    const auto property = [&](const char* name) {
        for (const auto& item : scripts.front().properties)
            if (item.name == name) return item;
        throw std::runtime_error(std::string{"missing migrated property "} + name);
    };
    check(property("walk_speed").number == 5.0 && property("ground_distance").number == 1.1 &&
              property("invert_y").boolean && property("camera_name").text == "Eyes",
          "the script component keeps the controller's settings");
}

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
        components_and_templates();
        input_mapping();
        demo_first_person_template();
        first_person_migration();
        std::cout << "Background editor workflow tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
