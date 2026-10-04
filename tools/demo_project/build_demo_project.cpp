// Authors the dev-build demo project through the trusted control protocol, using the same requests
// the editor and agents send. Run by tools/generate_demo_project.py from the repository root, after
// it has written models/primitives.glb and the sounds into the project folder. The committed scripts
// (FirstPersonController.cpp, Projectile.cpp, ImpactSound.cpp, ToneButton.cpp, MusicSwitch.cpp,
// GameMenu.cpp) are project source and are left as they are.
//
// `relay_build_demo_project --add-interface [root]` instead adds (or replaces) only the showcase's
// game interface in an existing demo, keeping everything else in its scene, and
// `--add-particles [root]` its particle effects and the Ball template's trail.

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
#include <string_view>
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
    void sound(const std::string& entity, const std::string& fields) {
        call("scene.set_audio_source", "\"entity\":" + text(entity) + ',' + fields);
    }
    void particles(const std::string& entity, const std::string& values) {
        call("scene.set_particle_emitter", "\"entity\":" + text(entity) + ",\"values\":{" + values + '}');
    }
    void script(const std::string& entity, const std::string& behaviour) {
        call("component.add", "\"entity\":" + text(entity) +
                                  ",\"component\":\"script\",\"behaviour\":" + text(behaviour));
    }
    // Points a script's node or script reference property (a relay::Entity or relay::Ref<T>) at a
    // node; `script_index` is the script's place among the node's script components.
    void reference(const std::string& entity, const unsigned script_index, const std::string& property,
                   const std::string& target) {
        call("scene.set_script_property", "\"entity\":" + text(entity) + ",\"index\":" +
                                              std::to_string(script_index) + ",\"property\":" +
                                              text(property) + ",\"target\":" + text(target));
    }
    // A thump from where the body is whenever it hits something (scripts/ImpactSound.cpp). Heavier
    // things get a lower `pitch`.
    void impact(const std::string& entity, const double pitch, const double volume_db = 0.0) {
        sound(entity, "\"clip\":\"sounds/thump.wav\",\"bus\":\"SFX\",\"play_on_start\":false,"
                      "\"spatial\":true,\"min_distance\":1.5,\"max_distance\":30,\"pitch\":" +
                          number(pitch) + ",\"volume_db\":" + number(volume_db));
        script(entity, "ImpactSound");
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

// The showcase's game interface (scripts/GameMenu.cpp): a crosshair, control hints and a count of
// balls fired while playing, and a menu on Tab with a button, a switch and a slider. Interface
// nodes draw only during Run Game, over the game's view.
void add_interface(Builder& demo) {
    const auto node = [&](const std::string& name, const char* type, const std::string& parent) {
        const auto result = demo.call("scene.create", "\"name\":" + Builder::text(name) + ",\"type\":\"" + type + '"' +
                                                          (parent.empty() ? "" : ",\"parent\":" + Builder::text(parent)));
        return *relay::field(*result.object(), "entity")->string();
    };
    const auto ui = [&](const std::string& entity, const char* component, const std::string& values) {
        demo.call("scene.set_ui", "\"entity\":" + Builder::text(entity) + ",\"component\":\"" + component +
                                      "\",\"values\":{" + values + '}');
    };
    const std::string shadow = "\"shadow_color\":[0,0,0,0.65],\"shadow_offset\":[2,2]";
    const auto hud = node("HUD", "Canvas", {});
    const auto crosshair = node("Crosshair", "Panel", hud);
    ui(crosshair, "ui_control", "\"offset_min\":[-4,-4],\"offset_max\":[4,4],\"mouse_filter\":\"ignore\"");
    ui(crosshair, "ui_panel", "\"color\":[1,1,1,0.9],\"corner_radius\":4,\"border_width\":1,\"border_color\":[0,0,0,0.55]");
    const auto hints = node("Hints", "Label", hud);
    ui(hints, "ui_control", "\"anchor_min\":[0,0],\"anchor_max\":[0,0],\"offset_min\":[28,20],\"offset_max\":[1100,60]");
    ui(hints, "ui_label", "\"text\":\"WASD move \u00b7 Space jump \u00b7 Shift sprint \u00b7 Click shoot \u00b7 E ring the "
                          "bell \u00b7 Tab menu\",\"size\":22," + shadow);
    const auto shots = node("Shots", "Label", hud);
    ui(shots, "ui_control", "\"anchor_min\":[1,0],\"anchor_max\":[1,0],\"offset_min\":[-328,16],\"offset_max\":[-28,64]");
    ui(shots, "ui_label", "\"text\":\"Balls 0\",\"size\":30,\"bold\":true,\"horizontal_align\":\"right\","
                          "\"color\":[1,0.84,0.36,1]," + shadow);
    // The menu is hidden until Tab opens it; its column places everything inside.
    const auto menu = node("Menu", "Panel", hud);
    ui(menu, "ui_control", "\"offset_min\":[-240,-236],\"offset_max\":[240,236],\"visible\":false");
    ui(menu, "ui_panel", "\"color\":[0.07,0.08,0.1,0.93],\"corner_radius\":18,\"border_width\":1,"
                         "\"border_color\":[1,1,1,0.1],\"shadow_color\":[0,0,0,0.55],\"shadow_size\":48,"
                         "\"shadow_offset\":[0,12]");
    const auto column = node("Menu layout", "VBoxContainer", menu);
    ui(column, "ui_control", "\"anchor_min\":[0,0],\"anchor_max\":[1,1],\"offset_min\":[0,0],\"offset_max\":[0,0]");
    ui(column, "ui_container", "\"padding\":[36,30,36,30],\"spacing\":18");
    const auto title = node("Title", "Label", column);
    ui(title, "ui_control", "\"min_size\":[0,64]");
    ui(title, "ui_label", "\"text\":\"Paused\",\"size\":44,\"bold\":true,\"horizontal_align\":\"center\"");
    const auto resume = node("Resume", "Button", column);
    ui(resume, "ui_control", "\"min_size\":[0,60]");
    ui(resume, "ui_panel", "\"color\":[0.2,0.45,0.9,1],\"corner_radius\":12");
    ui(resume, "ui_button", "\"hover_color\":[0.28,0.54,1,1],\"pressed_color\":[0.14,0.33,0.7,1]");
    ui(resume, "ui_label", "\"text\":\"Resume\",\"size\":24,\"bold\":true");
    const auto switch_node = node("Show hints", "CheckBox", column);
    ui(switch_node, "ui_toggle", "\"checked\":true,\"style\":\"switch\",\"box_size\":28");
    ui(switch_node, "ui_label", "\"text\":\"Show hints\",\"size\":22");
    const auto volume_label = node("Music volume label", "Label", column);
    ui(volume_label, "ui_control", "\"min_size\":[0,28]");
    ui(volume_label, "ui_label", "\"text\":\"Music volume\",\"size\":20,\"color\":[0.8,0.82,0.86,1]");
    const auto volume = node("Music volume", "Slider", column);
    ui(volume, "ui_control", "\"min_size\":[0,32]");
    ui(volume, "ui_slider", "\"value\":1");
    const auto footer = node("Footer", "Label", column);
    ui(footer, "ui_control", "\"expand_y\":true");
    ui(footer, "ui_label", "\"text\":\"Tab closes the menu\",\"size\":18,\"horizontal_align\":\"center\","
                           "\"vertical_align\":\"bottom\",\"color\":[0.6,0.62,0.66,1]");
    demo.script(hud, "GameMenu");
    demo.reference(hud, 0, "menu", menu);
    demo.reference(hud, 0, "crosshair", crosshair);
    demo.reference(hud, 0, "hints", hints);
    demo.reference(hud, 0, "shots_label", shots);
}

// Particle effects: a campfire under the warm fill light (glowing coals, flames, rising embers and
// lit smoke drifting on the wind) and a fireworks launcher to the right, in front of the joints
// playground, whose rockets burst through a sub emitter. Emitters play in the editor too, as they are tuned.
void add_particles(Builder& demo) {
    const auto emitter = [&](const std::string& name, const std::string& parent) {
        const auto result = demo.call("scene.create", "\"name\":" + Builder::text(name) +
                                                          ",\"type\":\"ParticleEmitter\",\"parent\":" + Builder::text(parent));
        return *relay::field(*result.object(), "entity")->string();
    };
    const auto campfire = demo.create("Campfire");
    demo.transform(campfire, {-3.4, 0, 3.9});
    for (int stone = 0; stone < 8; ++stone) {
        const double angle = stone * pi / 4.0 + 0.2;
        demo.mesh_entity("Stone " + std::to_string(stone + 1), campfire, "Sphere", "Ground",
                         {0.42 * std::cos(angle), 0.05, 0.42 * std::sin(angle)}, {0, stone * 37.0, 0},
                         {0.2, 0.13, 0.17});
    }
    // Three logs crossed over the coals, each lying on its side.
    for (int log = 0; log < 3; ++log)
        demo.mesh_entity("Log " + std::to_string(log + 1), campfire, "Cylinder", "Rubber",
                         {0, 0.06 + log * 0.05, 0}, {88, log * 60.0 + 15.0, 0}, {0.08, 0.66, 0.08});
    demo.mesh_entity("Coals", campfire, "Sphere", "Glow", {0, 0.02, 0}, {}, {0.34, 0.06, 0.34});
    const auto flames = emitter("Flames", campfire);
    demo.transform(flames, {0, 0.08, 0});
    demo.particles(flames, "\"prewarm\":true,\"max_particles\":300,\"rate\":80,\"shape\":\"cone\",\"radius\":0.2,"
                           "\"angle\":6,\"lifetime\":[0.5,0.85],\"speed\":[0.8,1.3],\"size\":[0.3,0.5],"
                           "\"rotation\":[0,360],\"angular_velocity\":[-70,70],\"gravity\":-0.15,"
                           "\"noise_strength\":0.45,\"noise_frequency\":1.6,\"noise_scroll\":1.2,"
                           "\"size_over_lifetime\":[[0,0.5],[0.25,1],[1,0.25]],"
                           "\"color\":[1,0.62,0.2,1],\"random_color\":true,\"color_alt\":[1,0.36,0.08,1],"
                           "\"color_over_lifetime\":[[0,1,0.9,0.6,0],[0.1,1,0.85,0.45,1],[0.5,1,0.45,0.12,0.8],"
                           "[1,0.6,0.1,0.02,0]],\"builtin_texture\":\"smoke\",\"emission\":3.5,"
                           "\"soft_distance\":0.15,\"seed\":11");
    const auto embers = emitter("Embers", campfire);
    demo.transform(embers, {0, 0.15, 0});
    demo.particles(embers, "\"prewarm\":true,\"rate\":10,\"shape\":\"sphere\",\"radius\":0.15,\"lifetime\":[1.4,2.6],"
                           "\"speed\":[0.3,0.8],\"direction_randomness\":0.3,\"size\":[0.018,0.032],"
                           "\"gravity\":-0.12,\"drag\":0.4,\"noise_strength\":0.9,\"noise_frequency\":0.9,"
                           "\"noise_scroll\":0.6,\"velocity\":[0,0.6,0],\"acceleration\":[0.25,0,0],"
                           "\"color\":[1,0.62,0.22,1],\"color_over_lifetime\":[[0,1,1,1,1],[0.7,1,0.6,0.4,1],[1,1,0.3,0.1,0]],"
                           "\"builtin_texture\":\"spark\",\"alignment\":\"stretched\",\"stretch_speed\":0.05,"
                           "\"stretch_length\":1.5,\"blend\":\"additive\",\"emission\":5,\"soft_distance\":0,\"seed\":12");
    const auto smoke = emitter("Smoke", campfire);
    demo.transform(smoke, {0, 0.75, 0});
    demo.particles(smoke, "\"prewarm\":true,\"rate\":9,\"shape\":\"cone\",\"radius\":0.12,\"angle\":10,"
                          "\"lifetime\":[3.5,5],\"speed\":[0.45,0.7],\"size\":[0.3,0.45],\"rotation\":[0,360],"
                          "\"angular_velocity\":[-18,18],\"acceleration\":[0.12,0.05,-0.04],\"drag\":0.15,"
                          "\"noise_strength\":0.2,\"noise_frequency\":0.4,"
                          "\"size_over_lifetime\":[[0,0.6],[1,3.2]],\"color\":[0.36,0.32,0.29,0.8],"
                          "\"color_over_lifetime\":[[0,1,1,1,0],[0.15,1,1,1,0.85],[0.6,1,1,1,0.55],[1,1,1,1,0]],"
                          "\"builtin_texture\":\"smoke\",\"lit\":true,\"soft_distance\":0.4,\"seed\":13");

    // Fireworks: one rocket every 1.4 s streaks up and bursts into colored sparks where it dies.
    const auto launcher = demo.mesh_entity("Fireworks launcher", {}, "Cylinder", "Brick", {7, 0.25, -6}, {},
                                           {0.18, 0.5, 0.18});
    const auto rockets = emitter("Fireworks", launcher);
    demo.transform(rockets, {0, 0.6, 0});
    demo.particles(rockets, "\"rate\":0,\"duration\":1.4,\"bursts\":[{\"time\":0,\"count\":1}],\"shape\":\"cone\","
                            "\"radius\":0,\"angle\":8,\"lifetime\":[1.1,1.35],\"speed\":[10,12],\"gravity\":0.45,"
                            "\"size\":[0.16,0.16],\"color\":[1,0.85,0.6,1],\"builtin_texture\":\"spark\","
                            "\"alignment\":\"stretched\",\"stretch_speed\":0.06,\"blend\":\"additive\",\"emission\":5,"
                            "\"simulation_space\":\"world\",\"soft_distance\":0,\"sub_emitter\":\"Burst\","
                            "\"sub_emitter_count\":160,\"sub_emitter_inherit\":0.15,\"seed\":21");
    const auto burst = emitter("Burst", rockets);
    demo.particles(burst, "\"play_on_start\":false,\"rate\":0,\"shape\":\"point\",\"max_particles\":1200,"
                          "\"lifetime\":[1.3,2],\"speed\":[6,9],\"gravity\":0.3,\"drag\":1.3,"
                          "\"size\":[0.35,0.5],\"size_over_lifetime\":[[0,1],[1,0.4]],\"random_color\":true,\"color\":[1,0.22,0.1,1],"
                          "\"color_alt\":[0.25,0.55,1,1],\"color_over_lifetime\":[[0,1,1,1,1],[0.6,1,1,1,0.9],[1,1,1,1,0]],"
                          "\"builtin_texture\":\"star\",\"blend\":\"additive\",\"emission\":3,\"soft_distance\":0,"
                          "\"seed\":22");
}

// A glowing gold trail behind the ball, emitted as it moves and left in the world.
void add_ball_trail(Builder& demo, const std::string& ball) {
    const auto result = demo.call("scene.create", "\"name\":\"Trail\",\"type\":\"ParticleEmitter\",\"parent\":" +
                                                      Builder::text(ball));
    const auto trail = *relay::field(*result.object(), "entity")->string();
    demo.particles(trail, "\"rate\":0,\"rate_over_distance\":14,\"shape\":\"sphere\",\"radius\":0.3,"
                          "\"lifetime\":[0.35,0.55],\"speed\":[0,0.2],\"size\":[0.1,0.16],"
                          "\"size_over_lifetime\":[[0,1],[1,0]],\"color\":[1,0.78,0.34,1],"
                          "\"color_over_lifetime\":[[0,1,1,1,0.9],[1,1,0.6,0.2,0]],\"blend\":\"additive\","
                          "\"emission\":2.5,\"soft_distance\":0,\"max_particles\":200");
}

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

// Finds the demo's primitive meshes and materials in the asset registry. Importers may reorder,
// drop or add materials, so assets are identified by their contents: meshes by index count,
// materials by base colour. `imported`, when given, limits the search to one import's assets.
bool resolve_primitives(Builder& demo, const relay::JsonValue& imported) {
    const bool limited = imported.object() != nullptr;
    const auto ids = limited ? strings(imported, "meshes") : std::vector<std::string>{};
    const auto material_ids = limited ? strings(imported, "materials") : std::vector<std::string>{};
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
        if (limited && std::find(ids.begin(), ids.end(), name) == ids.end()) continue;
        for (const auto& [label, count] : mesh_indices)
            if (number_of(mesh, "indices") == count) demo.meshes[label] = name;
    }
    for (const auto& value : *relay::field(*registry.object(), "materials")->array()) {
        const auto& material = *value.object();
        const auto name = string_of(material, "name");
        const auto* color = relay::field(material, "color");
        if ((limited && std::find(material_ids.begin(), material_ids.end(), name) == material_ids.end()) ||
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
        return false;
    }
    return true;
}

} // namespace

// Adds the interface to an existing demo's startup scene, replacing an older one.
int add_interface_only(const std::filesystem::path& root) {
    Builder demo;
    const auto project_file = (root / "demo.relayproject").generic_string();
    demo.call("project.open", "\"filename\":" + Builder::text(project_file));
    const auto listed = demo.call("scene.list");
    for (const auto& value : *relay::field(*listed.object(), "entities")->array())
        if (const auto& entity = *value.object(); string_of(entity, "name") == "HUD" && string_of(entity, "type") == "Canvas")
            demo.call("scene.destroy", "\"entity\":" + Builder::text(string_of(entity, "entity")));
    add_interface(demo);
    demo.call("scene.save", "\"filename\":\"showcase.relay.json\"");
    std::cout << "Added the interface to " << project_file << '\n';
    return 0;
}

// Adds the particle effects to an existing demo's startup scene, replacing older ones, and gives
// the Ball template its trail.
int add_particles_only(const std::filesystem::path& root) {
    Builder demo;
    const auto project_file = (root / "demo.relayproject").generic_string();
    demo.call("project.open", "\"filename\":" + Builder::text(project_file));
    if (!resolve_primitives(demo, {})) return 1;
    const auto listed = demo.call("scene.list");
    for (const auto& value : *relay::field(*listed.object(), "entities")->array())
        if (const auto& entity = *value.object();
            (string_of(entity, "name") == "Campfire" || string_of(entity, "name") == "Fireworks launcher") &&
            relay::field(entity, "parent")->is_null())
            demo.call("scene.destroy", "\"entity\":" + Builder::text(string_of(entity, "entity")));
    add_particles(demo);
    demo.call("scene.save", "\"filename\":\"showcase.relay.json\"");
    // The Ball template, placed away from the scene, given a fresh trail and saved again.
    const auto placed = demo.call("templates.instantiate", "\"template\":\"project:Ball\"");
    const auto ball = *relay::field(*placed.object(), "entity")->string();
    const auto after = demo.call("scene.list");
    for (const auto& value : *relay::field(*after.object(), "entities")->array())
        if (const auto& entity = *value.object(); string_of(entity, "name") == "Trail" && string_of(entity, "parent") == ball)
            demo.call("scene.destroy", "\"entity\":" + Builder::text(string_of(entity, "entity")));
    add_ball_trail(demo, ball);
    demo.call("templates.save", "\"entity\":" + Builder::text(ball) + ",\"name\":\"Ball\",\"replace\":true");
    std::cout << "Added the particle effects to " << project_file << " and a trail to the Ball template\n";
    return 0;
}

int main(const int argument_count, char** arguments) {
    if (argument_count > 1 && std::string_view(arguments[1]) == "--add-interface")
        return add_interface_only(argument_count > 2 ? arguments[2] : "examples/demo");
    if (argument_count > 1 && std::string_view(arguments[1]) == "--add-particles")
        return add_particles_only(argument_count > 2 ? arguments[2] : "examples/demo");
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
    if (!resolve_primitives(demo, imported)) return 1;

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
            demo.impact(entity, 1.0);
        }
    const auto wrecking = demo.mesh_entity("Wrecking ball", playground, "Sphere", "Chrome",
                                           {-4.3, 6.0, 1.6});
    demo.collider(wrecking, "\"type\":\"sphere\",\"radius\":0.5");
    demo.body(wrecking, 4.0, 0.3, 0.4);
    demo.impact(wrecking, 0.55, 3.0);
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
        demo.impact(entity, 1.25 + 0.15 * static_cast<double>(index), -3.0);
    }
    const auto capsule = demo.mesh_entity("Tumbling capsule", playground, "Capsule", "Copper",
                                          {1.5, 3.5, 3.8}, {0, 0, 60});
    demo.collider(capsule, "\"type\":\"capsule\",\"radius\":0.25,\"half_height\":0.5");
    demo.body(capsule, 1.2, 0.1, 0.5);
    demo.impact(capsule, 1.15);
    const auto barrel = demo.mesh_entity("Barrel (convex hull)", playground, "Cylinder", "Ocean",
                                         {0.5, 5.0, 2.2}, {80, 0, 20}, {0.7, 0.9, 0.7});
    demo.collider(barrel, "\"type\":\"convex\"");
    demo.body(barrel, 2.0, 0.1, 0.5);
    demo.impact(barrel, 0.8);

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
    // It hums as it turns: walk around it to hear the sound pan and fade.
    demo.sound(torus, "\"clip\":\"sounds/hum.wav\",\"bus\":\"Ambience\",\"loop\":true,"
                      "\"play_on_start\":true,\"spatial\":true,\"volume_db\":-6,"
                      "\"min_distance\":1.5,\"max_distance\":16");

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
        demo.impact(entity, 1.8, -6.0);
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
    demo.impact(door, 0.7);
    joint(door, "\"type\":\"hinge\",\"connected\":" + Builder::text(post) +
                    ",\"anchor_x\":-0.5,\"limits\":true,\"limit_min\":-100,\"limit_max\":100");
    const auto spinner = demo.mesh_entity("Spinner", joints, "Cube", "Ocean", {4, 0.35, -9}, {},
                                          {2.4, 0.2, 0.3});
    demo.collider(spinner, "\"type\":\"box\"");
    demo.body(spinner, 20.0, 0.1, 0.5);
    demo.impact(spinner, 0.6);
    joint(spinner, "\"type\":\"hinge\",\"motor\":true,\"motor_speed\":60,\"motor_force\":5000");
    const auto bungee = demo.mesh_entity("Bungee ball", joints, "Sphere", "Gold", {7.5, 2.5, -9}, {},
                                         {0.6, 0.6, 0.6});
    demo.collider(bungee, "\"type\":\"sphere\",\"radius\":0.5");
    demo.body(bungee, 1.0, 0.3, 0.4);
    demo.impact(bungee, 1.4, -3.0);
    joint(bungee, "\"type\":\"distance\",\"connected_anchor_x\":7.5,\"connected_anchor_y\":5,"
                  "\"connected_anchor_z\":-9,\"limits\":true,\"limit_min\":0,\"limit_max\":1.5,"
                  "\"spring_frequency\":1.2,\"spring_damping\":0.1");

    // Tone button, just ahead and to the right of where the player starts: look at the cap and
    // press interact, or hit it with a ball, to play the next note (scripts/ToneButton.cpp).
    const auto sounds = demo.create("Sound tests");
    const auto stand = demo.mesh_entity("Button stand", sounds, "Cylinder", "Chrome", {1.5, 0.5, 6.5},
                                        {}, {0.5, 1.0, 0.5});
    demo.collider(stand, "\"type\":\"convex\"");
    const auto button = demo.mesh_entity("Tone button", sounds, "Cylinder", "Glow", {1.5, 1.05, 6.5},
                                         {}, {0.35, 0.1, 0.35});
    demo.collider(button, "\"type\":\"convex\"");
    demo.sound(button, "\"clip\":\"sounds/tone.wav\",\"bus\":\"SFX\",\"play_on_start\":false,"
                       "\"spatial\":true,\"min_distance\":2,\"max_distance\":25");
    demo.script(button, "ToneButton");

    // The soundtrack: two 16 s tracks (they stream), crossfading on the bar, and a pad to the
    // left of the start that moves on to the next track (scripts/MusicSwitch.cpp).
    const auto soundtrack = demo.create("Soundtrack", sounds);
    demo.call("component.add", "\"entity\":" + Builder::text(soundtrack) + ",\"component\":\"music_player\"");
    demo.call("scene.set_music_player", "\"entity\":" + Builder::text(soundtrack) +
                                            ",\"tracks\":[\"music/daylight.wav\",\"music/dusk.wav\"],"
                                            "\"volume_db\":-8,\"crossfade_seconds\":1,\"bpm\":120,"
                                            "\"beats_per_bar\":4,\"sync\":\"bar\"");
    const auto switch_stand = demo.mesh_entity("Switch stand", sounds, "Cylinder", "Chrome",
                                               {-1.5, 0.5, 6.5}, {}, {0.5, 1.0, 0.5});
    demo.collider(switch_stand, "\"type\":\"convex\"");
    const auto music_switch = demo.mesh_entity("Music switch", sounds, "Cube", "Ocean",
                                               {-1.5, 1.05, 6.5}, {}, {0.4, 0.1, 0.4});
    demo.collider(music_switch, "\"type\":\"box\"");
    demo.script(music_switch, "MusicSwitch");

    // A hall's reverb around the joints playground: walk in from the start to hear the thumps
    // and the button ring on.
    const auto hall = demo.create("Hall reverb", sounds);
    demo.transform(hall, {1.5, 3, -9});
    demo.call("scene.set_reverb_zone", "\"entity\":" + Builder::text(hall) +
                                           ",\"shape\":\"box\",\"half_x\":8,\"half_y\":3,"
                                           "\"half_z\":3.5,\"fade\":3,\"preset\":\"hall\"");

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
    // The game hears from the player's eyes.
    demo.call("component.add", "\"entity\":" + Builder::text(player_camera) +
                                   ",\"component\":\"audio_listener\"");
    demo.call("scene.set_camera", "\"entity\":" + Builder::text(player_camera) +
                                      ",\"enabled\":true,\"active\":false,"
                                      "\"field_of_view_y_degrees\":75,\"near_plane\":0.05");
    // The script holds the camera as a node reference; saving the template keeps it pointing at the
    // template's own Camera.
    demo.reference(player, 0, "camera", player_camera);
    demo.call("templates.save", player_field + ",\"name\":\"First Person Controller\",\"replace\":true");
    demo.transform(player, {0, 1, 8});
    demo.call("scene.set_camera", "\"entity\":" + Builder::text(player_camera) + ",\"active\":true");
    // The sound tests find the player, the way MusicSwitch also finds its soundtrack, by reference.
    demo.reference(button, 0, "player", player);
    demo.reference(music_switch, 0, "player", player);
    demo.reference(music_switch, 0, "music", soundtrack);
    add_interface(demo);
    add_particles(demo);

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
    demo.impact(ball, 1.5, -4.0);
    add_ball_trail(demo, ball);
    demo.call("templates.save", ball_field + ",\"name\":\"Ball\",\"replace\":true");
    demo.call("scene.destroy", ball_field);

    std::cout << "Wrote " << project_file
              << " with scenes/showcase.relay.json (with a First Person Controller and sound tests) "
                 "and the First Person Controller and Ball templates\n";
    return 0;
}
