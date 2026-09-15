#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/shader_reflection.hpp"
#include "relay/scene/scene.hpp"
#include "relay/scene/scene_history.hpp"
#include "relay/scene/scene_io.hpp"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint32_t> read_spirv(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return {};
    const auto bytes = input.tellg();
    if (bytes <= 0 || bytes % static_cast<std::streamoff>(sizeof(std::uint32_t)) != 0) return {};
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / sizeof(std::uint32_t));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(words.data()), bytes);
    return input ? words : std::vector<std::uint32_t>{};
}

} // namespace

int main() {
    relay::Scene scene;
    const auto root = scene.create("Root");
    const auto child = scene.create("Child", root);
    expect(scene.contains(root) && scene.contains(child), "scene creates stable entity handles");
    expect(scene.get(child)->parent == root, "scene records entity hierarchy");
    expect(!scene.set_parent(root, child), "scene rejects hierarchy cycles");
    const auto stale_child = child;
    expect(scene.destroy(child), "scene destroys existing entities");
    expect(!scene.contains(stale_child), "destroyed handles become stale");
    const auto replacement = scene.create("Replacement");
    expect(replacement.index == stale_child.index && replacement.generation != stale_child.generation,
           "reused slots receive a new generation");

    relay::SceneHistory history(scene);
    relay::Entity history_entity{};
    expect(history.execute("Create history entity", [&](relay::Scene& target) {
        history_entity = target.create("Undoable");
        return history_entity.valid();
    }), "scene history executes transactions");
    expect(scene.contains(history_entity), "transaction mutation is visible");
    expect(history.undo() && !scene.contains(history_entity), "undo restores the exact prior scene");
    expect(history.redo() && scene.contains(history_entity), "redo restores the transaction result");
    relay::Entity subtree_root{};
    relay::Entity subtree_child{};
    expect(history.execute("Create subtree", [&](relay::Scene& target) {
        subtree_root = target.create("Subtree root");
        subtree_child = target.create("Subtree child", subtree_root);
        return subtree_root.valid() && subtree_child.valid();
    }), "history records a hierarchy transaction");
    expect(history.execute("Destroy subtree", [&](relay::Scene& target) {
        return target.destroy(subtree_root);
    }), "history records recursive destruction");
    expect(!scene.contains(subtree_root) && !scene.contains(subtree_child),
           "destroying a parent destroys its descendants");
    expect(history.undo() && scene.contains(subtree_root) && scene.contains(subtree_child) &&
               scene.get(subtree_child)->parent == subtree_root,
           "undo restores a subtree with exact handles and hierarchy");
    expect(scene.serialize_json().find(R"("components")") != std::string::npos,
           "scene serialization includes reflection metadata");
    expect(scene.serialize_json().find("Undoable") != std::string::npos,
           "scene serialization includes entity data");

    relay::Scene render_source;
    const auto camera_entity = render_source.create("Camera rig");
    auto camera_transform = render_source.get(camera_entity)->transform;
    camera_transform.position = {1.0, 0.0, 5.0};
    expect(render_source.set_transform(camera_entity, camera_transform) &&
               render_source.set_camera(camera_entity, relay::Camera{}),
           "scene accepts a validated active camera component");
    const auto rendered_entity = render_source.create("Rendered child", camera_entity);
    auto rendered_transform = render_source.get(rendered_entity)->transform;
    rendered_transform.position.x = 2.0;
    expect(render_source.set_transform(rendered_entity, rendered_transform),
           "rendered entity receives a local transform");
    expect(render_source.set_mesh_renderer(
               rendered_entity, relay::MeshRenderer{"builtin.quad", "builtin.violet"}),
           "scene accepts a registered mesh and material reference");
    const auto render_snapshot = relay::build_render_scene(render_source, 16.0F / 9.0F);
    expect(!render_snapshot.camera.using_default && render_snapshot.camera.entity == camera_entity,
           "render snapshot selects the scene's active camera");
    expect(render_snapshot.instances.size() == 1U &&
               render_snapshot.instances.front().entity == rendered_entity,
           "camera entities are excluded while drawable scene entities become instances");
    if (!render_snapshot.instances.empty()) {
        expect(std::abs(render_snapshot.instances.front().model.values[12] - 3.0F) < 0.0001F &&
                   std::abs(render_snapshot.instances.front().model.values[14] - 5.0F) < 0.0001F,
               "render snapshot resolves parent and local transforms into a world matrix");
        expect(render_snapshot.instances.front().mesh == "builtin.quad" &&
                   render_snapshot.instances.front().material == "builtin.violet",
               "render snapshot carries mesh and material asset identifiers");
    }

    const auto render_graph = relay::make_scene_render_graph();
    expect(render_graph.valid && render_graph.ordered_passes.size() == 2U &&
               render_graph.ordered_passes.front().name == "scene_geometry" &&
               render_graph.transitions.size() == 3U,
           "render graph compiles geometry and presentation with explicit transitions");
    relay::RenderGraph invalid_graph;
    const auto orphan = invalid_graph.add_resource("orphan", relay::RenderResourceKind::image);
    invalid_graph.add_pass("reader", {{orphan, relay::RenderAccess::sampled}});
    expect(!invalid_graph.compile().valid,
           "render graph rejects reads of non-imported resources without a producer");
    expect(relay::find_mesh_asset("builtin.quad") != nullptr &&
               relay::find_material_asset("builtin.azure") != nullptr,
           "built-in render assets are registered for deterministic lookup");
    expect(relay::built_in_textures().size() == 2U &&
               relay::built_in_textures().front().rgba.size() == 64U * 64U * 4U &&
               relay::texture_asset_index("builtin.gradient") == 1U,
           "texture assets expose stable bindless slots and complete RGBA payloads");
    expect(relay::model_import_capabilities_json().find(R"(".gltf")") != std::string::npos,
           "model importer advertises Godot's recommended glTF interchange format");
#ifdef RELAY_TEST_SOURCE_DIR
    relay::Scene imported_scene;
    std::string import_error;
    const auto asset_revision = relay::render_asset_revision();
    const auto imported = relay::import_model_asset(
        std::filesystem::path{RELAY_TEST_SOURCE_DIR} / "assets/relay-test-triangle.gltf",
        &imported_scene, import_error);
    expect(imported.imported && imported.meshes.size() == 1U && !imported.roots.empty() &&
               relay::find_mesh_asset(imported.meshes.front()) != nullptr &&
               relay::render_asset_revision() == asset_revision + 1U,
           "model import registers content-addressed geometry and instantiates its hierarchy: " +
               import_error);
    expect(relay::build_render_scene(imported_scene, 1.0F).instances.size() == 1U,
           "imported hierarchy nodes remain organizational and only mesh nodes are drawable");
    const auto imported_again = relay::import_model_asset(
        std::filesystem::path{RELAY_TEST_SOURCE_DIR} / "assets/relay-test-triangle.gltf",
        nullptr, import_error);
    expect(imported_again.imported && imported_again.content_id == imported.content_id &&
               relay::render_asset_revision() == asset_revision + 1U,
           "reimporting identical model content reuses stable asset identities");
#endif

#ifdef RELAY_TEST_VERTEX_SHADER_PATH
    const auto vertex_words = read_spirv(RELAY_TEST_VERTEX_SHADER_PATH);
    const auto fragment_words = read_spirv(RELAY_TEST_FRAGMENT_SHADER_PATH);
    const auto vertex_interface = relay::reflect_spirv(vertex_words);
    const auto fragment_interface = relay::reflect_spirv(fragment_words);
    expect(vertex_interface.valid && vertex_interface.stage == "vertex" &&
               vertex_interface.push_constant_bytes == 84U &&
               !vertex_interface.inputs.empty() && vertex_interface.inputs.front().location == 0U,
           "SPIR-V reflection discovers the vertex input and native push-constant layout");
    expect(fragment_interface.valid && fragment_interface.stage == "fragment" &&
               fragment_interface.push_constant_bytes == 84U &&
               !fragment_interface.outputs.empty() && fragment_interface.outputs.front().location == 0U &&
               !fragment_interface.bindings.empty() && fragment_interface.bindings.front().set == 0U &&
               fragment_interface.bindings.front().binding == 0U,
           "SPIR-V reflection discovers the fragment output and texture-table binding");
#endif

    const auto scene_test_directory = std::filesystem::temp_directory_path() / "relay-engine-scene-tests";
    std::filesystem::create_directories(scene_test_directory);
    const auto round_trip_path = scene_test_directory / "round-trip.relay.json";
    std::string scene_file_error;
    expect(relay::save_scene_file_atomic(scene, round_trip_path, scene_file_error),
           "scene saves through an atomic replacement: " + scene_file_error);
    const auto loaded_scene = relay::load_scene_file(round_trip_path);
    expect(static_cast<bool>(loaded_scene), "saved scene validates and loads: " + loaded_scene.error);
    if (loaded_scene) {
        relay::Scene restored;
        restored.restore_state(*loaded_scene.state);
        expect(restored.serialize_json() == scene.serialize_json(),
               "scene round trip preserves exact handles, hierarchy and components");
    }
    bool found_temporary_file = false;
    for (const auto& entry : std::filesystem::directory_iterator(scene_test_directory)) {
        if (entry.path().filename().string().find("round-trip.relay.json.tmp.") == 0U) {
            found_temporary_file = true;
        }
    }
    expect(!found_temporary_file, "atomic save leaves no temporary file behind");

    const auto legacy_path = scene_test_directory / "legacy.relay.json";
    {
        std::ofstream legacy(legacy_path);
        legacy << R"({"format":"relay.scene","version":0,"entities":[{"entity":"4:7","name":"Legacy","parent":null,"transform":{"position":{"x":1,"y":2,"z":3},"rotation":{"x":10,"y":20,"z":30},"scale":{"x":1,"y":1,"z":1}}}]})";
    }
    const auto legacy_scene = relay::load_scene_file(legacy_path);
    expect(legacy_scene && legacy_scene.migrated && legacy_scene.source_version == 0U,
           "version 0 scene migrates to the current representation");
    if (legacy_scene) {
        relay::Scene migrated;
        migrated.restore_state(*legacy_scene.state);
        const auto* migrated_record = migrated.get({4U, 7U});
        expect(migrated_record != nullptr && migrated_record->transform.rotation_degrees.y == 20.0,
               "migration maps legacy rotation into rotation_degrees");
    }

    const auto version_one_path = scene_test_directory / "version-one.relay.json";
    {
        std::ofstream version_one(version_one_path);
        version_one << R"({"format":"relay.scene","version":1,"components":[],"scene":{"entities":[{"entity":"0:1","name":"Version one","parent":null,"transform":{"position":{"x":0,"y":0,"z":0},"rotation_degrees":{"x":0,"y":0,"z":0},"scale":{"x":1,"y":1,"z":1}}}],"allocator":{"slot_generations":[1],"free_indices":[]}}})";
    }
    const auto version_one_scene = relay::load_scene_file(version_one_path);
    expect(version_one_scene && version_one_scene.migrated &&
               version_one_scene.source_version == 1U &&
               !version_one_scene.state->slots[0].record.camera.has_value(),
           "version 1 scenes migrate with no camera component");

    const auto version_two_path = scene_test_directory / "version-two.relay.json";
    {
        std::ofstream version_two(version_two_path);
        version_two << R"({"format":"relay.scene","version":2,"components":[],"scene":{"entities":[{"entity":"0:1","name":"Version two camera","parent":null,"transform":{"position":{"x":0,"y":0,"z":5},"rotation_degrees":{"x":0,"y":0,"z":0},"scale":{"x":1,"y":1,"z":1}},"camera":{"field_of_view_y_degrees":60,"near_plane":0.1,"far_plane":1000,"active":true}}],"allocator":{"slot_generations":[1],"free_indices":[]}}})";
    }
    const auto version_two_scene = relay::load_scene_file(version_two_path);
    expect(version_two_scene && version_two_scene.migrated &&
               version_two_scene.state->slots[0].record.camera.has_value() &&
               !version_two_scene.state->slots[0].record.mesh_renderer.has_value(),
           "version 2 scenes preserve cameras and migrate with no mesh renderer component");

    const auto invalid_path = scene_test_directory / "invalid.relay.json";
    {
        std::ofstream invalid(invalid_path);
        invalid << R"({"format":"relay.scene","version":0,"entities":[{"entity":"0:1","name":"A","parent":"1:1","transform":{"position":{"x":0,"y":0,"z":0},"rotation":{"x":0,"y":0,"z":0},"scale":{"x":1,"y":1,"z":1}}}]})";
    }
    const auto invalid_scene = relay::load_scene_file(invalid_path);
    expect(!invalid_scene && invalid_scene.error.find("missing or stale parent") != std::string::npos,
           "loader rejects unresolved hierarchy references");

    relay::Engine engine({64, 48, 1.0 / 60.0});
    expect(engine.status().frame_index == 0, "engine starts at frame zero");
    engine.tick();
    expect(engine.status().frame_index == 1, "tick advances a running engine");
    engine.pause();
    engine.tick();
    expect(engine.status().frame_index == 1, "tick does not advance a paused engine");
    engine.step(4);
    expect(engine.status().frame_index == 5, "step advances an exact number of frames while paused");

    relay::ControlProtocol protocol(engine);
    expect(relay::protocol_schema_version == 4U && relay::protocol_methods().size() == 38U,
           "generated native protocol catalog contains every schema method");
    const auto status = protocol.handle(R"({"id":7,"method":"runtime.status"})");
    expect(status.find(R"("id":7)") != std::string::npos, "protocol preserves request id");
    expect(status.find(R"("frame":5)") != std::string::npos, "protocol reports frame state");
    const auto unknown = protocol.handle(R"({"id":8,"method":"does.not.exist"})");
    expect(unknown.find(R"("ok":false)") != std::string::npos, "protocol reports unknown methods");
    expect(protocol.handle(R"({"id":80,"method":"runtime.step","frames":0})").find(
               "outside its allowed range") != std::string::npos,
           "schema-generated validation rejects out-of-range native parameters");
    expect(protocol.handle(R"({"id":81,"method":"runtime.step","frames":"two"})").find(
               "must be integer") != std::string::npos,
           "schema-generated validation rejects incorrect native parameter types");
    expect(protocol.handle(R"({"id":82,"method":"runtime.status","surprise":true})").find(
               "unknown field") != std::string::npos,
           "schema-generated validation rejects method-specific unknown fields");
    expect(protocol.handle(R"({"id":83,"method":"scene.inspect"})").find(
               "missing required field") != std::string::npos,
           "schema-generated validation enforces required native parameters");
    expect(protocol.handle(R"({"id":84,"method":"runtime.status","id":85})").find(
               "duplicate field") != std::string::npos,
           "native request parser rejects duplicate JSON keys");
    const auto capabilities = protocol.handle(R"({"id":9,"method":"render.capabilities"})");
    expect(capabilities.find(R"("vulkan":)") != std::string::npos,
           "protocol exposes Vulkan capabilities even when Vulkan is unavailable");
    const auto create_entity = protocol.handle(
        R"({"id":10,"method":"scene.create","name":"Agent Camera"})");
    expect(create_entity.find(R"("entity":"0:1")") != std::string::npos,
           "protocol creates scene entities with readable stable handles");
    const auto set_transform = protocol.handle(
        R"({"id":11,"method":"scene.set_transform","entity":"0:1","px":4.5,"ry":90})");
    expect(set_transform.find(R"("x":4.5)") != std::string::npos,
           "protocol updates individual transform fields");
    const auto scene_undo = protocol.handle(R"({"id":12,"method":"scene.undo"})");
    expect(scene_undo.find(R"("ok":true)") != std::string::npos,
           "protocol exposes undoable scene operations");
    const auto set_camera = protocol.handle(
        R"({"id":120,"method":"scene.set_camera","entity":"0:1","field_of_view_y_degrees":72,"near_plane":0.25,"far_plane":500})");
    expect(set_camera.find(R"("field_of_view_y_degrees":72)") != std::string::npos &&
               engine.scene().active_camera() == relay::Entity{0U, 1U},
           "protocol configures the active scene camera through a generated tool contract");
    expect(protocol.handle(
               R"({"id":121,"method":"scene.set_camera","entity":"0:1","near_plane":10,"far_plane":2})")
               .find("greater than near_plane") != std::string::npos,
           "protocol rejects an invalid camera clipping range");
    expect(protocol.handle(R"({"id":122,"method":"scene.set_renderer","entity":"0:1","mesh":"builtin.quad","material":"builtin.azure"})")
                   .find(R"("mesh":"builtin.quad")") != std::string::npos,
           "protocol attaches registered mesh and material assets to an entity");
    expect(protocol.handle(R"({"id":123,"method":"render.graph"})").find("scene_geometry") !=
               std::string::npos &&
               protocol.handle(R"({"id":124,"method":"render.assets"})").find("builtin.violet") !=
                   std::string::npos,
           "protocol exposes render graph and asset inspection");
    expect(protocol.handle(R"({"id":126,"method":"assets.formats"})").find(R"(".glb")") !=
               std::string::npos &&
               protocol.handle(R"({"id":127,"method":"assets.import_model","filename":"../bad.obj"})")
                   .find(R"("ok":false)") != std::string::npos,
           "protocol exposes import capabilities and rejects model path traversal");
    expect(protocol.handle(R"({"id":125,"method":"render.shader_interfaces"})").find(
               R"("available":false)") != std::string::npos,
           "headless protocol explains when live shader reflection is unavailable");
    const auto unsafe_save = protocol.handle(
        R"({"id":15,"method":"scene.save","filename":"../escape.relay.json"})");
    expect(unsafe_save.find(R"("ok":false)") != std::string::npos,
           "protocol rejects scene path traversal");
    const auto scene_save = protocol.handle(
        R"({"id":16,"method":"scene.save","filename":"protocol-test.relay.json"})");
    expect(scene_save.find(R"("ok":true)") != std::string::npos,
           "protocol atomically saves into the project scenes directory");
    const auto extra_entity = protocol.handle(
        R"({"id":17,"method":"scene.create","name":"Only before load"})");
    expect(extra_entity.find(R"("ok":true)") != std::string::npos,
           "test scene diverges after save");
    const auto scene_load = protocol.handle(
        R"({"id":18,"method":"scene.load","filename":"protocol-test.relay.json"})");
    expect(scene_load.find(R"("ok":true)") != std::string::npos,
           "protocol validates and loads a scene file");
    expect(engine.scene().active_camera() == relay::Entity{0U, 1U},
           "scene round trip preserves the active camera component");
    expect(engine.scene().get({0U, 1U})->mesh_renderer.has_value() &&
               engine.scene().get({0U, 1U})->mesh_renderer->mesh == "builtin.quad",
           "scene round trip preserves mesh renderer components");
    expect(protocol.handle(R"({"id":19,"method":"scene.undo"})").find(R"("ok":true)") != std::string::npos &&
               engine.scene().entities().size() == 2U,
           "loading a scene is one undoable transaction");

    bool live_capture_called = false;
    relay::ControlProtocol live_protocol(engine, [&](const std::filesystem::path&, std::string&) {
        live_capture_called = true;
        return true;
    });
    const auto live_capture = live_protocol.handle(
        R"({"id":13,"method":"render.capture","path":"capture.bmp","source":"vulkan"})");
    expect(live_capture_called && live_capture.find(R"("source":"vulkan")") != std::string::npos,
           "protocol routes Vulkan captures to the active renderer");
    live_capture_called = false;
    const auto deterministic_capture = live_protocol.handle(
        R"({"id":14,"method":"render.capture","path":"capture.bmp","source":"deterministic"})");
    expect(!live_capture_called && deterministic_capture.find(R"("source":"deterministic")") != std::string::npos,
           "protocol retains deterministic capture while a live renderer is attached");
    std::filesystem::remove("capture.bmp");

    const auto capture_path = std::filesystem::temp_directory_path() / "relay-engine-test.bmp";
    std::string capture_error;
    expect(engine.capture(capture_path, capture_error), "engine captures a frame: " + capture_error);
    expect(std::filesystem::file_size(capture_path) == 54U + 64U * 48U * 4U,
           "captured BMP has the expected size");
    std::filesystem::remove(capture_path);

    const auto png_path = std::filesystem::temp_directory_path() / "relay-engine-test.png";
    expect(engine.capture(png_path, capture_error), "engine captures a PNG frame: " + capture_error);
    {
        std::ifstream png(png_path, std::ios::binary);
        unsigned char signature[8]{};
        png.read(reinterpret_cast<char*>(signature), 8);
        expect(signature[0] == 137U && signature[1] == 'P' && signature[2] == 'N' &&
                   signature[3] == 'G',
               "PNG capture has the standard signature");
    }
    std::filesystem::remove(png_path);

    const auto async_path = std::filesystem::temp_directory_path() / "relay-engine-async.png";
    const auto capture_job = engine.capture_async(async_path, capture_error);
    expect(capture_job != 0U, "asynchronous capture accepts a frame without blocking");
    relay::CaptureJobStatus async_status;
    for (int attempt = 0; attempt < 100; ++attempt) {
        async_status = engine.capture_status(capture_job);
        if (async_status.state == relay::CaptureJobState::complete ||
            async_status.state == relay::CaptureJobState::failed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(async_status.state == relay::CaptureJobState::complete,
           "background worker completes asynchronous PNG capture: " + async_status.error);
    std::filesystem::remove(async_path);

    engine.step(3);
    const auto performance = engine.performance();
    expect(!performance.samples.empty() && performance.average_cpu_ms >= 0.0 &&
               performance.resident_memory_bytes > 0U,
           "performance telemetry reports bounded frame samples and process memory");
    expect(protocol.handle(R"({"id":20,"method":"performance.read"})").find(
               R"("resident_memory_bytes":)") != std::string::npos,
           "protocol exposes structured performance telemetry");

    const auto trace_start = protocol.handle(
        R"({"id":21,"method":"trace.start","filename":"test.relay-trace.jsonl"})");
    expect(trace_start.find(R"("ok":true)") != std::string::npos, "protocol starts trace recording");
    const auto before_traced_step = engine.status().frame_index;
    (void)protocol.handle(R"({"id":22,"method":"runtime.step","frames":3})");
    engine.apply_input_event("key:down:32");
    expect(protocol.handle(R"({"id":23,"method":"trace.stop"})").find(R"("events":2)") !=
               std::string::npos,
           "trace records agent commands and normalized input events");
    const auto replay = protocol.handle(
        R"({"id":24,"method":"trace.replay","filename":"test.relay-trace.jsonl"})");
    expect(replay.find(R"("commands":1)") != std::string::npos &&
               replay.find(R"("inputs":1)") != std::string::npos &&
               engine.status().frame_index == before_traced_step + 6U,
           "trace replay reproduces command timing and injected inputs");

    const auto video_start = protocol.handle(
        R"({"id":25,"method":"video.start","filename":"test-observability.webm","fps":10,"maximum_frames":10})");
    expect(video_start.find(R"("ok":true)") != std::string::npos, "protocol starts WebM recording");
    engine.step(12);
    const auto video_stop = protocol.handle(R"({"id":26,"method":"video.stop"})");
    expect(video_stop.find(R"("ok":true)") != std::string::npos &&
               std::filesystem::file_size("captures/test-observability.webm") > 0U,
           "queued frames encode into a WebM recording");

    std::filesystem::remove("scenes/protocol-test.relay.json");
    std::filesystem::remove("traces/test.relay-trace.jsonl");
    std::filesystem::remove("captures/test-observability.webm");
    std::filesystem::remove(round_trip_path);
    std::filesystem::remove(legacy_path);
    std::filesystem::remove(version_one_path);
    std::filesystem::remove(version_two_path);
    std::filesystem::remove(invalid_path);
    std::filesystem::remove(scene_test_directory);

    expect(!engine.logs().read_after(0).empty(), "engine exposes structured logs");

    if (failures == 0) std::cout << "All Relay engine tests passed\n";
    return failures == 0 ? 0 : 1;
}
