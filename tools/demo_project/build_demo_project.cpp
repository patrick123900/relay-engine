// Authors the dev-build demo project through the trusted control protocol, using the same requests
// the editor and agents send. Run by tools/generate_demo_project.py from the repository root, after
// it has written models/primitives.glb into the project folder. The committed scripts
// (FirstPersonController.cpp, Projectile.cpp) are project source and are left as they are.

#include "relay/control/control_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/input.hpp"
#include "relay/core/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double pi = 3.14159265358979323846;

struct Builder {
    relay::Engine engine{relay::EngineConfig{1280, 720, 1.0 / 60.0, 0x52454C4159ULL, true}};
    relay::ControlProtocol protocol{engine};
    int next_id{1};
    std::map<std::string, std::string> meshes, materials;

    relay::JsonValue call(const std::string& method, const std::string& fields = {}) {
        const auto request = "{\"id\":" + std::to_string(next_id++) + ",\"method\":\"" + method +
                             '"' + (fields.empty() ? "" : "," + fields) + '}';
        const auto response = protocol.handle(request);
        relay::JsonParser parser(response);
        const auto parsed = parser.parse();
        const auto* object = parsed ? parsed->object() : nullptr;
        const auto* ok = object ? relay::field(*object, "ok") : nullptr;
        if (!ok || !ok->boolean() || !*ok->boolean()) {
            std::cerr << method << " failed: " << response << '\n';
            std::exit(1);
        }
        const auto* result = relay::field(*object, "result");
        return result ? *result : relay::JsonValue{};
    }

    static std::string number(const double value) {
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        return output.str();
    }
    static std::string text(const std::string& value) { return '"' + relay::json_escape(value) + '"'; }
    static std::string vector(const char* prefix, const std::array<double, 3>& value) {
        return std::string(",\"") + prefix + "x\":" + number(value[0]) + ",\"" + prefix +
               "y\":" + number(value[1]) + ",\"" + prefix + "z\":" + number(value[2]);
    }

    std::string create(const std::string& name, const std::string& parent = {}) {
        const auto result = call("scene.create", "\"name\":" + text(name) +
                                                     (parent.empty() ? "" : ",\"parent\":" + text(parent)));
        return *relay::field(*result.object(), "entity")->string();
    }
    void transform(const std::string& entity, std::array<double, 3> position,
                   std::array<double, 3> rotation = {}, std::array<double, 3> scale = {1, 1, 1}) {
        call("scene.set_transform", "\"entity\":" + text(entity) + vector("p", position) +
                                        vector("r", rotation) + vector("s", scale));
    }
    void render(const std::string& entity, const std::string& mesh, const std::string& material) {
        call("scene.set_renderer", "\"entity\":" + text(entity) + ",\"mesh\":" + text(meshes.at(mesh)) +
                                       ",\"material\":" + text(materials.at(material)));
    }
    void collider(const std::string& entity, const std::string& fields) {
        call("scene.set_collider", "\"entity\":" + text(entity) + ',' + fields);
    }
    void body(const std::string& entity, const double mass, const double restitution = 0.1,
              const double friction = 0.5) {
        call("scene.set_physics_body", "\"entity\":" + text(entity) +
                                           ",\"type\":\"dynamic\",\"mass\":" + number(mass) +
                                           ",\"restitution\":" + number(restitution) +
                                           ",\"friction\":" + number(friction));
    }
    // Euler degrees that turn the -Z forward axis from `from` towards `to`.
    static std::array<double, 3> look(std::array<double, 3> from, std::array<double, 3> to) {
        const double dx = to[0] - from[0], dy = to[1] - from[1], dz = to[2] - from[2];
        const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
        return {std::asin(dy / length) * 180.0 / pi, std::atan2(-dx, -dz) * 180.0 / pi, 0.0};
    }
    std::string mesh_entity(const std::string& name, const std::string& parent,
                            const std::string& mesh, const std::string& material,
                            std::array<double, 3> position, std::array<double, 3> rotation = {},
                            std::array<double, 3> scale = {1, 1, 1}) {
        const auto entity = create(name, parent);
        transform(entity, position, rotation, scale);
        render(entity, mesh, material);
        return entity;
    }
};

std::string string_of(const relay::JsonValue::Object& object, const char* key) {
    const auto* value = relay::field(object, key);
    return value && value->string() ? *value->string() : std::string{};
}
double number_of(const relay::JsonValue::Object& object, const char* key) {
    const auto* value = relay::field(object, key);
    return value && value->number() ? *value->number() : 0.0;
}

std::vector<std::string> strings(const relay::JsonValue& result, const char* key) {
    std::vector<std::string> values;
    if (const auto* list = relay::field(*result.object(), key); list && list->array())
        for (const auto& value : *list->array())
            if (value.string()) values.push_back(*value.string());
    return values;
}

} // namespace

int main(const int argument_count, char** arguments) {
    const std::filesystem::path root = argument_count > 1 ? arguments[1] : "examples/demo";
    const auto project_file = (root / "demo.relayproject").generic_string();
    // Start from a clean project, keeping the generated models.
    std::error_code ignored;
    std::filesystem::remove(project_file, ignored);
    std::filesystem::remove(root / ".relay-imports.json", ignored);
    // Older demos kept the input map beside the project file; it now lives inside it.
    std::filesystem::remove(root / "input.relay-input.json", ignored);
    std::filesystem::remove_all(root / "scenes", ignored);

    Builder demo;
    demo.call("project.create", "\"filename\":" + Builder::text(project_file) +
                                    ",\"name\":\"Relay Demo\"");
    const auto imported = demo.call("assets.import_model",
                                    "\"filename\":\"models/primitives.glb\",\"instantiate\":false");
    // Importers may reorder, drop or add materials, so assets are identified by their contents:
    // meshes by index count, materials by base colour.
    const auto ids = strings(imported, "meshes");
    const auto material_ids = strings(imported, "materials");
    const std::vector<std::pair<std::string, double>> mesh_indices{
        {"Cube", 36}, {"Sphere", 4800}, {"Capsule", 5040}, {"Cylinder", 720}, {"Plane", 6},
        {"Torus", 6912}};
    const std::vector<std::pair<std::string, std::array<double, 4>>> material_colors{
        {"Ground", {0.46, 0.48, 0.5, 1}}, {"Brick", {0.78, 0.2, 0.14, 1}},
        {"Ocean", {0.12, 0.38, 0.86, 1}}, {"Mint", {0.28, 0.82, 0.58, 1}},
        {"Gold", {1.0, 0.77, 0.34, 1}}, {"Chrome", {0.95, 0.95, 0.96, 1}},
        {"Copper", {0.95, 0.62, 0.52, 1}}, {"Rubber", {0.05, 0.05, 0.06, 1}},
        {"Glass", {0.62, 0.82, 1.0, 0.32}}, {"Glow", {1.0, 0.55, 0.18, 1}}};
    const auto registry = demo.call("render.assets");
    for (const auto& value : *relay::field(*registry.object(), "meshes")->array()) {
        const auto& mesh = *value.object();
        const auto name = string_of(mesh, "name");
        if (std::find(ids.begin(), ids.end(), name) == ids.end()) continue;
        for (const auto& [label, count] : mesh_indices)
            if (number_of(mesh, "indices") == count) demo.meshes[label] = name;
    }
    for (const auto& value : *relay::field(*registry.object(), "materials")->array()) {
        const auto& material = *value.object();
        const auto name = string_of(material, "name");
        const auto* color = relay::field(material, "color");
        if (std::find(material_ids.begin(), material_ids.end(), name) == material_ids.end() ||
            !color || !color->array() || color->array()->size() != 4) continue;
        for (const auto& [label, expected] : material_colors) {
            bool same = true;
            for (std::size_t channel = 0; channel < 4; ++channel)
                same &= std::abs(*(*color->array())[channel].number() - expected[channel]) < 1e-3;
            if (same) demo.materials[label] = name;
        }
    }
    if (demo.meshes.size() != mesh_indices.size() || demo.materials.size() != material_colors.size()) {
        std::cerr << "primitives.glb resolved " << demo.meshes.size() << " of " << mesh_indices.size()
                  << " meshes and " << demo.materials.size() << " of " << material_colors.size()
                  << " materials\n";
        return 1;
    }

    // Lighting: a shadowed sun, a warm point fill and a cool spot over the material row.
    const auto lighting = demo.create("Lighting");
    const auto sun = demo.create("Sun", lighting);
    demo.transform(sun, {8, 12, 6}, Builder::look({8, 12, 6}, {0, 0, 0}));
    demo.call("scene.set_light", "\"entity\":" + Builder::text(sun) +
                                     ",\"type\":\"directional\",\"red\":1,\"green\":0.95,\"blue\":0.88,"
                                     "\"intensity\":2.6");
    const auto fill = demo.create("Warm fill light", lighting);
    demo.transform(fill, {-4, 1.3408241271972656, 4.126101493835449});
    demo.call("scene.set_light", "\"entity\":" + Builder::text(fill) +
                                     ",\"type\":\"point\",\"red\":1,\"green\":0.62,\"blue\":0.32,"
                                     "\"intensity\":6,\"range\":10");
    const auto spot = demo.create("Stage spot light", lighting);
    demo.transform(spot, {0, 6.5, -2.5}, Builder::look({0, 6.5, -2.5}, {0, 0, -4.5}));
    demo.call("scene.set_light", "\"entity\":" + Builder::text(spot) +
                                     ",\"type\":\"spot\",\"red\":0.75,\"green\":0.86,\"blue\":1,"
                                     "\"intensity\":14,\"range\":16,\"inner_cone\":0.4,\"outer_cone\":0.62");

    // Environment: a triangle-mesh ground, a static ramp and a transparent glass block.
    const auto environment = demo.create("Environment");
    const auto ground = demo.mesh_entity("Ground", environment, "Plane", "Ground", {0, 0, 0}, {},
                                         {30, 1, 30});
    demo.collider(ground, "\"type\":\"mesh\"");
    const auto ramp = demo.mesh_entity("Ramp", environment, "Cube", "Mint", {4.5, 1.1, 3.0},
                                       {0, 0, 18}, {5, 0.25, 2});
    demo.collider(ramp, "\"type\":\"box\"");
    const auto glass = demo.mesh_entity("Glass block", environment, "Cube", "Glass",
                                        {-6.5, 0.75, -3.0}, {0, 20, 0}, {1.5, 1.5, 1.5});
    demo.collider(glass, "\"type\":\"box\"");

    // Physics playground: everything here falls, rolls or topples during Run Game.
    const auto playground = demo.create("Physics playground");
    const std::vector<std::string> crate_materials{"Brick", "Ocean", "Mint"};
    int crate = 0;
    for (int level = 0; level < 3; ++level)
        for (int column = 0; column < 3 - level; ++column) {
            const double x = -4.5 + (column - (2 - level) * 0.5) * 0.84;
            ++crate;
            const auto entity = demo.mesh_entity(
                "Crate " + std::to_string(crate), playground, "Cube",
                crate_materials[static_cast<std::size_t>(crate) % crate_materials.size()],
                {x, 0.41 + level * 0.81, 1.5}, {}, {0.8, 0.8, 0.8});
            demo.collider(entity, "\"type\":\"box\"");
            demo.body(entity, 1.0, 0.05, 0.6);
        }
    const auto wrecking = demo.mesh_entity("Wrecking ball", playground, "Sphere", "Chrome",
                                           {-4.3, 6.0, 1.6});
    demo.collider(wrecking, "\"type\":\"sphere\",\"radius\":0.5");
    demo.body(wrecking, 4.0, 0.3, 0.4);
    const std::vector<std::pair<std::string, std::string>> balls{
        {"Gold ball", "Gold"}, {"Bouncy ball", "Rubber"}, {"Brick ball", "Brick"}};
    for (std::size_t index = 0; index < balls.size(); ++index) {
        const double offset = 0.5 * static_cast<double>(index);
        const auto entity = demo.mesh_entity(balls[index].first, playground, "Sphere",
                                             balls[index].second,
                                             {6.4, 2.6 + offset, 2.5 + offset}, {},
                                             {0.6, 0.6, 0.6});
        demo.collider(entity, "\"type\":\"sphere\",\"radius\":0.5");
        demo.body(entity, 0.8, balls[index].second == "Rubber" ? 0.85 : 0.2, 0.4);
    }
    const auto capsule = demo.mesh_entity("Tumbling capsule", playground, "Capsule", "Copper",
                                          {1.5, 3.5, 3.8}, {0, 0, 60});
    demo.collider(capsule, "\"type\":\"capsule\",\"radius\":0.25,\"half_height\":0.5");
    demo.body(capsule, 1.2, 0.1, 0.5);
    const auto barrel = demo.mesh_entity("Barrel (convex hull)", playground, "Cylinder", "Ocean",
                                         {0.5, 5.0, 2.2}, {80, 0, 20}, {0.7, 0.9, 0.7});
    demo.collider(barrel, "\"type\":\"convex\"");
    demo.body(barrel, 2.0, 0.1, 0.5);

    // Material showcase: PBR spheres on pedestals under the spot light.
    const auto showcase = demo.create("Material showcase");
    const std::vector<std::string> finishes{"Gold", "Chrome", "Copper", "Rubber", "Glass"};
    for (std::size_t index = 0; index < finishes.size(); ++index) {
        const double x = -4.0 + 2.0 * static_cast<double>(index);
        const auto pedestal = demo.mesh_entity(finishes[index] + " pedestal", showcase, "Cylinder",
                                               "Ground", {x, 0.3, -4.5}, {}, {0.9, 0.6, 0.9});
        demo.collider(pedestal, "\"type\":\"convex\"");
        const auto sphere = demo.mesh_entity(finishes[index] + " sphere", showcase, "Sphere",
                                             finishes[index], {x, 1.05, -4.5}, {}, {0.9, 0.9, 0.9});
        demo.collider(sphere, "\"type\":\"sphere\",\"radius\":0.5");
    }

    // Animation: scene-owned transform keyframes loop in the editor and during Run Game.
    const auto animation = demo.create("Animation");
    const auto torus = demo.mesh_entity("Spinning torus", animation, "Torus", "Glow",
                                        {6.2, 1.5, -2.5});
    const std::vector<std::pair<double, std::array<double, 6>>> keys{
        {0.0, {1.5, 0, 0, 0, 0, 0}}, {2.0, {2.1, 90, 180, 0, 0, 0}}, {4.0, {1.5, 180, 360, 0, 0, 0}}};
    for (const auto& [time, value] : keys)
        demo.call("scene.keyframe.set", "\"entity\":" + Builder::text(torus) + ",\"time_seconds\":" +
                                            Builder::number(time) + ",\"px\":6.2,\"py\":" +
                                            Builder::number(value[0]) + ",\"pz\":-2.5,\"rx\":" +
                                            Builder::number(value[1]) + ",\"ry\":" +
                                            Builder::number(value[2]) + ",\"rz\":0");
    demo.call("scene.keyframes.playback", "\"entity\":" + Builder::text(torus) +
                                              ",\"playing\":true,\"loop\":true,\"duration_seconds\":4");

    // Joints playground, behind the material row: a swinging chain, a hinged door, a motorised
    // spinner and a ball on a spring.
    const auto joints = demo.create("Joints playground");
    const auto joint = [&](const std::string& entity, const std::string& fields) {
        demo.call("scene.set_joint", "\"entity\":" + Builder::text(entity) + ',' + fields);
    };
    const auto beam = demo.mesh_entity("Chain beam", joints, "Cube", "Mint", {-5, 4.2, -9}, {},
                                       {1.2, 0.2, 0.2});
    demo.collider(beam, "\"type\":\"box\"");
    // The chain starts level and swings down; each link hangs from the one before it.
    auto previous = beam;
    for (int link = 1; link <= 3; ++link) {
        const auto entity = demo.mesh_entity("Chain link " + std::to_string(link), joints, "Sphere",
                                             "Chrome", {-5 + 0.6 * link, 4.2, -9}, {},
                                             {0.4, 0.4, 0.4});
        demo.collider(entity, "\"type\":\"sphere\",\"radius\":0.5");
        demo.body(entity, 1.0, 0.1, 0.4);
        joint(entity, "\"type\":\"point\",\"connected\":" + Builder::text(previous) +
                          ",\"anchor_x\":-1.5");
        previous = entity;
    }
    const auto post = demo.mesh_entity("Door post", joints, "Cube", "Ground", {-0.6, 1.2, -9}, {},
                                       {0.2, 2.4, 0.2});
    demo.collider(post, "\"type\":\"box\"");
    const auto door = demo.mesh_entity("Door", joints, "Cube", "Copper", {0, 1.15, -9}, {},
                                       {1.0, 2.2, 0.1});
    demo.collider(door, "\"type\":\"box\"");
    demo.body(door, 5.0, 0.1, 0.5);
    joint(door, "\"type\":\"hinge\",\"connected\":" + Builder::text(post) +
                    ",\"anchor_x\":-0.5,\"limits\":true,\"limit_min\":-100,\"limit_max\":100");
    const auto spinner = demo.mesh_entity("Spinner", joints, "Cube", "Ocean", {4, 0.35, -9}, {},
                                          {2.4, 0.2, 0.3});
    demo.collider(spinner, "\"type\":\"box\"");
    demo.body(spinner, 20.0, 0.1, 0.5);
    joint(spinner, "\"type\":\"hinge\",\"motor\":true,\"motor_speed\":60,\"motor_force\":5000");
    const auto bungee = demo.mesh_entity("Bungee ball", joints, "Sphere", "Gold", {7.5, 2.5, -9}, {},
                                         {0.6, 0.6, 0.6});
    demo.collider(bungee, "\"type\":\"sphere\",\"radius\":0.5");
    demo.body(bungee, 1.0, 0.3, 0.4);
    joint(bungee, "\"type\":\"distance\",\"connected_anchor_x\":7.5,\"connected_anchor_y\":5,"
                  "\"connected_anchor_z\":-9,\"limits\":true,\"limit_min\":0,\"limit_max\":1.5,"
                  "\"spring_frequency\":1.2,\"spring_damping\":0.1");

    // Input: the engine's default map, locking the mouse so first-person look can turn freely.
    auto input = relay::default_input_map();
    input.lock_mouse = true;
    demo.call("input.set_map", "\"map\":" + Builder::text(relay::input_map_json(input)));

    // First Person Controller: an upright capsule body with a camera at eye height, moved by the
    // project's FirstPersonController script. It is saved as a template at the origin (its camera
    // inactive, so placing it never steals another scene's view), then placed in the showcase in
    // front of the playgrounds, looking down -Z, as the scene's only and active camera.
    const auto player = demo.create("First Person Controller");
    const auto player_field = "\"entity\":" + Builder::text(player);
    demo.transform(player, {0, 1, 0});
    // Its mask leaves out layer 2, the Ball template's, so the player's own shots pass through it.
    demo.collider(player, "\"type\":\"capsule\",\"radius\":0.35,\"half_height\":0.55,"
                          "\"mask\":4294967293");
    // Walking sets velocity, so friction and damping would only catch on walls.
    demo.call("scene.set_physics_body", player_field +
                                            ",\"type\":\"dynamic\",\"mass\":70,\"friction\":0,"
                                            "\"linear_damping\":0,\"angular_damping\":0,"
                                            "\"lock_rotation\":true");
    demo.call("component.add", player_field + ",\"component\":\"script\","
                                              "\"behaviour\":\"FirstPersonController\"");
    const auto player_camera = demo.create("Camera", player);
    demo.transform(player_camera, {0, 0.7, 0});
    demo.call("scene.set_camera", "\"entity\":" + Builder::text(player_camera) +
                                      ",\"enabled\":true,\"active\":false,"
                                      "\"field_of_view_y_degrees\":75,\"near_plane\":0.05");
    demo.call("templates.save", player_field + ",\"name\":\"First Person Controller\",\"replace\":true");
    demo.transform(player, {0, 1, 8});
    demo.call("scene.set_camera", "\"entity\":" + Builder::text(player_camera) + ",\"active\":true");

    demo.call("scene.save", "\"filename\":\"showcase.relay.json\"");
    demo.call("project.add_scene", "\"scene_file\":\"showcase.relay.json\"");
    demo.call("project.set_startup", "\"scene_file\":\"showcase.relay.json\"");

    // Ball template: what the First Person Controller shoots, built after the showcase is saved and
    // kept only as a template. A bouncy gold sphere on collider layer 2 that removes itself after a
    // few seconds (scripts/Projectile.cpp).
    const auto ball = demo.mesh_entity("Ball", {}, "Sphere", "Gold", {0, 1, 0}, {}, {0.3, 0.3, 0.3});
    const auto ball_field = "\"entity\":" + Builder::text(ball);
    demo.collider(ball, "\"type\":\"sphere\",\"radius\":0.5,\"layer\":2");
    demo.body(ball, 0.5, 0.5, 0.4);
    demo.call("component.add", ball_field + ",\"component\":\"script\",\"behaviour\":\"Projectile\"");
    demo.call("templates.save", ball_field + ",\"name\":\"Ball\",\"replace\":true");
    demo.call("scene.destroy", ball_field);

    std::cout << "Wrote " << project_file
              << " with scenes/showcase.relay.json (with a First Person Controller) and the "
                 "First Person Controller and Ball templates\n";
    return 0;
}
