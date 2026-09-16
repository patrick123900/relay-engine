#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/shader_reflection.hpp"
#include "relay/scene/scene.hpp"
#include "relay/scene/scene_history.hpp"
#include "relay/scene/scene_io.hpp"

#include <algorithm>
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

bool importer_available() {
    return relay::model_import_capabilities_json().find(R"("available":true)") != std::string::npos;
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// A minimal but valid glTF triangle whose vertex data lives in a separate file, so the importer
// has to follow a dependency to succeed.
std::string external_buffer_gltf(const std::string& buffer_uri) {
    return R"({
  "asset": {"version": "2.0", "generator": "Relay sandbox test"},
  "buffers": [{"byteLength": 66, "uri": ")" + buffer_uri + R"("}],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36, "target": 34962},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24, "target": 34962},
    {"buffer": 0, "byteOffset": 60, "byteLength": 6, "target": 34963}
  ],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [-0.5, -0.5, 0], "max": [0.5, 0.5, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
    {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}
  ],
  "materials": [{"name": "Relay White", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}],
  "meshes": [{"name": "RelayTriangle", "primitives": [{
    "attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0
  }]}],
  "nodes": [{"name": "RelayTriangle", "mesh": 0}],
  "scenes": [{"nodes": [0]}],
  "scene": 0
})";
}

std::string triangle_buffer_bytes(const float first_x) {
    const std::array<float, 9> positions{first_x, -0.5F, 0.0F, 0.5F, -0.5F, 0.0F, 0.0F, 0.5F, 0.0F};
    const std::array<float, 6> uvs{0.0F, 1.0F, 1.0F, 1.0F, 0.5F, 0.0F};
    const std::array<std::uint16_t, 3> indices{0U, 1U, 2U};
    std::string bytes;
    bytes.append(reinterpret_cast<const char*>(positions.data()), sizeof(positions));
    bytes.append(reinterpret_cast<const char*>(uvs.data()), sizeof(uvs));
    bytes.append(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
    return bytes;
}

std::string one_pixel_png() {
    static constexpr std::array<unsigned char, 70> bytes{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xfc, 0xcf, 0xc0, 0x50,
        0x0f, 0x00, 0x04, 0x85, 0x01, 0x80, 0x84, 0xa9, 0x8c, 0x21, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string textured_external_gltf(const std::string& buffer_uri) {
    auto document = external_buffer_gltf(buffer_uri);
    const std::string original =
        R"("materials": [{"name": "Relay White", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}}])";
    const std::string replacement = R"("images": [{"uri": "pixel.png"}],
  "textures": [{"source": 0}],
  "materials": [{
    "name": "Relay PBR", "doubleSided": true, "alphaMode": "MASK", "alphaCutoff": 0.25,
    "emissiveFactor": [0.1, 0.2, 0.3],
    "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.7, 0.6, 1], "baseColorTexture": {"index": 0}, "metallicFactor": 0.7, "roughnessFactor": 0.3, "metallicRoughnessTexture": {"index": 0}},
    "normalTexture": {"index": 0, "scale": 0.75},
    "occlusionTexture": {"index": 0, "strength": 0.6},
    "emissiveTexture": {"index": 0}
  }])";
    const auto position = document.find(original);
    if (position != std::string::npos) document.replace(position, original.size(), replacement);
    return document;
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
    // The camera looks down -Z from z = 5, so the child must sit in front of it to survive culling.
    rendered_transform.position.z = -4.0;
    expect(render_source.set_transform(rendered_entity, rendered_transform),
           "rendered entity receives a local transform");
    expect(render_source.set_mesh_renderer(
               rendered_entity, relay::MeshRenderer{"builtin.quad", "builtin.violet"}),
           "scene accepts a registered mesh and material reference");
    relay::AssetRegistry registry;
    const auto render_snapshot = relay::build_render_scene(render_source, registry, 16.0F / 9.0F);
    expect(!render_snapshot.camera.using_default && render_snapshot.camera.entity == camera_entity,
           "render snapshot selects the scene's active camera");
    expect(render_snapshot.instances.size() == 1U &&
               render_snapshot.instances.front().entity == rendered_entity,
           "camera entities are excluded while drawable scene entities become instances");
    if (!render_snapshot.instances.empty()) {
        expect(std::abs(render_snapshot.instances.front().model.values[12] - 3.0F) < 0.0001F &&
                   std::abs(render_snapshot.instances.front().model.values[14] - 1.0F) < 0.0001F,
               "render snapshot resolves parent and local transforms into a world matrix");
        expect(render_snapshot.instances.front().mesh == "builtin.quad" &&
                   render_snapshot.instances.front().material == "builtin.violet",
               "render snapshot carries mesh and material asset identifiers");
    }
    expect(render_snapshot.culled == 0U, "a visible entity is not culled");

    // Frustum culling: an entity behind the camera must be rejected before it reaches a draw call.
    auto behind_transform = render_source.get(rendered_entity)->transform;
    behind_transform.position.z = 4.0;
    expect(render_source.set_transform(rendered_entity, behind_transform),
           "rendered entity can be moved behind the camera");
    const auto culled_snapshot = relay::build_render_scene(render_source, registry, 16.0F / 9.0F);
    expect(culled_snapshot.instances.empty() && culled_snapshot.culled == 1U,
           "an entity behind the camera is culled from the render scene");

    // Far outside the horizontal field of view is culled too.
    behind_transform.position = {400.0, 0.0, -4.0};
    expect(render_source.set_transform(rendered_entity, behind_transform),
           "rendered entity can be moved outside the horizontal field of view");
    expect(relay::build_render_scene(render_source, registry, 16.0F / 9.0F).culled == 1U,
           "an entity beyond the side planes is culled from the render scene");
    expect(render_source.set_transform(rendered_entity, rendered_transform),
           "rendered entity returns to a visible position");

    // Draw order must be front to back and independent of the order entities were created in.
    {
        relay::Scene order_scene;
        const auto order_camera = order_scene.create("Camera");
        auto order_camera_transform = order_scene.get(order_camera)->transform;
        order_camera_transform.position = {0.0, 0.0, 5.0};
        (void)order_scene.set_transform(order_camera, order_camera_transform);
        (void)order_scene.set_camera(order_camera, relay::Camera{});

        // Created far first, near second.
        const auto far_entity = order_scene.create("Far");
        auto far_transform = order_scene.get(far_entity)->transform;
        far_transform.position = {0.0, 0.0, -5.0};
        (void)order_scene.set_transform(far_entity, far_transform);
        (void)order_scene.set_mesh_renderer(far_entity,
                                            relay::MeshRenderer{"builtin.quad", "builtin.orange"});
        const auto near_entity = order_scene.create("Near");
        auto near_transform = order_scene.get(near_entity)->transform;
        near_transform.position = {0.0, 0.0, 1.0};
        (void)order_scene.set_transform(near_entity, near_transform);
        (void)order_scene.set_mesh_renderer(near_entity,
                                            relay::MeshRenderer{"builtin.quad", "builtin.azure"});

        const auto ordered = relay::build_render_scene(order_scene, registry, 1.0F);
        expect(ordered.instances.size() == 2U &&
                   ordered.instances.front().entity == near_entity &&
                   ordered.instances.back().entity == far_entity,
               "opaque instances draw front to back regardless of creation order");
        expect(ordered.instances.front().view_depth < ordered.instances.back().view_depth,
               "render instances carry an increasing camera-relative depth");
    }

    const auto render_graph = relay::make_scene_render_graph();
    expect(render_graph.valid && render_graph.ordered_passes.size() == 2U &&
               render_graph.ordered_passes.front().name == "scene_geometry" &&
               render_graph.transitions.size() == 4U,
           "render graph compiles geometry and presentation with explicit transitions: " +
               render_graph.error);
    const auto depth_resource = std::find_if(
        render_graph.resources.begin(), render_graph.resources.end(),
        [](const auto& resource) { return resource.name == "scene_depth"; });
    expect(depth_resource != render_graph.resources.end() && !depth_resource->imported,
           "render graph declares the depth attachment as a pass-local resource");
    expect(std::any_of(render_graph.transitions.begin(), render_graph.transitions.end(),
                       [](const auto& transition) {
                           return transition.after == "depth_stencil_attachment" &&
                                  transition.pass == "scene_geometry";
                       }),
           "render graph records the depth attachment transition in the geometry pass");
    relay::RenderGraph invalid_graph;
    const auto orphan = invalid_graph.add_resource("orphan", relay::RenderResourceKind::image);
    invalid_graph.add_pass("reader", {{orphan, relay::RenderAccess::sampled}});
    expect(!invalid_graph.compile().valid,
           "render graph rejects reads of non-imported resources without a producer");
    expect(registry.find_mesh("builtin.quad") != nullptr &&
               registry.find_material("builtin.azure") != nullptr,
           "built-in render assets are registered for deterministic lookup");
    expect(registry.textures().size() == 2U &&
               registry.textures().front().rgba.size() == 64U * 64U * 4U &&
               registry.texture_index("builtin.gradient") == 1U,
           "texture assets expose stable bindless slots and complete RGBA payloads");
    {
        relay::AssetRegistry isolated;
        expect(isolated.revision() == registry.revision() &&
                   isolated.meshes().size() == registry.meshes().size(),
               "independent registries start from identical built-in state");
    }
    expect(relay::model_import_capabilities_json().find(R"(".gltf")") != std::string::npos,
           "model importer advertises Godot's recommended glTF interchange format");
#ifdef RELAY_TEST_SOURCE_DIR
    relay::Scene imported_scene;
    std::string import_error;
    const std::filesystem::path assets_root = std::filesystem::path{RELAY_TEST_SOURCE_DIR} / "assets";
    const auto asset_revision = registry.revision();
    const auto imported = relay::import_model_asset(assets_root, "relay-test-triangle.gltf",
                                                    registry, &imported_scene, import_error);
    expect(imported.imported && imported.meshes.size() == 1U && !imported.roots.empty() &&
               registry.find_mesh(imported.meshes.front()) != nullptr &&
               registry.revision() == asset_revision + 1U,
           "model import registers content-addressed geometry and instantiates its hierarchy: " +
               import_error);
    expect(relay::build_render_scene(imported_scene, registry, 1.0F).instances.size() == 1U,
           "imported hierarchy nodes remain organizational and only mesh nodes are drawable");
    const auto imported_again = relay::import_model_asset(assets_root, "relay-test-triangle.gltf",
                                                          registry, nullptr, import_error);
    expect(imported_again.imported && imported_again.content_id == imported.content_id &&
               registry.revision() == asset_revision + 1U,
           "reimporting identical model content reuses stable asset identities");

    relay::Scene camera_scene;
    const auto existing_camera = camera_scene.create("Existing camera");
    expect(camera_scene.set_camera(existing_camera, relay::Camera{}), "create existing camera");
    const auto camera_import = relay::import_model_asset(assets_root, "relay-camera-golden.gltf",
                                                         registry, &camera_scene, import_error);
    relay::Entity imported_camera_entity{};
    std::size_t camera_count = 0;
    for (const auto entity : camera_scene.entities()) {
        const auto* record = camera_scene.get(entity);
        if (record->camera && entity != existing_camera) {
            imported_camera_entity = entity;
            ++camera_count;
        }
    }
    const auto* camera_record = camera_scene.get(imported_camera_entity);
    expect(camera_import.imported && camera_count == 1U && camera_record &&
               !camera_record->camera->active &&
               std::abs(camera_record->camera->field_of_view_y_degrees - 60.0) < 0.001 &&
               std::abs(camera_record->camera->near_plane - 0.25) < 0.001 &&
               std::abs(camera_record->camera->far_plane - 200.0) < 0.001 &&
               camera_scene.active_camera() == existing_camera,
           "perspective cameras preserve projection without stealing the active camera: " +
               import_error + camera_scene.serialize_json());
    expect(camera_import.json().find("orthographic camera") != std::string::npos,
           "unsupported orthographic cameras produce an explicit diagnostic");
    if (camera_record) {
        const auto* node = camera_scene.get(camera_record->parent);
        expect(node && node->name == "Perspective" && node->transform.position.z == 5.0 &&
                   std::abs(camera_record->transform.rotation_degrees.y) < 0.001 &&
                   camera_record->transform.position == relay::Vec3{},
               "camera-local orientation and node hierarchy are preserved");
    }
    relay::Scene static_camera_scene;
    relay::ModelImportSettings static_camera_settings;
    static_camera_settings.preset = "static_mesh";
    const auto static_camera_import = relay::import_model_asset(
        assets_root, "relay-camera-golden.gltf", registry, &static_camera_scene,
        import_error, static_camera_settings);
    bool static_has_camera = false;
    for (const auto entity : static_camera_scene.entities()) {
        static_has_camera = static_has_camera || static_camera_scene.get(entity)->camera.has_value();
    }
    expect(static_camera_import.imported && !static_has_camera,
           "static_mesh preset excludes cameras for direct glTF imports too");
    const auto camera_round_trip = std::filesystem::temp_directory_path() /
                                   "relay-imported-camera.relay.json";
    expect(relay::save_scene_file_atomic(camera_scene, camera_round_trip, import_error),
           "imported camera scene saves: " + import_error);
    const auto camera_loaded = relay::load_scene_file(camera_round_trip);
    if (camera_loaded) {
        relay::Scene restored_camera_scene;
        restored_camera_scene.restore_state(*camera_loaded.state);
        expect(restored_camera_scene.serialize_json() == camera_scene.serialize_json(),
               "imported camera projection, hierarchy and activation survive restart");
    } else {
        expect(false, "imported camera scene loads: " + camera_loaded.error);
    }
    std::filesystem::remove(camera_round_trip);
    if (camera_record) {
        auto camera = *camera_record->camera;
        camera.active = true;
        expect(camera_scene.set_camera(imported_camera_entity, camera) &&
                   relay::build_render_scene(camera_scene, registry, 2.0F).camera.entity ==
                       imported_camera_entity,
               "imported cameras can be activated through the existing scene interface");
    }

    relay::AssetRegistry golden_gltf_registry;
    relay::AssetRegistry golden_glb_registry;
    std::string golden_error;
    const auto golden_gltf = relay::import_model_asset(assets_root, "relay-pbr-golden.gltf",
                                                       golden_gltf_registry, nullptr, golden_error);
    const auto golden_glb = relay::import_model_asset(assets_root, "relay-pbr-golden.glb",
                                                      golden_glb_registry, nullptr, golden_error);
    const auto* golden_material = golden_glb.materials.empty()
                                      ? nullptr
                                      : golden_glb_registry.find_material(golden_glb.materials.front());
    expect(golden_gltf.imported && golden_glb.imported &&
               golden_gltf.content_id == golden_glb.content_id &&
               golden_glb.meshes.size() == 2U && golden_glb.textures.size() == 2U &&
               golden_material != nullptr && !golden_material->normal_texture.empty(),
           "golden glTF and binary GLB import to the same complete PBR asset identity: " +
               golden_error);
    const auto& golden_textures = golden_glb_registry.textures();
    expect(golden_textures.size() == 4U &&
               golden_textures[2].mag_filter == relay::TextureFilter::linear &&
               golden_textures[2].min_filter == relay::TextureFilter::linear &&
               golden_textures[2].mip_filter == relay::TextureFilter::linear &&
               golden_textures[2].wrap_u == relay::TextureWrap::repeat &&
               golden_textures[2].wrap_v == relay::TextureWrap::repeat,
           "glTF sampler filters and wrap modes survive normalized import");

    if (importer_available()) {
        const auto sandbox_root = std::filesystem::temp_directory_path() / "relay-phase-a-assets";
        const auto outside_root = std::filesystem::temp_directory_path() / "relay-phase-a-outside";
        std::filesystem::remove_all(sandbox_root);
        std::filesystem::remove_all(outside_root);
        std::filesystem::create_directories(sandbox_root);
        std::filesystem::create_directories(outside_root);

        // A model whose vertex data lives in a sibling .bin must import through the sandbox.
        write_file(sandbox_root / "external.gltf", external_buffer_gltf("external.bin"));
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.5F));
        relay::AssetRegistry external_registry;
        std::string external_error;
        const auto external = relay::import_model_asset(sandbox_root, "external.gltf",
                                                        external_registry, nullptr, external_error);
        expect(external.imported && external.meshes.size() == 1U,
               "importer resolves an external glTF buffer inside the assets root: " + external_error);
        expect(std::find(external.dependencies.begin(), external.dependencies.end(),
                         std::string{"external.bin"}) != external.dependencies.end(),
               "import records the external buffer as a tracked dependency");

        write_file(sandbox_root / "textured.gltf", textured_external_gltf("external.bin"));
        write_file(sandbox_root / "pixel.png", one_pixel_png());
        relay::AssetRegistry textured_registry;
        std::string textured_error;
        const auto textured = relay::import_model_asset(sandbox_root, "textured.gltf",
                                                        textured_registry, nullptr, textured_error);
        const auto* pbr_material = textured.materials.empty()
                                       ? nullptr : textured_registry.find_material(textured.materials.front());
        const auto* pbr_mesh = textured.meshes.empty()
                                   ? nullptr : textured_registry.find_mesh(textured.meshes.front());
        expect(textured.imported && pbr_material != nullptr && pbr_mesh != nullptr &&
                   !pbr_material->texture.empty() &&
                   !pbr_material->metallic_roughness_texture.empty() &&
                   !pbr_material->normal_texture.empty() &&
                   !pbr_material->occlusion_texture.empty() &&
                   !pbr_material->emissive_texture.empty() &&
                   std::abs(pbr_material->metallic_factor - 0.7F) < 0.001F &&
                   std::abs(pbr_material->roughness_factor - 0.3F) < 0.001F &&
                   pbr_material->alpha_mode == relay::MaterialAsset::AlphaMode::mask &&
                   pbr_material->double_sided,
               "glTF import preserves PBR factors, flags and all five texture channels: " +
                   textured_error);
        expect(textured_registry.textures().size() == 4U &&
                   textured_registry.textures()[2].color_space == relay::TextureColorSpace::srgb &&
                   textured_registry.textures()[3].color_space == relay::TextureColorSpace::linear,
               "image textures decode to RGBA8 and retain sRGB versus linear semantics");
        if (pbr_mesh != nullptr) {
            const auto vertex = textured_registry.mesh_vertices()[
                static_cast<std::size_t>(pbr_mesh->vertex_offset)];
            expect(std::abs(vertex.nz) > 0.9F &&
                       std::abs(vertex.tx) + std::abs(vertex.ty) + std::abs(vertex.tz) > 0.9F,
                   "import generates missing normals and tangents for textured geometry");
        }

        // The same model pointed at a file outside the root must be refused.
        write_file(outside_root / "secret.bin", triangle_buffer_bytes(-0.5F));
        write_file(sandbox_root / "escape.gltf",
                   external_buffer_gltf("../relay-phase-a-outside/secret.bin"));
        relay::AssetRegistry escape_registry;
        std::string escape_error;
        const auto escaped = relay::import_model_asset(sandbox_root, "escape.gltf",
                                                       escape_registry, nullptr, escape_error);
        expect(!escaped.imported &&
                   escape_error.find("outside the project assets directory") != std::string::npos,
               "importer refuses a model dependency that escapes the assets root: " + escape_error);
        expect(escape_registry.revision() == relay::AssetRegistry{}.revision(),
               "a blocked import leaves the asset registry untouched");

        // An absolute path is refused for the same reason.
        write_file(sandbox_root / "absolute.gltf",
                   external_buffer_gltf((outside_root / "secret.bin").generic_string()));
        relay::AssetRegistry absolute_registry;
        std::string absolute_error;
        const auto absolute = relay::import_model_asset(sandbox_root, "absolute.gltf",
                                                        absolute_registry, nullptr, absolute_error);
        expect(!absolute.imported, "importer refuses an absolute dependency path: " + absolute_error);

        // A dependency that is simply gone must fail cleanly rather than import partial geometry.
        write_file(sandbox_root / "missing.gltf", external_buffer_gltf("absent.bin"));
        relay::AssetRegistry missing_registry;
        std::string missing_error;
        const auto missing = relay::import_model_asset(sandbox_root, "missing.gltf",
                                                       missing_registry, nullptr, missing_error);
        expect(!missing.imported && !missing_error.empty(),
               "importer reports a missing dependency instead of importing partial geometry");

        // Changing the referenced buffer must change the content identity.
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.25F));
        relay::AssetRegistry changed_registry;
        std::string changed_error;
        const auto changed = relay::import_model_asset(sandbox_root, "external.gltf",
                                                       changed_registry, nullptr, changed_error);
        expect(changed.imported && changed.content_id != external.content_id,
               "changed dependency content produces a different asset identity");
        expect(external.content_id.rfind("sha256-v1-", 0U) == 0U,
               "asset identities carry a versioned SHA-256 scheme");

#ifndef _WIN32
        // Exercise the Blender process boundary without depending on the host Blender version. The
        // stand-in accepts the same fixed argv shape and emits a known-good GLB into Relay's cache.
        const auto fake_blender = sandbox_root / "fake-blender";
        write_file(fake_blender,
                   "#!/bin/sh\n"
                   "if [ \"$1\" = \"--version\" ]; then echo 'Blender 4.3.0 test'; exit 0; fi\n"
                   "for value in \"$@\"; do output=\"$value\"; done\n"
                   "directory=$(dirname \"$0\")\n"
                   "dependency=\"$directory/external.bin\"\n"
                   "if [ -f \"$directory/escape-dependency\" ]; then "
                   "dependency=\"$directory/../relay-phase-a-outside/secret.bin\"; fi\n"
                   "if [ \"$output\" = \"relay-dependencies\" ]; then "
                   "echo \"RELAY_DEPENDENCIES:[\\\"$dependency\\\"]\"; "
                   "exit 0; fi\n"
                   "if [ -f \"$directory/slow-conversion\" ]; then sleep 5; fi\n"
                   "cp \"$(dirname \"$0\")/fixture.glb\" \"$output\"\n");
        std::filesystem::permissions(
            fake_blender,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace);
        std::filesystem::copy_file(assets_root / "relay-pbr-golden.glb",
                                   sandbox_root / "fixture.glb");
        write_file(sandbox_root / "source.blend", "Relay Blender adapter fixture");
        relay::ModelImportSettings blender_settings;
        blender_settings.blender.executable = fake_blender;
        blender_settings.blender.timeout_seconds = 5U;
        blender_settings.blender.allow_unsandboxed = true; // Only this known local stand-in.
        relay::AssetRegistry blend_registry;
        std::string blend_error;
        const auto blend = relay::import_model_asset(sandbox_root, "source.blend", blend_registry,
                                                      nullptr, blend_error, blender_settings);
        expect(blend.imported && blend.source_adapter == "blender-glb" &&
                   !blend.conversion_cache_hit && blend.source_format == ".blend" &&
                   std::find(blend.dependencies.begin(), blend.dependencies.end(),
                             std::string{"source.blend"}) != blend.dependencies.end(),
               "Blender sources convert through the fixed, contained GLB adapter: " + blend_error);
        relay::AssetRegistry cached_blend_registry;
        const auto cached_blend = relay::import_model_asset(
            sandbox_root, "source.blend", cached_blend_registry, nullptr, blend_error,
            blender_settings);
        expect(cached_blend.imported && cached_blend.conversion_cache_hit &&
                   cached_blend.content_id == blend.content_id,
               "unchanged Blender content reuses its versioned conversion cache: " + blend_error);
        relay::ModelImportSettings static_blender_settings = blender_settings;
        static_blender_settings.preset = "static_mesh";
        relay::AssetRegistry static_blend_registry;
        const auto static_blend = relay::import_model_asset(
            sandbox_root, "source.blend", static_blend_registry, nullptr, blend_error,
            static_blender_settings);
        expect(static_blend.imported && !static_blend.conversion_cache_hit &&
                   static_blend.preset == "static_mesh" &&
                   static_blend.dependencies.back() != blend.dependencies.back(),
               "Blender import presets receive distinct content-addressed conversion caches: " +
                   blend_error);
        write_file(sandbox_root / cached_blend.dependencies.back(), "glTFbroken-cache-payload");
        relay::AssetRegistry repaired_blend_registry;
        const auto repaired_blend = relay::import_model_asset(
            sandbox_root, "source.blend", repaired_blend_registry, nullptr, blend_error,
            blender_settings);
        expect(repaired_blend.imported && !repaired_blend.conversion_cache_hit &&
                   repaired_blend.content_id == blend.content_id,
               "invalid cached GLB data is discarded and regenerated: " + blend_error);
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.2F));
        relay::AssetRegistry changed_blend_registry;
        const auto changed_blend = relay::import_model_asset(
            sandbox_root, "source.blend", changed_blend_registry, nullptr, blend_error,
            blender_settings);
        expect(changed_blend.imported && !changed_blend.conversion_cache_hit &&
                   changed_blend.dependencies.back() != blend.dependencies.back(),
               "changed external Blender dependencies invalidate the conversion cache: " +
                   blend_error);
        write_file(sandbox_root / "escape-dependency", "escape");
        relay::AssetRegistry blocked_blend_registry;
        const auto blocked_blend = relay::import_model_asset(
            sandbox_root, "source.blend", blocked_blend_registry, nullptr, blend_error,
            blender_settings);
        expect(!blocked_blend.imported &&
                   blend_error.find("outside the assets directory") != std::string::npos &&
                   blocked_blend_registry.revision() == 1U,
               "Blender preflight refuses external dependencies outside the asset root");
        std::filesystem::remove(sandbox_root / "escape-dependency");
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.1F));
        write_file(sandbox_root / "slow-conversion", "slow");
        auto timeout_settings = blender_settings;
        timeout_settings.blender.timeout_seconds = 1U;
        const auto timed_out_blend = relay::import_model_asset(
            sandbox_root, "source.blend", blocked_blend_registry, nullptr, blend_error,
            timeout_settings);
        expect(!timed_out_blend.imported && blend_error.find("timed out") != std::string::npos,
               "Blender conversion timeouts terminate the subprocess and reject partial output");
        std::filesystem::remove(sandbox_root / "slow-conversion");

        const auto escaped_cache_root = sandbox_root / "escaped-cache";
        std::filesystem::create_directories(escaped_cache_root);
        std::filesystem::copy_file(fake_blender, escaped_cache_root / "fake-blender");
        write_file(escaped_cache_root / "source.blend", "cache escape fixture");
        write_file(escaped_cache_root / "external.bin", "dependency");
        std::filesystem::create_directory_symlink(outside_root,
                                                  escaped_cache_root / ".relay-cache");
        auto escaped_cache_settings = blender_settings.blender;
        escaped_cache_settings.executable = escaped_cache_root / "fake-blender";
        const auto escaped_cache = relay::convert_blend_to_glb(
            escaped_cache_root, escaped_cache_root / "source.blend", escaped_cache_settings,
            blend_error);
        expect(!escaped_cache.converted &&
                   blend_error.find("cache resolves outside") != std::string::npos &&
                   !std::filesystem::exists(outside_root / "blender"),
               "cache symlink escapes are rejected before creating directories outside assets");
#endif

        // A manifest must let a fresh registry rebuild the same asset ids after a restart.
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.5F));
        relay::ImportManifest manifest;
        std::string manifest_error;
        expect(manifest.load(sandbox_root, manifest_error),
               "a project with no manifest loads as empty: " + manifest_error);
        manifest.record({"external.gltf", relay::model_importer_version, external.content_id,
                         external.dependencies, external.meshes, external.materials,
                         external.textures});
        expect(manifest.save(sandbox_root, manifest_error),
               "import manifest is written to the project: " + manifest_error);

        relay::AssetRegistry restored_registry;
        const auto report = reload_imported_assets(sandbox_root, restored_registry);
        expect(report.restored == 1U && report.changed == 0U && report.failed == 0U,
               "a fresh registry restores imported assets from the manifest");
        expect(restored_registry.find_mesh(external.meshes.front()) != nullptr,
               "asset ids saved in a scene resolve again after a simulated restart");

        // A source file that changed since the manifest was written must be reported, not hidden.
        write_file(sandbox_root / "external.bin", triangle_buffer_bytes(-0.25F));
        relay::AssetRegistry stale_registry;
        auto stale_report = reload_imported_assets(sandbox_root, stale_registry);
        expect(stale_report.changed == 1U && stale_report.restored == 0U,
               "a changed dependency is reported rather than silently re-pointed");
        relay::Scene stale_scene;
        const auto stale_entity = stale_scene.create("Previously imported mesh");
        expect(stale_scene.set_mesh_renderer(
                   stale_entity, relay::MeshRenderer{external.meshes.front(),
                                                     external.materials.front()}),
               "test scene can reference the previous imported asset identity");
        expect(stale_report.rebind_scene(stale_scene) == 1U &&
                   stale_scene.get(stale_entity)->mesh_renderer->mesh == changed.meshes.front() &&
                   stale_registry.find_mesh(changed.meshes.front()) != nullptr,
               "changed imports automatically rebind saved scene renderers to rebuilt assets");
        relay::ImportReloadReport ambiguous_report;
        ambiguous_report.asset_remaps = {{changed.meshes.front(), "asset.new-a.mesh.0"},
                                         {changed.meshes.front(), "asset.new-b.mesh.0"}};
        expect(ambiguous_report.rebind_scene(stale_scene) == 0U &&
                   stale_scene.get(stale_entity)->mesh_renderer->mesh == changed.meshes.front() &&
                   !ambiguous_report.messages.empty(),
               "conflicting source mappings do not silently rebind a scene to the wrong mesh");

        std::filesystem::remove_all(sandbox_root);
        std::filesystem::remove_all(outside_root);
    }
#endif

#ifdef RELAY_TEST_VERTEX_SHADER_PATH
    const auto vertex_words = read_spirv(RELAY_TEST_VERTEX_SHADER_PATH);
    const auto fragment_words = read_spirv(RELAY_TEST_FRAGMENT_SHADER_PATH);
    const auto vertex_interface = relay::reflect_spirv(vertex_words);
    const auto fragment_interface = relay::reflect_spirv(fragment_words);
    expect(vertex_interface.valid && vertex_interface.stage == "vertex" &&
               vertex_interface.push_constant_bytes == 128U &&
               !vertex_interface.inputs.empty() && vertex_interface.inputs.front().location == 0U,
           "SPIR-V reflection discovers the vertex input and native push-constant layout");
    expect(fragment_interface.valid && fragment_interface.stage == "fragment" &&
               fragment_interface.push_constant_bytes == 128U &&
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
    expect(deterministic_capture.find(R"("path":"captures/capture.bmp")") != std::string::npos &&
               std::filesystem::exists("captures/capture.bmp") &&
               !std::filesystem::exists("capture.bmp"),
           "a bare capture filename is contained in the captures directory, not the working directory");

    // The MCP bridge sends an explicit prefix; it must resolve to the same place, not captures/captures.
    const auto prefixed_capture = live_protocol.handle(
        R"({"id":15,"method":"render.capture","path":"captures/prefixed.png","source":"deterministic"})");
    expect(prefixed_capture.find(R"("path":"captures/prefixed.png")") != std::string::npos &&
               std::filesystem::exists("captures/prefixed.png"),
           "an explicit captures/ prefix is accepted without being applied twice");

    const auto escaping_capture = live_protocol.handle(
        R"({"id":16,"method":"render.capture","path":"../escaped.png","source":"deterministic"})");
    expect(escaping_capture.find(R"("ok":false)") != std::string::npos &&
               !std::filesystem::exists("../escaped.png"),
           "a capture path with directory components is refused");

    const auto async_capture = live_protocol.handle(
        R"({"id":17,"method":"render.capture_async","path":"async.png"})");
    expect(async_capture.find(R"("path":"captures/async.png")") != std::string::npos,
           "asynchronous captures are contained in the captures directory too");

    std::filesystem::remove("capture.bmp");
    std::filesystem::remove("captures/capture.bmp");
    std::filesystem::remove("captures/prefixed.png");
    std::filesystem::remove("captures/async.png");

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
