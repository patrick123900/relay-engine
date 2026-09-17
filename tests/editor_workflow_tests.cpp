#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/editor/editor_selection.hpp"
#include "relay/editor/editor_timeline.hpp"
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
} // namespace

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    int result = 0;
    try {
        clipboard_and_groups();
        selections_and_timeline();
        projects();
        std::cout << "Background editor workflow tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    return result;
}
