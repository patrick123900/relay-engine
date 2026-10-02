// Native gameplay scripts: trust, background builds, lifecycle, contacts, errors, hot reload and
// the demo project's first person controller.
// These compile real C++ with the configured compiler, so they take a few seconds.
#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"
#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/scene/scene_io.hpp"
#include "relay/script/script_system.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <set>
#include <string>
#include <thread>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool ok(const std::string& response) { return response.find("\"ok\":true") != std::string::npos; }

std::string request(relay::ControlProtocol& protocol, const std::string& method,
                    const std::string& fields = {}) {
    static std::uint64_t id = 1;
    return protocol.handle("{\"id\":" + std::to_string(id++) + ",\"method\":\"" + method + "\"" +
                           (fields.empty() ? "" : "," + fields) + "}");
}

std::string quoted(const std::string& text) { return "\"" + relay::json_escape(text) + "\""; }

std::string write_script(relay::ControlProtocol& protocol, const std::string& path,
                         const std::string& source) {
    return request(protocol, "scripts.write", "\"path\":" + quoted(path) + ",\"source\":" + quoted(source));
}

// Waits for the background build and returns the final status response.
std::string wait_for_build(relay::Engine& engine, relay::ControlProtocol& protocol) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{240};
    while (std::chrono::steady_clock::now() < deadline) {
        engine.tick();
        auto status = request(protocol, "scripts.status");
        if (status.find("\"state\":\"building\"") == std::string::npos) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    throw std::runtime_error("script build did not finish");
}

bool logged(const relay::Engine& engine, const std::string& text) {
    for (const auto& entry : engine.logs().read_after(0))
        if (entry.message.find(text) != std::string::npos) return true;
    return false;
}

relay::Entity entity_from(const std::string& response) {
    const auto marker = response.find("\"entity\":\"");
    if (marker != std::string::npos) {
        const auto start = marker + 10U;
        if (const auto parsed = relay::Entity::parse(response.substr(start, response.find('"', start) - start)))
            return *parsed;
    }
    const auto id = response.find("\"id\":\"");
    const auto start = id + 6U;
    return relay::Entity::parse(response.substr(start, response.find('"', start) - start)).value_or(relay::Entity{});
}

std::string add_script(relay::ControlProtocol& protocol, relay::Entity entity,
                       const std::string& behaviour) {
    return request(protocol, "component.add", "\"entity\":\"" + entity.to_string() +
                                                  "\",\"component\":\"script\",\"behaviour\":\"" +
                                                  behaviour + "\"");
}

std::string set_property(relay::ControlProtocol& protocol, relay::Entity entity, int index,
                         const std::string& property, const std::string& value) {
    return request(protocol, "scene.set_script_property",
                   "\"entity\":\"" + entity.to_string() + "\",\"index\":" + std::to_string(index) +
                       ",\"property\":\"" + property + "\"," + value);
}

const char* mover_source = R"(#include "relay_script.hpp"
#include <stdexcept>

class Mover : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("speed", speed);
        p.add("axis", axis);
    }
    void on_start() override { relay::world::log("mover started"); }
    void on_update(double dt) override { self().set_position(self().position() + axis * (speed * dt)); }
    void on_stop() override { relay::world::log("mover stopped"); }
    void on_reload() override { relay::world::log("mover reloaded"); }
private:
    double speed = 6.0;
    relay::Vec3 axis{1, 0, 0};
};
RELAY_BEHAVIOUR(Mover)

class Tagger : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("label", label);
        p.add("count", count);
        p.add("loud", loud);
        p.add("scale", scale);
    }
    void on_start() override {
        relay::world::log("tag " + label + " " + std::to_string(count) + (loud ? " loud" : " quiet") +
                          " " + std::to_string(static_cast<int>(scale * 10)));
    }
private:
    std::string label = "plain";
    int count = 2;
    bool loud = false;
    float scale = 0.5f;
};
RELAY_BEHAVIOUR(Tagger)

class Faulty : public relay::Behaviour {
public:
    void on_update(double) override {
        if (relay::world::frame() >= 3) throw std::runtime_error("faulty behaviour gave up");
        self().set_position(self().position() + relay::Vec3{0, 1, 0});
    }
};
RELAY_BEHAVIOUR(Faulty)

class Controlled : public relay::Behaviour {
public:
    void on_update(double dt) override {
        auto position = self().position();
        if (relay::input::pressed("jump")) position.y += 1.0;
        position.x += relay::input::axis("move_x") * 6.0 * dt;
        if (relay::input::key_held("q")) position.z += 1.0;
        self().set_position(position);
    }
};
RELAY_BEHAVIOUR(Controlled)
)";

const char* probe_source = R"(#include "relay_script.hpp"

class ContactProbe : public relay::Behaviour {
public:
    void on_start() override {
        const auto hit = relay::world::raycast(self().world_position(), {0, -1, 0}, 100.0,
                                               0xffffffffu, self());
        relay::world::log(hit ? "ray hit " + hit->entity.name() : std::string{"ray missed"});
        floor = relay::world::find("Floor");
    }
    void on_contact_begin(relay::Entity other) override {
        relay::world::log("contact begin " + other.name());
        if (other == floor) self().set_velocity({0, 8, 0});
    }
    void on_contact_end(relay::Entity other) override {
        relay::world::log("contact end " + other.name());
    }
private:
    relay::Entity floor;
};
RELAY_BEHAVIOUR(ContactProbe)
)";

void untrusted_projects(relay::Engine& engine, relay::ControlProtocol& protocol) {
    const auto build = request(protocol, "scripts.build");
    expect(!ok(build) && build.find("not trusted") != std::string::npos,
           "untrusted project refuses to build native scripts");
    const auto* trust = relay::find_protocol_method("scripts.trust");
    expect(trust && trust->host_only, "only the host can trust project scripts");
    const auto status = request(protocol, "scripts.status");
    expect(status.find("\"trusted\":false") != std::string::npos &&
               status.find("\"project\":true") != std::string::npos,
           "status reports an open, untrusted project");
    expect(ok(request(protocol, "scripts.create", "\"behaviour\":\"Starter\"")),
           "creating a script works without trust, since nothing runs");
    expect(!ok(request(protocol, "scripts.create", "\"behaviour\":\"Starter\"")),
           "script creation refuses to overwrite");
    const auto starter = request(protocol, "scripts.read", "\"path\":\"Starter.cpp\"");
    expect(starter.find("RELAY_BEHAVIOUR(Starter)") != std::string::npos,
           "starter template registers the named behaviour");
    const auto scripted = entity_from(request(protocol, "scene.create", "\"name\":\"Scripted\""));
    expect(ok(request(protocol, "component.add", "\"entity\":\"" + scripted.to_string() +
                                                     "\",\"component\":\"script\",\"behaviour\":\"Starter\"")),
           "script component attaches");
    const auto play = request(protocol, "runtime.play");
    expect(!ok(play) && play.find("not trusted") != std::string::npos,
           "Run Game refuses scripted scenes in untrusted projects");
    expect(ok(request(protocol, "component.remove", "\"entity\":\"" + scripted.to_string() +
                                                        "\",\"component\":\"script\",\"index\":0")),
           "script component detaches");
    expect(engine.run_game() && engine.stop_game(),
           "scenes without enabled scripts run in untrusted projects");
    std::filesystem::remove("projects/scripts/scripts/Starter.cpp");
}

void path_rules(relay::ControlProtocol& protocol) {
    expect(!ok(write_script(protocol, "nested/../../escape.cpp", "int x;")),
           "script paths cannot traverse out of scripts/");
    expect(!ok(write_script(protocol, ".hidden.cpp", "int x;")), "hidden script files are refused");
    expect(!ok(write_script(protocol, "notes.txt", "hello")), "only C++ files are script sources");
    expect(!ok(request(protocol, "scripts.create", "\"behaviour\":\"9lives\"")),
           "behaviour names must be identifiers");
    const auto sdk = request(protocol, "scripts.sdk");
    expect(ok(sdk) && sdk.find("class Behaviour") != std::string::npos,
           "agents can read the complete script SDK");
}

void lifecycle(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(request(protocol, "scripts.trust", "\"trusted\":true")), "host trusts the project");
    expect(relay::project_scripts_trusted(engine.project()->root()),
           "trust is stored outside the project");
    expect(ok(write_script(protocol, "mover.cpp", mover_source)), "script source is written");
    expect(ok(write_script(protocol, "probe/contact_probe.cpp", probe_source)),
           "scripts may live in subfolders");
    expect(request(protocol, "scripts.status").find("\"stale\":true") != std::string::npos,
           "new sources mark the scripts stale");

    const auto mover = entity_from(request(protocol, "scene.create", "\"name\":\"Mover\""));
    expect(ok(add_script(protocol, mover, "Mover")), "mover script attaches");
    const auto early = request(protocol, "runtime.play");
    expect(!ok(early) && early.find("run scripts.build") != std::string::npos,
           "Run Game refuses to run scripts that were never built");

    expect(ok(request(protocol, "scripts.build")), "trusted project starts a build");
    auto status = wait_for_build(engine, protocol);
    expect(status.find("\"state\":\"ready\"") != std::string::npos,
           "scripts build and load: " + status.substr(0, 600));
    const auto probe_at = status.find("{\"name\":\"ContactProbe\"");
    const auto faulty_at = status.find("{\"name\":\"Faulty\"");
    const auto mover_at = status.find("{\"name\":\"Mover\"");
    expect(probe_at != std::string::npos && probe_at < faulty_at && faulty_at < mover_at,
           "status lists every registered behaviour, sorted");
    const auto types = request(protocol, "component.types");
    expect(types.find("{\"name\":\"speed\",\"type\":\"number\",\"value\":6}") != std::string::npos &&
               types.find("{\"name\":\"axis\",\"type\":\"vector\",\"value\":[1,0,0]}") != std::string::npos &&
               types.find("{\"name\":\"label\",\"type\":\"text\",\"value\":\"plain\"}") != std::string::npos &&
               types.find("{\"name\":\"loud\",\"type\":\"boolean\",\"value\":false}") != std::string::npos &&
               types.find("{\"id\":\"camera\",\"name\":\"Camera\",\"category\":\"Rendering\"") != std::string::npos,
           "component.types lists engine components and behaviour properties with code defaults: " +
               types.substr(0, 500));

    // A second Mover with its own values, and a node carrying two scripts.
    const auto tuned = entity_from(request(protocol, "scene.create", "\"name\":\"Tuned\""));
    expect(ok(add_script(protocol, tuned, "Mover")) && ok(add_script(protocol, tuned, "Tagger")),
           "a node carries several script components");
    expect(ok(set_property(protocol, tuned, 0, "speed", "\"number\":12")) &&
               ok(set_property(protocol, tuned, 0, "axis", "\"vector\":[0,0,1]")) &&
               ok(set_property(protocol, tuned, 1, "label", "\"text\":\"hello\"")) &&
               ok(set_property(protocol, tuned, 1, "count", "\"number\":5")) &&
               ok(set_property(protocol, tuned, 1, "loud", "\"boolean\":true")) &&
               ok(set_property(protocol, tuned, 1, "scale", "\"number\":0.8")),
           "properties of every type can be overridden per component");
    expect(!ok(set_property(protocol, tuned, 1, "label", "\"number\":1,\"text\":\"two\"")),
           "a property value has exactly one type");
    expect(!ok(set_property(protocol, tuned, 4, "label", "\"text\":\"x\"")),
           "property edits need an existing script component");
    const auto odd = entity_from(request(protocol, "scene.create", "\"name\":\"Odd\""));
    (void)add_script(protocol, odd, "Tagger");
    (void)set_property(protocol, odd, 0, "label", "\"number\":3");
    (void)set_property(protocol, odd, 0, "removed_field", "\"number\":3");
    expect(status.find("\"stale\":false") != std::string::npos, "a finished build is current");

    const auto player = entity_from(request(protocol, "scene.create", "\"name\":\"Player\""));
    (void)add_script(protocol, player, "Controlled");
    const auto faulty = entity_from(request(protocol, "scene.create", "\"name\":\"Faulty\""));
    (void)add_script(protocol, faulty, "Faulty");
    const auto floor = entity_from(request(protocol, "scene.create", "\"name\":\"Floor\""));
    (void)request(protocol, "scene.set_collider", "\"entity\":\"" + floor.to_string() +
                                                      "\",\"half_x\":5,\"half_y\":0.5,\"half_z\":5");
    (void)request(protocol, "scene.set_physics_body",
                  "\"entity\":\"" + floor.to_string() + "\",\"type\":\"static\"");
    const auto ball = entity_from(request(protocol, "scene.create", "\"name\":\"Ball\""));
    (void)request(protocol, "scene.set_transform", "\"entity\":\"" + ball.to_string() + "\",\"py\":2");
    (void)request(protocol, "scene.set_collider",
                  "\"entity\":\"" + ball.to_string() + "\",\"type\":\"sphere\",\"radius\":0.5");
    (void)request(protocol, "scene.set_physics_body", "\"entity\":\"" + ball.to_string() + "\"");
    (void)add_script(protocol, ball, "ContactProbe");

    expect(ok(request(protocol, "runtime.play")), "Run Game starts with built scripts");
    expect(logged(engine, "[Mover " + mover.to_string() + "] mover started"),
           "on_start runs and logs with the behaviour and entity");
    expect(logged(engine, "ray hit Floor"), "scripts raycast against the live physics world");
    expect(logged(engine, "tag hello 5 loud 8"),
           "stored text, integer, boolean and float values reach the behaviour before on_start");
    expect(logged(engine, "tag plain 2 quiet 5"),
           "unset properties keep their code defaults");
    expect(logged(engine, "ignored property label") && logged(engine, "ignored property removed_field"),
           "mismatched or undeclared stored properties are skipped with a warning");
    engine.step(30);
    const auto moved = engine.scene().get(mover)->transform.position.x;
    expect(std::abs(moved - 3.0) < 1e-9, "on_update runs every fixed step: " + std::to_string(moved));
    const auto tuned_position = engine.scene().get(tuned)->transform.position;
    expect(std::abs(tuned_position.z - 6.0) < 1e-9 && tuned_position.x == 0.0,
           "each component uses its own stored property values");
    const auto faulty_y = engine.scene().get(faulty)->transform.position.y;
    expect(faulty_y == 2.0, "a behaviour that throws stops running: " + std::to_string(faulty_y));
    status = request(protocol, "scripts.status");
    expect(status.find("\"callback\":\"on_update\"") != std::string::npos &&
               status.find("faulty behaviour gave up") != std::string::npos &&
               status.find("\"entity\":\"" + faulty.to_string() + "\"") != std::string::npos,
           "runtime errors report entity, callback and message");
    expect(engine.scene().get(player)->transform.position == relay::Vec3{},
           "without input the player stays put");
    expect(ok(request(protocol, "input.simulate", "\"name\":\"jump\"")) &&
               ok(request(protocol, "input.simulate", "\"name\":\"move_x\",\"value\":1,\"frames\":10")),
           "input can be simulated during Run Game");
    engine.apply_input_event("key:down:q");
    engine.step(10);
    engine.apply_input_event("key:up:q");
    const auto moved_player = engine.scene().get(player)->transform.position;
    expect(moved_player.y == 1.0 && std::abs(moved_player.x - 1.0) < 1e-9 && moved_player.z == 10.0,
           "scripts read pressed actions once, axes every step, and raw keys: " +
               std::to_string(moved_player.x) + "," + std::to_string(moved_player.y) + "," +
               std::to_string(moved_player.z));
    expect(logged(engine, "contact begin Floor"), "contact begin reaches the scripted body");
    engine.step(40);
    expect(logged(engine, "contact end Floor"),
           "contact end follows the script's own velocity change");

    // Hot reload: new code replaces the running instances without restarting the game.
    std::string reversed = mover_source;
    reversed.replace(reversed.find("speed = 6.0"), 11, "speed = -6.0");
    expect(ok(write_script(protocol, "mover.cpp", reversed)), "scripts can be edited during Run Game");
    expect(ok(request(protocol, "scripts.build")), "rebuild starts during Run Game");
    status = wait_for_build(engine, protocol);
    expect(status.find("\"reloads\":1") != std::string::npos &&
               status.find("\"compiled_files\":1") != std::string::npos,
           "hot reload recompiles only the changed file: " + status.substr(0, 400));
    expect(logged(engine, "mover reloaded"), "on_reload runs after hot reload");
    const auto before = engine.scene().get(mover)->transform.position.x;
    const auto tuned_before = engine.scene().get(tuned)->transform.position.z;
    engine.step(10);
    expect(engine.scene().get(mover)->transform.position.x < before,
           "a changed code default reaches nodes that did not override it");
    expect(engine.scene().get(tuned)->transform.position.z > tuned_before,
           "stored values survive hot reload");
    expect(engine.scene().get(faulty)->transform.position.y > faulty_y ||
               request(protocol, "scripts.status").find("\"runtime_error_count\":2") != std::string::npos,
           "hot reload gives failed instances a fresh start");

    expect(ok(request(protocol, "runtime.stop")), "Stop Game ends the session");
    expect(logged(engine, "mover stopped"), "on_stop runs before the scene is restored");
    expect(engine.scene().get(mover)->transform.position.x == 0.0,
           "Stop Game restores what scripts changed");

    // A scene saved with scripts reloads them.
    expect(ok(request(protocol, "scene.save", "\"filename\":\"scripted.relay.json\"")), "scene saves");
    const auto loaded = relay::load_scene_file("projects/scripts/scenes/scripted.relay.json");
    const auto& saved_scripts = loaded.state->slots[tuned.index].record.scripts;
    expect(loaded && saved_scripts.size() == 2U && saved_scripts[0].behaviour == "Mover" &&
               saved_scripts[1].properties.size() == 4U &&
               saved_scripts[0].properties[1].type == relay::ScriptProperty::Type::vector &&
               saved_scripts[0].properties[1].vector.z == 1.0,
           "version 13 scenes save and load script components with their values");

    const auto missing = entity_from(request(protocol, "scene.create", "\"name\":\"Missing\""));
    (void)add_script(protocol, missing, "Absent");
    const auto absent = request(protocol, "runtime.play");
    expect(!ok(absent) && absent.find("Absent") != std::string::npos,
           "Run Game names a behaviour the scripts do not define");
    expect(ok(request(protocol, "scene.undo")), "script component changes are undoable");
    expect(engine.scene().get(missing)->scripts.empty(), "undo removes the added script");
}

void compile_errors(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(write_script(protocol, "broken.cpp",
                           "#include \"relay_script.hpp\"\n\nint broken() { return missing_name; }\n")),
           "broken script is written");
    expect(ok(request(protocol, "scripts.build")), "build of a broken script starts");
    const auto status = wait_for_build(engine, protocol);
    expect(status.find("\"state\":\"failed\"") != std::string::npos, "compile errors fail the build");
    expect(status.find("\"file\":\"scripts/broken.cpp\",\"line\":3") != std::string::npos &&
               status.find("\"severity\":\"error\"") != std::string::npos &&
               status.find("missing_name") != std::string::npos,
           "diagnostics carry project-relative file, line and message: " + status.substr(0, 800));
    const auto play = request(protocol, "runtime.play");
    expect(!ok(play) && play.find("build failed") != std::string::npos,
           "Run Game refuses to start after a failed build");
    expect(request(protocol, "scripts.status").find("\"stale\":false") != std::string::npos,
           "a failed build is not rebuilt until sources change");
    std::filesystem::remove("projects/scripts/scripts/broken.cpp");
    expect(request(protocol, "scripts.status").find("\"stale\":true") != std::string::npos,
           "fixing sources marks them stale again");
    expect(ok(request(protocol, "scripts.build")), "rebuild after fix starts");
    expect(wait_for_build(engine, protocol).find("\"state\":\"ready\"") != std::string::npos,
           "fixed scripts build");

    expect(ok(request(protocol, "scripts.trust", "\"trusted\":false")), "trust can be revoked");
    expect(request(protocol, "scripts.status").find("\"behaviours\":[]") != std::string::npos,
           "revoking trust unloads the script library");
}

// The demo project's First Person Controller: a template running scripts/FirstPersonController.cpp,
// play-tested with simulated input on a flat floor.
void demo_first_person(relay::Engine& engine, relay::ControlProtocol& protocol) {
    std::filesystem::create_directories("examples");
    std::filesystem::copy(std::filesystem::path{RELAY_TEST_SOURCE_DIR} / "examples/demo", "examples/demo",
                          std::filesystem::copy_options::recursive);
    std::filesystem::remove_all("examples/demo/.relay-cache");
    expect(ok(request(protocol, "project.open", "\"filename\":\"examples/demo/demo.relayproject\"")),
           "open a copy of the demo project");
    expect(ok(request(protocol, "scripts.trust", "\"trusted\":true")), "trust the demo project");
    expect(ok(request(protocol, "scripts.build")), "build the demo's scripts");
    const auto status = wait_for_build(engine, protocol);
    expect(status.find("\"state\":\"ready\"") != std::string::npos &&
               status.find("{\"name\":\"FirstPersonController\"") != std::string::npos,
           "the demo's FirstPersonController script builds: " + status.substr(0, 600));
    // The showcase itself starts with a player: Run Game plays through its camera.
    relay::Entity scene_player{}, scene_camera{};
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->name == "First Person Controller") scene_player = entity;
    for (const auto entity : engine.scene().entities())
        if (scene_player.valid() && engine.scene().get(entity)->parent == scene_player)
            scene_camera = entity;
    if (!scene_player.valid() || !scene_camera.valid()) {
        expect(false, "the showcase has a first person controller with a camera");
        return;
    }
    expect(engine.run_game(), "the trusted showcase runs with its controller");
    expect(engine.scene().active_camera() == scene_camera,
           "the showcase plays through the player's camera");
    engine.step(60);
    const auto landed = engine.scene().get(scene_player)->transform.position;
    (void)request(protocol, "input.simulate", "\"name\":\"move_y\",\"value\":1,\"frames\":30");
    engine.step(30);
    const auto moved = engine.scene().get(scene_player)->transform.position;
    expect(std::abs(landed.y - 0.9) < 0.05 && moved.z < landed.z - 1.5,
           "the showcase player stands on the ground and walks towards the playgrounds");
    expect(engine.stop_game() && engine.scene().active_camera() == scene_camera &&
               engine.scene().get(scene_player)->transform.position.z == 8.0,
           "Stop Game returns the player to its start");

    // Sound tests in the showcase: bodies thump as they land, the torus hums, and the tone button
    // plays a note when the player looks at it and presses interact.
    relay::Entity button{}, torus{}, hall{};
    for (const auto entity : engine.scene().entities()) {
        if (engine.scene().get(entity)->name == "Tone button") button = entity;
        if (engine.scene().get(entity)->name == "Spinning torus") torus = entity;
        if (engine.scene().get(entity)->name == "Hall reverb") hall = entity;
    }
    expect(button.valid() && torus.valid() && engine.scene().get(scene_camera)->audio_listener &&
               hall.valid() && engine.scene().get(hall)->reverb_zone,
           "the showcase has a tone button, a humming torus, a hall reverb zone and a listener "
           "on the player's camera");
    expect(engine.run_game(), "the showcase runs again for its sounds");
    std::set<relay::Entity> thumped;
    bool hummed = false;
    for (int frame = 0; frame < 120; ++frame) {
        engine.step(1);
        const auto text = engine.audio().status_json(engine.scene());
        relay::JsonParser parser(text);
        const auto audio = parser.parse();
        for (const auto& voice : *relay::field(*audio->object(), "voices")->array()) {
            const auto& object = *voice.object();
            const auto entity = relay::Entity::parse(*relay::field(object, "entity")->string());
            if (*relay::field(object, "clip")->string() == "sounds/thump.wav") thumped.insert(*entity);
            if (*entity == torus) hummed = true;
        }
    }
    expect(thumped.size() >= 5U, "falling bodies thump as they land (" +
                                     std::to_string(thumped.size()) + " did)");
    expect(hummed && engine.audio().listener() == scene_camera,
           "the torus hums, heard from the player's camera");
    // The player stands at (0, 1.6, 8) looking down -Z; the button's cap is at (1.5, 1.05, 6.5):
    // 45 degrees right and about 14.5 degrees down, at 0.12 degrees per pixel of mouse motion.
    engine.apply_input_event("mouse_motion:0:0:375:121");
    engine.step(2);
    expect(!engine.audio().playing(button), "the button is silent until pressed");
    (void)request(protocol, "input.simulate", "\"name\":\"interact\"");
    engine.step(2);
    expect(engine.audio().playing(button), "looking at the button and pressing interact plays a note");
    engine.step(8); // The duck takes 0.1 s.
    expect(engine.audio().bus_volume("Music").value_or(0.0) < -9.0,
           "the button ducks the music under its note");
    engine.step(120);
    expect(std::abs(engine.audio().bus_volume("Music").value_or(-99.0) -
                    engine.audio_settings().buses[1].volume_db) < 0.1,
           "and brings it back afterwards");
    relay::Entity soundtrack{};
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->name == "Soundtrack") soundtrack = entity;
    expect(soundtrack.valid() && engine.audio().music_track(soundtrack) == 0,
           "the soundtrack plays its first track");
    // The music switch sits 3 m to the left of the button: turn back 90 degrees (750 px) left.
    engine.apply_input_event("mouse_motion:0:0:-750:0");
    engine.step(2);
    (void)request(protocol, "input.simulate", "\"name\":\"interact\"");
    engine.step(2);
    int switched = -1;
    for (int frame = 0; frame < 150 && switched != 1; ++frame) {
        engine.step(1);
        switched = engine.audio().music_track(soundtrack);
    }
    expect(switched == 1, "the music switch moves the soundtrack on, on the next bar");
    (void)request(protocol, "input.simulate", "\"name\":\"fire\"");
    engine.step(1);
    const bool fired = engine.audio().status_json(engine.scene()).find(
                           "\"clip\":\"sounds/shot.wav\"") != std::string::npos;
    expect(fired, "firing plays the shot sound as a one-shot");
    expect(engine.stop_game(), "stop the sound tests");

    expect(ok(request(protocol, "scene.clear")), "start from an empty scene");

    const auto floor = engine.scene().create("Ground");
    (void)engine.scene().set_transform(floor, {{0, -0.5, 0}, {}, {1, 1, 1}});
    relay::BoxCollider ground;
    ground.half_extents = {50, 0.5, 50};
    (void)engine.scene().set_collider(floor, ground);
    (void)engine.scene().set_physics_body(floor, relay::PhysicsBody{relay::PhysicsBody::Type::static_body});
    const auto overview = engine.scene().create("Overview");
    relay::Camera overview_camera;
    overview_camera.active = true;
    (void)engine.scene().set_camera(overview, overview_camera);
    const auto player = entity_from(request(protocol, "templates.instantiate",
                                            "\"template\":\"project:First Person Controller\""));
    relay::Entity camera{};
    for (const auto entity : engine.scene().entities())
        if (engine.scene().get(entity)->parent == player) camera = entity;
    if (!engine.scene().contains(player) || !camera.valid() || !engine.scene().get(camera)->camera) {
        expect(false, "the template creates a player with a child camera");
        return;
    }

    expect(engine.run_game(), "the first person scene runs once its script is built");
    expect(engine.scene().get(camera)->camera->active && !engine.scene().get(overview)->camera->active,
           "the controller looks through its own camera during the game");
    engine.step(60);
    const auto rest = engine.scene().get(player)->transform;
    expect(std::abs(rest.position.y - 0.9) < 0.05 && rest.rotation_degrees == relay::Vec3{},
           "the capsule lands on the ground and stays upright");
    (void)request(protocol, "input.simulate", "\"name\":\"move_y\",\"value\":1,\"frames\":60");
    engine.step(60);
    auto walked = engine.scene().get(player)->transform;
    expect(walked.position.z < rest.position.z - 3.5 &&
               std::abs(walked.position.x - rest.position.x) < 0.05 &&
               walked.rotation_degrees == relay::Vec3{},
           "move_y walks forward along -Z at walking speed");
    engine.apply_input_event("mouse_motion:0:0:750:0");
    engine.step(1);
    expect(std::abs(engine.scene().get(camera)->transform.rotation_degrees.y + 90.0) < 1e-6,
           "moving the mouse right turns the view right");
    const auto turned = engine.scene().get(player)->transform.position;
    (void)request(protocol, "input.simulate", "\"name\":\"move_y\",\"value\":1,\"frames\":30");
    engine.step(30);
    walked = engine.scene().get(player)->transform;
    expect(walked.position.x > turned.x + 1.5 && std::abs(walked.position.z - turned.z) < 0.1,
           "walking follows the direction the camera faces");
    (void)request(protocol, "input.simulate", "\"name\":\"move_y\",\"value\":1,\"frames\":30");
    (void)request(protocol, "input.simulate", "\"name\":\"sprint\",\"frames\":30");
    const auto before_sprint = engine.scene().get(player)->transform.position.x;
    engine.step(30);
    expect(engine.scene().get(player)->transform.position.x - before_sprint > 3.2,
           "sprint moves faster than walking");
    engine.step(20);
    const auto grounded = engine.scene().get(player)->transform.position.y;
    (void)request(protocol, "input.simulate", "\"name\":\"jump\"");
    engine.step(10);
    expect(engine.scene().get(player)->transform.position.y > grounded + 0.3,
           "jump lifts the player off the ground");
    const auto rising = engine.physics().velocity(engine.scene(), player)->y;
    (void)request(protocol, "input.simulate", "\"name\":\"jump\"");
    engine.step(1);
    expect(rising > 2.0 && engine.physics().velocity(engine.scene(), player)->y < rising,
           "jumping again in mid-air does nothing");
    engine.step(90);
    expect(std::abs(engine.scene().get(player)->transform.position.y - grounded) < 0.05,
           "the player lands again");

    // Shooting: fire launches a Ball template along the view, which is turned to face +X.
    const auto balls = [&] {
        std::vector<relay::Entity> found;
        for (const auto entity : engine.scene().entities())
            if (engine.scene().get(entity)->name == "Ball") found.push_back(entity);
        return found;
    };
    const auto shooter = engine.scene().get(player)->transform.position;
    (void)request(protocol, "input.simulate", "\"name\":\"fire\"");
    engine.step(1);
    auto shots = balls();
    expect(shots.size() == 1U, "fire spawns one ball");
    if (shots.size() == 1U) {
        const auto velocity = engine.physics().velocity(engine.scene(), shots.front());
        const auto at = engine.scene().get(shots.front())->transform.position;
        expect(velocity && velocity->x > 18.0 && std::abs(velocity->z) < 0.5 &&
                   std::abs(velocity->y) < 1.0 && at.x > shooter.x + 0.3,
               "the ball starts in front of the camera and flies where it looks");
    }
    engine.apply_input_event("mouse_motion:0:0:0:-250");
    engine.step(1);
    (void)request(protocol, "input.simulate", "\"name\":\"fire\"");
    engine.step(1);
    const auto first_shot = shots.empty() ? relay::Entity{} : shots.front();
    shots = balls();
    std::optional<relay::Vec3> rising_ball;
    for (const auto shot : shots)
        if (shot != first_shot) rising_ball = engine.physics().velocity(engine.scene(), shot);
    expect(shots.size() == 2U && rising_ball && rising_ball->y > 8.0 && rising_ball->x > 15.0,
           "looking up 30 degrees shoots the ball upward");
    const auto standing = engine.scene().get(player)->transform.position;
    engine.step(30);
    expect(std::abs(engine.scene().get(player)->transform.position.x - standing.x) < 0.05,
           "the player's own balls pass through it");
    engine.step(400);
    expect(balls().empty(), "balls remove themselves after their lifetime");
    expect(engine.stop_game() && engine.scene().get(overview)->camera->active &&
               !engine.scene().get(camera)->camera->active &&
               engine.scene().get(player)->transform.position.y == 1.0,
           "Stop Game restores the scene's camera and the player's place");
}

const char* spawn_source = R"(#include "relay_script.hpp"
#include <string>

// Falls for `lifetime` updates, then destroys itself.
class Bullet : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override { p.add("lifetime", lifetime); }
    void on_start() override { relay::world::log("bullet start after " + std::to_string(updates)); }
    void on_update(double) override {
        if (++updates == lifetime) self().destroy();
    }
    void on_destroy() override {
        relay::world::log("bullet destroyed after " + std::to_string(updates) + " updates");
    }
private:
    int lifetime = 30;
    int updates = 0;
};
RELAY_BEHAVIOUR(Bullet)

class Pad : public relay::Behaviour {
public:
    void on_contact_begin(relay::Entity other) override { relay::world::log("pad touched by " + other.name()); }
    void on_contact_end(relay::Entity other) override {
        relay::world::log(std::string{"pad released a "} + (other.alive() ? "live" : "destroyed") + " entity");
    }
};
RELAY_BEHAVIOUR(Pad)

// Drives its hinge with the motor for half a second, then reports the angle.
class Crank : public relay::Behaviour {
public:
    void on_start() override {
        relay::world::log(std::string{"crank motor "} + (self().set_joint_motor(180.0) ? "on" : "missing"));
    }
    void on_update(double) override {
        if (relay::world::frame() != 31) return;
        const auto angle = self().joint_position();
        relay::world::log("crank turned " + (angle ? std::to_string(static_cast<int>(*angle)) : std::string{"?"}));
        self().stop_joint_motor();
    }
};
RELAY_BEHAVIOUR(Crank)

class Spawner : public relay::Behaviour {
public:
    void on_start() override {
        const auto missing = relay::world::instantiate("Missing");
        relay::world::log(missing ? "missing template spawned" : "missing template gives no entity");
        (void)relay::world::instantiate("Missing");
        group = relay::world::create("Bullets", self());
        crate = relay::world::find("Crate");
        relay::world::log("spawner children " + std::to_string(self().children().size()));
    }
    void on_update(double) override {
        const auto frame = relay::world::frame();
        if (frame == 2) {
            bullet = relay::world::instantiate("Bullet", {0, 5, 0}, std::nullopt, group);
            relay::world::log("spawned " + bullet.name() + " under " + bullet.parent().name());
        } else if (frame == 3) {
            const auto copy = bullet.clone();
            copy.set_position({2, 5, 0});
            relay::world::log("cloned " + copy.name() + ", bullets " +
                              std::to_string(group.children().size()));
        } else if (frame == 20) {
            std::string touching;
            for (const auto other : crate.overlaps()) touching += " " + other.name();
            relay::world::log("crate overlaps" + touching);
            std::string nearby;
            for (const auto other : relay::world::overlap_sphere(crate.world_position(), 1.0, 0xffffffffu, crate))
                nearby += " " + other.name();
            relay::world::log("sphere finds" + nearby);
            crate.destroy();
            relay::world::log(std::string{"crate alive until the updates finish: "} + (crate.alive() ? "yes" : "no"));
        }
    }
private:
    relay::Entity group, bullet, crate;
};
RELAY_BEHAVIOUR(Spawner)
)";

std::size_t count_logged(const relay::Engine& engine, const std::string& text) {
    std::size_t count = 0;
    for (const auto& entry : engine.logs().read_after(0))
        count += entry.message.find(text) != std::string::npos;
    return count;
}

std::size_t named(const relay::Engine& engine, const std::string& name) {
    std::size_t count = 0;
    for (const auto entity : engine.scene().entities())
        count += engine.scene().get(entity)->name == name;
    return count;
}

// Scripts spawn templates, clones and empty nodes, query overlaps, and destroy entities.
void spawning(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(write_script(protocol, "spawn/spawner.cpp", spawn_source)), "spawn scripts are written");
    expect(ok(request(protocol, "scripts.build")), "spawn scripts build");
    const auto status = wait_for_build(engine, protocol);
    expect(status.find("\"state\":\"ready\"") != std::string::npos,
           "spawn scripts compile: " + status.substr(0, 600));
    auto& scene = engine.scene();
    const auto earlier = scene.capture_state();
    expect(ok(request(protocol, "scene.clear")), "start the spawn scene empty");

    // The Bullet template: a small falling sphere running the Bullet behaviour.
    const auto prototype = scene.create("Bullet");
    relay::BoxCollider sphere;
    sphere.type = relay::BoxCollider::Type::sphere;
    sphere.radius = 0.25;
    (void)scene.set_collider(prototype, sphere);
    (void)scene.set_physics_body(prototype, relay::PhysicsBody{});
    expect(ok(add_script(protocol, prototype, "Bullet")) &&
               ok(request(protocol, "templates.save",
                          "\"entity\":\"" + prototype.to_string() + "\",\"name\":\"Bullet\"")),
           "save the Bullet template");
    (void)scene.destroy(prototype);

    const auto floor = scene.create("Floor");
    (void)scene.set_transform(floor, {{0, -0.5, 0}, {}, {1, 1, 1}});
    relay::BoxCollider ground;
    ground.half_extents = {50, 0.5, 50};
    (void)scene.set_collider(floor, ground);
    const auto pad = scene.create("Pad");
    (void)scene.set_transform(pad, {{10, 0.25, 0}, {}, {1, 1, 1}});
    relay::BoxCollider slab;
    slab.half_extents = {1, 0.25, 1};
    (void)scene.set_collider(pad, slab);
    expect(ok(add_script(protocol, pad, "Pad")), "the pad reports contacts");
    const auto crate = scene.create("Crate");
    (void)scene.set_transform(crate, {{10, 0.8, 0}, {}, {1, 1, 1}});
    relay::BoxCollider box;
    box.half_extents = {0.25, 0.25, 0.25};
    (void)scene.set_collider(crate, box);
    (void)scene.set_physics_body(crate, relay::PhysicsBody{});
    // A hinge whose motor only a script turns on.
    const auto crank = scene.create("Crank");
    (void)scene.set_transform(crank, {{-10, 3, 0}, {}, {1, 1, 1}});
    relay::BoxCollider bar;
    bar.half_extents = {1, 0.1, 0.1};
    (void)scene.set_collider(crank, bar);
    (void)scene.set_physics_body(crank, relay::PhysicsBody{});
    relay::Joint hinge;
    hinge.motor_force = 1e6;
    expect(scene.set_joint(crank, hinge) && ok(add_script(protocol, crank, "Crank")),
           "attach the crank's hinge and script");
    const auto spawner = scene.create("Spawner");
    expect(ok(add_script(protocol, spawner, "Spawner")), "attach the spawner");
    const auto authored = scene.entities().size();

    expect(engine.run_game(), "the spawning scene runs");
    expect(logged(engine, "missing template gives no entity") &&
               count_logged(engine, "instantiate Missing: project template not found") == 1U,
           "a missing template spawns nothing and is reported once");
    expect(named(engine, "Bullets") == 1U && logged(engine, "spawner children 1"),
           "world::create makes an empty child node at once");
    engine.step(2);
    relay::Entity bullet{};
    for (const auto entity : scene.entities())
        if (scene.get(entity)->name == "Bullet") bullet = entity;
    expect(bullet.valid() && logged(engine, "spawned Bullet under Bullets") &&
               scene.get(scene.get(bullet)->parent)->parent == spawner,
           "instantiate places the template under the given parent, keeping its name");
    expect(logged(engine, "bullet start after 0"), "a spawned script starts before its first update");
    engine.step(1);
    expect(named(engine, "Bullet") == 2U && logged(engine, "cloned Bullet, bullets 2"),
           "clone copies an entity beside the original with the same name");
    engine.step(16); // Frame 19: the crate has landed on the pad and is still awake.
    const auto falling = engine.physics().velocity(scene, bullet);
    expect(falling && falling->y < -1.5 && scene.get(bullet)->transform.position.y < 5.0,
           "spawned dynamic bodies fall under gravity");
    const auto before = engine.physics().raycast(scene, {10, 5, 0}, {0, -1, 0}, 10);
    expect(before.hit && before.entity == crate && logged(engine, "pad touched by Crate") &&
               !logged(engine, "pad released"),
           "the crate rests on the pad");
    engine.step(1); // Frame 20.
    std::string sphere_line;
    for (const auto& entry : engine.logs().read_after(0))
        if (entry.message.find("sphere finds") != std::string::npos) sphere_line = entry.message;
    expect(logged(engine, "crate overlaps Pad") && sphere_line.find(" Pad") != std::string::npos &&
               sphere_line.find(" Floor") != std::string::npos &&
               sphere_line.find("Crate") == std::string::npos,
           "overlaps counts a resting neighbour, and overlap_sphere finds nearby colliders "
           "except the ignored one");
    expect(logged(engine, "crate alive until the updates finish: yes") && !scene.contains(crate),
           "destroy waits for the round of updates, then removes the entity");
    const auto after = engine.physics().raycast(scene, {10, 5, 0}, {0, -1, 0}, 10);
    expect(after.hit && after.entity == pad, "a destroyed entity's body leaves the physics world");
    expect(logged(engine, "pad released a destroyed entity"),
           "destroying a touching body ends its contacts");

    engine.step(11); // Frame 31.
    std::string crank_line;
    for (const auto& entry : engine.logs().read_after(0))
        if (entry.message.find("crank turned ") != std::string::npos) crank_line = entry.message;
    const auto turned_at = crank_line.find("crank turned ");
    const auto crank_angle = turned_at == std::string::npos
        ? -1 : std::atoi(crank_line.c_str() + turned_at + 13);
    expect(logged(engine, "crank motor on") && crank_angle >= 80 && crank_angle <= 92,
           "a script drives a hinge motor and reads the joint angle: " + crank_line);
    engine.step(8); // Frame 39; both bullets reached 30 updates by frame 33.
    expect(count_logged(engine, "bullet destroyed after 30 updates") == 2U &&
               named(engine, "Bullet") == 0U && !scene.contains(bullet),
           "behaviours destroy their own entity after on_destroy");
    const auto instances_after = request(protocol, "scripts.status");
    // Pad, Crank and Spawner remain.
    expect(instances_after.find("\"instances\":3") != std::string::npos,
           "destroyed entities' script instances are removed: " + instances_after.substr(0, 300));

    expect(engine.stop_game() && scene.entities().size() == authored && scene.contains(crate) &&
               named(engine, "Bullets") == 0U,
           "Stop Game removes spawned entities and restores destroyed ones");
    scene.restore_state(earlier);
}

const char* flash_source = R"(#include "relay_script.hpp"

class Flash : public relay::Behaviour {
public:
    void on_start() override {
        const auto before = self().material_parameter("power");
        relay::world::log("power before " + std::to_string(before.size()) + " " +
                          std::to_string(static_cast<int>(before.empty() ? 0.0 : before[0])));
        self().set_material_parameter("power", 3.5);
        self().set_material_parameter("tint", relay::Vec3{1, 0, 0});
        relay::world::log(std::string("wrong count ") +
                          (self().set_material_parameter("tint", 1.0) ? "accepted" : "refused"));
        relay::world::log(std::string("missing ") +
                          (self().set_material_parameter("nothing", 1.0) ? "accepted" : "refused"));
        relay::world::log("power after " + std::to_string(self().material_parameter("power")[0]));
    }
    void on_update(double) override {
        if (++updates == 2)
            relay::world::log(std::string("cleared ") + (self().clear_material_parameter("tint") ? "yes" : "no"));
    }
private:
    int updates = 0;
};
RELAY_BEHAVIOUR(Flash)
)";

// Scripts give one object its own shader parameter values; Stop Game puts the authored ones back.
void material_parameters(relay::Engine& engine, relay::ControlProtocol& protocol) {
    const std::string shader = "shader_type surface;\nuniform vec3 tint : source_color = vec3(1.0);\n"
                               "uniform float power = 2.0;\nvoid fragment() { ALBEDO = tint * power; }\n";
    expect(ok(request(protocol, "shaders.write", "\"path\":\"Flash.relay-shader\",\"text\":" + quoted(shader))) &&
               ok(request(protocol, "assets.set_material",
                          "\"path\":\"Flash.relay-material\",\"create\":true,\"type\":\"surface\","
                          "\"shader\":\"Flash.relay-shader\"")),
           "write the flash shader and material");
    expect(ok(write_script(protocol, "flash.cpp", flash_source)) && ok(request(protocol, "scripts.build")) &&
               wait_for_build(engine, protocol).find("\"state\":\"ready\"") != std::string::npos,
           "the flash script builds");
    auto& scene = engine.scene();
    const auto earlier = scene.capture_state();
    expect(ok(request(protocol, "scene.clear")), "start the flash scene empty");
    const auto target = scene.create("Target");
    expect(scene.set_mesh_renderer(target, relay::MeshRenderer{"builtin.quad", "Flash.relay-material"}) &&
               ok(add_script(protocol, target, "Flash")),
           "a quad with the flash material and script");
    expect(engine.run_game(), "the flash scene runs");
    engine.step(3);
    const auto& own = scene.get(target)->mesh_renderer->parameters;
    expect(logged(engine, "power before 1 2") && logged(engine, "power after 3.5") &&
               logged(engine, "wrong count refused") && logged(engine, "missing refused") &&
               logged(engine, "tint takes 3 number(s)") && logged(engine, "cleared yes"),
           "scripts read the material's value, set their own, and are told about wrong names and counts");
    expect(own == std::map<std::string, std::vector<double>, std::less<>>{{"power", {3.5}}},
           "a script's values are the object's own, and clearing one returns it to the material's");
    expect(engine.stop_game() && scene.get(target)->mesh_renderer->parameters.empty(),
           "Stop Game restores the authored values");
    scene.restore_state(earlier);
}

const char* menu_source = R"(#include "relay_script.hpp"

// One script on the canvas handles every control below it.
class Menu : public relay::Behaviour {
public:
    void on_start() override {
        relay::input::set_mouse_locked(false);
        score = self().child("Score");
        relay::world::log(std::string("locked ") + (relay::input::mouse_locked() ? "yes" : "no"));
        relay::world::log("title " + self().child("Title").text());
        if (!score.set_ui("label.size", -5.0)) relay::world::log("size refused");
        if (!score.set_ui("label.nope", 1)) relay::world::log("unknown refused");
        score.set_ui("label.horizontal_align", "center");
        score.set_ui("label.color", {1.0, 0.0, 0.0, 1.0});
        const auto color = score.ui_numbers("label.color");
        relay::world::log("color " + std::to_string(color.size()) + " " + std::to_string(static_cast<int>(color[0])));
        relay::world::log("align " + score.ui_text("label.horizontal_align"));
    }
    void on_ui(const relay::ui::Event& event) override {
        if (event.clicked()) {
            ++clicks;
            score.set_text("Clicks: " + std::to_string(clicks));
            relay::world::log("clicked " + event.control.name());
        }
        if (event.toggled()) relay::world::log("toggled " + std::to_string(static_cast<int>(event.value)) +
                                               (event.control.checked() ? " checked" : " clear"));
    }
private:
    relay::Entity score;
    int clicks = 0;
};
RELAY_BEHAVIOUR(Menu)

// A button's own script hears its clicks too.
class HideOnClick : public relay::Behaviour {
public:
    void on_ui(const relay::ui::Event& event) override {
        if (event.clicked() && event.control == self()) self().set_visible(false);
    }
};
RELAY_BEHAVIOUR(HideOnClick)
)";

void game_interface(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(request(protocol, "scripts.trust", "\"trusted\":true")), "trust the project for the menu script");
    expect(ok(write_script(protocol, "menu.cpp", menu_source)) && ok(request(protocol, "scripts.build")) &&
               wait_for_build(engine, protocol).find("\"state\":\"ready\"") != std::string::npos,
           "the menu script builds");
    auto& scene = engine.scene();
    const auto earlier = scene.capture_state();
    expect(ok(request(protocol, "scene.clear")), "start the menu scene empty");
    const auto create = [&](const std::string& name, const std::string& type, relay::Entity parent) {
        return entity_from(request(protocol, "scene.create",
                                   "\"name\":\"" + name + "\",\"type\":\"" + type + "\"" +
                                       (parent.valid() ? ",\"parent\":\"" + parent.to_string() + '"' : std::string{})));
    };
    const auto hud = create("HUD", "Canvas", {});
    const auto title = create("Title", "Label", hud);
    const auto score = create("Score", "Label", hud);
    const auto play = create("Play", "Button", hud);
    const auto sound = create("Sound", "CheckBox", hud);
    expect(ok(request(protocol, "scene.set_ui", "\"entity\":\"" + title.to_string() +
                                                     R"(","component":"ui_label","values":{"text":"Main menu"})")) &&
               ok(request(protocol, "scene.set_ui", "\"entity\":\"" + sound.to_string() +
                                                        R"(","component":"ui_control","anchor_preset":"top_left")")),
           "author the menu");
    expect(ok(add_script(protocol, hud, "Menu")) && ok(add_script(protocol, play, "HideOnClick")), "attach the scripts");
    expect(engine.run_game(), "the menu runs");
    engine.step(1);
    expect(logged(engine, "locked no") && !engine.input().mouse_locked(), "scripts unlock the cursor");
    expect(logged(engine, "title Main menu") && logged(engine, "size refused") && logged(engine, "unknown refused") &&
               logged(engine, "color 4 1") && logged(engine, "align center"),
           "scripts read and write interface fields, and bad values are refused");
    std::string error;
    expect(engine.ui().simulate_click(scene, play, error), "click Play: " + error);
    engine.step(2);
    expect(logged(engine, "clicked Play") && scene.get(score)->ui.label->text == "Clicks: 1",
           "on_ui reaches the canvas script, which updates the score label");
    expect(!scene.get(play)->ui.control->visible, "the button's own script hears the click too");
    expect(engine.ui().simulate_click(scene, sound, error), "click Sound: " + error);
    engine.step(2);
    expect(logged(engine, "toggled 1 checked") && scene.get(sound)->ui.toggle->checked, "check boxes toggle and report it");
    expect(engine.stop_game() && scene.get(play)->ui.control->visible && scene.get(score)->ui.label->text == "Label" &&
               !engine.input().mouse_locked(),
           "Stop Game restores the authored interface");
    scene.restore_state(earlier);
}

const char* sparks_source = R"(#include "relay_script.hpp"

// Starts a waiting emitter, bursts from it, retunes it, and stops it after half a second.
class Sparks : public relay::Behaviour {
public:
    void on_start() override {
        const bool played = self().play_particles();
        relay::world::log(std::string("played ") + (played && self().particles_playing() ? "yes" : "no"));
        relay::world::log("burst " + std::to_string(self().emit_particles(12)));
        self().set_particles("rate", 120.0);
        self().set_particles("speed", {1.0, 2.0});
        self().set_particles("color", {1.0, 0.5, 0.0, 1.0});
        self().set_particles("blend", "additive");
        if (!self().set_particles("rate", -3.0)) relay::world::log("negative refused");
        if (!self().set_particles("sparkle", 1.0)) relay::world::log("unknown refused");
        if (!self().set_particles("speed", {1.0, 2.0, 3.0})) relay::world::log("count refused");
        const auto speed = self().particle_numbers("speed");
        relay::world::log("speed " + std::to_string(speed.size()) + " " + std::to_string(static_cast<int>(speed[1])));
    }
    void on_update(double delta) override {
        elapsed += delta;
        if (!stopped && elapsed >= 0.5) {
            stopped = true;
            relay::world::log("alive " + std::string(self().particle_count() > 12 ? "many" : "few"));
            self().stop_particles();
            relay::world::log(std::string("stopped ") + (self().particles_playing() ? "no" : "yes"));
        }
    }
private:
    double elapsed = 0.0;
    bool stopped = false;
};
RELAY_BEHAVIOUR(Sparks)
)";

void particle_scripts(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(write_script(protocol, "sparks.cpp", sparks_source)) && ok(request(protocol, "scripts.build")) &&
               wait_for_build(engine, protocol).find("\"state\":\"ready\"") != std::string::npos,
           "the particle script builds");
    auto& scene = engine.scene();
    const auto earlier = scene.capture_state();
    expect(ok(request(protocol, "scene.clear")), "start the particle scene empty");
    const auto emitter = entity_from(request(protocol, "scene.create", "\"type\":\"ParticleEmitter\""));
    expect(ok(request(protocol, "scene.set_particle_emitter",
                      "\"entity\":\"" + emitter.to_string() + R"(","values":{"play_on_start":false,"lifetime":[5,5]})")) &&
               ok(add_script(protocol, emitter, "Sparks")),
           "a waiting emitter with the sparks script");
    expect(engine.run_game(), "the particle scene runs");
    engine.step(40);
    const auto& settings = *scene.get(emitter)->particle_emitter;
    expect(logged(engine, "played yes") && logged(engine, "burst 12") && logged(engine, "negative refused") &&
               logged(engine, "unknown refused") && logged(engine, "count refused") && logged(engine, "speed 2 2"),
           "scripts play, burst and retune emitters, and bad fields and values are refused");
    expect(settings.rate == 120.0 && settings.blend == relay::ParticleEmitter::Blend::additive &&
               settings.color.g == 0.5 && logged(engine, "alive many") && logged(engine, "stopped yes"),
           "the script's settings take effect and stop ends emission");
    const auto alive = engine.particles().status(emitter)->particles;
    engine.step(10);
    expect(alive > 12U && engine.particles().status(emitter)->particles == alive,
           "stopped emitters keep their living particles");
    expect(engine.stop_game() && scene.get(emitter)->particle_emitter->rate == relay::ParticleEmitter{}.rate,
           "Stop Game restores the authored emitter");
    scene.restore_state(earlier);
}

const char* builder_source = R"(#include "relay_script.hpp"
#include <cmath>

namespace {
std::string yes(bool value, const char* on, const char* off) { return value ? on : off; }
std::string centi(double value) { return std::to_string(std::lround(value * 100.0)); }
std::string cast_text(const std::optional<relay::RayHit>& hit) {
    return hit ? hit->entity.name() + " " + centi(hit->distance) + " normal " + centi(hit->normal.y) : "missed";
}
} // namespace

class Builder : public relay::Behaviour {
public:
    void on_start() override {
        using relay::world::log;
        // A crate made from nothing: a node, a collider and a dynamic body.
        crate = relay::world::create("Built");
        crate.set_position({0, 3, 0});
        const bool built = crate.add_component("collider") &&
                           crate.set_field("collider.half_extents", relay::Vec3{0.5, 0.5, 0.5}) &&
                           crate.add_component("physics_body") && crate.set_field("physics_body.mass", 20.0);
        log("built " + yes(built, "yes", "no") + " mass " + centi(crate.field_numbers("physics_body.mass")[0]) +
            " type " + crate.field_text("physics_body.type") + " has " +
            yes(crate.has_component("physics_body"), "body", "nothing"));
        log("twice " + yes(crate.add_component("collider"), "accepted", "refused"));
        log("negative mass " + yes(crate.set_field("physics_body.mass", -1.0), "accepted", "refused"));
        log("bad choice " + yes(crate.set_field("physics_body.type", "floating"), "accepted", "refused"));
        log("wrong count " + yes(crate.set_field("collider.center", 1.0), "accepted", "refused"));
        log("unknown field " + yes(crate.set_field("collider.colour", 1.0), "accepted", "refused"));
        log("ui field " + yes(crate.set_field("ui_label.text", "hi"), "accepted", "refused"));
        log("script component " + yes(crate.add_component("script"), "accepted", "refused"));
        const bool masked = crate.set_field("collider.mask", 0xFFFFFFFDu);
        log("mask " + yes(masked, "set", "refused") + " " +
            std::to_string(static_cast<std::uint32_t>(crate.field_numbers("collider.mask")[0])));

        // Shape casts beside the crate; the floor's top is at y = 0.
        log("sphere cast " + cast_text(relay::world::sphere_cast({5, 4, 0}, 0.5, {0, -2, 0}, 10.0)));
        log("box cast " + cast_text(relay::world::shape_cast(relay::Shape::box({1, 0.25, 1}, {0, 45, 0}),
                                                               {5, 2, 0}, {0, -1, 0}, 10.0)));
        log("short cast " + cast_text(relay::world::sphere_cast({5, 4, 0}, 0.5, {0, -1, 0}, 1.0)));
        log("inside cast " + cast_text(relay::world::sphere_cast({5, 0, 0}, 0.5, {1, 0, 0}, 1.0)));
        log("ignored cast " + cast_text(relay::world::sphere_cast({5, 4, 0}, 0.5, {0, -1, 0}, 10.0, 0xffffffffu,
                                                                  relay::world::find("Floor"))));
        log("masked cast " + cast_text(relay::world::sphere_cast({5, 4, 0}, 0.5, {0, -1, 0}, 10.0, 2u)));
        log("capsule overlaps " +
            std::to_string(relay::world::overlap(relay::Shape::capsule(0.25, 1.0), {5, 0.1, 0}).size()));
        log("bad shape " + cast_text(relay::world::sphere_cast({0, 4, 0}, -1.0, {0, -1, 0}, 10.0)));

        // Scripts come and go while the game runs.
        log("add script " + yes(self().add_script("Counter"), "yes", "no"));
        log("add unknown " + yes(self().add_script("Nobody"), "yes", "no"));

        // Reparenting keeps the world pose by default.
        const auto holder = relay::world::find("Holder");
        const auto item = relay::world::find("Item");
        const bool moved = item.set_parent(holder);
        const auto world = item.world_position();
        const auto local = item.position();
        log("reparented " + yes(moved, "yes", "no") + " world " + centi(world.x) + " " + centi(world.y) + " " +
            centi(world.z) + " scale " + centi(item.scale().x) + " local " + centi(local.x) + " " +
            centi(local.y) + " " + centi(local.z));
        log("cycle " + yes(holder.set_parent(item), "accepted", "refused"));
        // Keeping the local transform instead moves it with the new parent (here, none).
        item.set_parent({}, false);
        const auto top = item.world_position();
        log("top level " + centi(top.x) + " " + centi(top.y) + " " + centi(top.z) + " parent " +
            yes(static_cast<bool>(item.parent()), "some", "none"));
    }
    void on_update(double) override {
        ++frames;
        if (frames == 90) {
            relay::world::log("crate rests at " + centi(crate.world_position().y));
            crate.set_scale({2, 2, 2}); // Its collider grows with it.
        }
        if (frames == 150) {
            relay::world::log("grown crate rests at " + centi(crate.world_position().y));
            relay::world::log("body removed " + yes(crate.remove_component("physics_body"), "yes", "no"));
        }
    }
private:
    relay::Entity crate;
    int frames = 0;
};
RELAY_BEHAVIOUR(Builder)

class Counter : public relay::Behaviour {
public:
    void on_start() override { relay::world::log("counter started"); }
    void on_update(double) override {
        if (++updates != 5) return;
        const bool first = self().remove_script("Counter");
        const bool again = self().remove_script("Counter");
        relay::world::log("counter removal " + yes(first, "queued", "refused") + " again " +
                          yes(again, "queued", "refused"));
    }
    void on_destroy() override { relay::world::log("counter removed after " + std::to_string(updates)); }
private:
    int updates = 0;
};
RELAY_BEHAVIOUR(Counter)

class Resting : public relay::Behaviour {
public:
    void on_contact_begin(relay::Entity other) override { relay::world::log("resting begin " + other.name()); }
    void on_contact_end(relay::Entity other) override { relay::world::log("resting end " + other.name()); }
    void on_update(double) override {
        // Before it sleeps: sleeping bodies end their contacts.
        if (++frames == 10)
            relay::world::log("resting retuned " + yes(self().set_field("physics_body.friction", 0.8), "yes", "no"));
    }
private:
    int frames = 0;
};
RELAY_BEHAVIOUR(Resting)
)";

// Scripts add, remove and configure components, move nodes between parents, and sweep shapes
// through the physics world; bodies rebuilt by those changes keep their contacts.
void component_scripts(relay::Engine& engine, relay::ControlProtocol& protocol) {
    expect(ok(write_script(protocol, "builder.cpp", builder_source)) && ok(request(protocol, "scripts.build")),
           "the builder script is written");
    const auto status = wait_for_build(engine, protocol);
    expect(status.find("\"state\":\"ready\"") != std::string::npos,
           "the builder script compiles: " + status.substr(0, 600));
    auto& scene = engine.scene();
    const auto earlier = scene.capture_state();
    expect(ok(request(protocol, "scene.clear")), "start the builder scene empty");
    const auto floor = scene.create("Floor");
    (void)scene.set_transform(floor, {{0, -0.5, 0}, {}, {1, 1, 1}});
    relay::BoxCollider ground;
    ground.half_extents = {50, 0.5, 50};
    (void)scene.set_collider(floor, ground);
    const auto holder = scene.create("Holder");
    (void)scene.set_transform(holder, {{10, 0, 0}, {0, 90, 0}, {2, 2, 2}});
    const auto item = scene.create("Item");
    (void)scene.set_transform(item, {{10, 0, 5}, {}, {1, 1, 1}});
    const auto resting = scene.create("Resting");
    (void)scene.set_transform(resting, {{-5, 0.5, 0}, {}, {1, 1, 1}});
    relay::BoxCollider box;
    box.half_extents = {0.5, 0.5, 0.5};
    (void)scene.set_collider(resting, box);
    (void)scene.set_physics_body(resting, relay::PhysicsBody{});
    const auto builder = scene.create("Builder");
    expect(ok(add_script(protocol, resting, "Resting")) && ok(add_script(protocol, builder, "Builder")),
           "attach the builder scripts");
    const auto authored = scene.entities().size();

    expect(engine.run_game(), "the builder scene runs");
    expect(logged(engine, "built yes mass 2000 type dynamic has body"), "scripts add and configure components");
    expect(logged(engine, "twice refused") && logged(engine, "negative mass refused") &&
               logged(engine, "bad choice refused") && logged(engine, "wrong count refused") &&
               logged(engine, "unknown field refused") && logged(engine, "ui field refused") &&
               logged(engine, "interface fields are set with set_ui") && logged(engine, "script component refused") &&
               logged(engine, "physics_body.type is one of static, dynamic"),
           "bad components and field values are refused with reasons");
    expect(logged(engine, "mask set 4294967293"), "collider masks take unsigned values");
    expect(logged(engine, "sphere cast Floor 350 normal 100") && logged(engine, "box cast Floor 175 normal 100") &&
               logged(engine, "short cast missed") && logged(engine, "inside cast Floor 0") &&
               logged(engine, "ignored cast missed") && logged(engine, "masked cast missed") &&
               logged(engine, "capsule overlaps 1") && logged(engine, "bad shape missed") &&
               logged(engine, "shape_cast: invalid shape cast"),
           "shape casts and overlaps find the floor, respect distance, ignore and masks, and refuse bad shapes");
    expect(logged(engine, "add script yes") && logged(engine, "add unknown no") &&
               logged(engine, "the scripts define no such behaviour"),
           "scripts attach defined behaviours only");
    std::string reparented;
    for (const auto& entry : engine.logs().read_after(0))
        if (entry.message.find("reparented ") != std::string::npos) reparented = entry.message;
    expect(reparented.find("reparented yes world 1000 0 500 scale 50") != std::string::npos,
           "set_parent keeps the world pose under a turned, scaled parent: " + reparented);
    expect(logged(engine, "cycle refused"), "set_parent refuses cycles");
    const auto local_at = reparented.find(" local ");
    const auto local_text = local_at == std::string::npos ? std::string{} : reparented.substr(local_at + 7U);
    expect(!local_text.empty() && logged(engine, "top level " + local_text + " parent none"),
           "set_parent can keep the local transform instead: " + local_text);

    engine.step(10);
    expect(logged(engine, "counter started") && logged(engine, "counter removal queued again refused") &&
               logged(engine, "counter removed after 5") && scene.get(builder)->scripts.size() == 1U &&
               request(protocol, "scripts.status").find("\"instances\":2") != std::string::npos,
           "an added script runs, and removing itself calls on_destroy and drops the component");
    engine.step(10); // Frame 20: the retuned body is still awake.
    expect(logged(engine, "resting retuned yes") && count_logged(engine, "resting begin Floor") == 1U &&
               !logged(engine, "resting end"),
           "retuning a resting body rebuilds it without ending or restarting its contact");
    engine.step(70); // Frame 90.
    relay::Entity built{};
    for (const auto entity : scene.entities())
        if (scene.get(entity)->name == "Built") built = entity;
    // Heights in centimetres, as the builder logs them. Jolt lets resting bodies sink up to its
    // 2 cm penetration slop.
    const auto height = [&](const std::string& text) {
        long value = -1;
        for (const auto& entry : engine.logs().read_after(0))
            if (const auto at = entry.message.find(text); at != std::string::npos)
                value = std::atol(entry.message.c_str() + at + text.size());
        return value;
    };
    const auto landed = height("] crate rests at ");
    expect(built.valid() && landed >= 47 && landed <= 50,
           "the built crate falls and lands on the floor: " + std::to_string(landed));
    engine.step(60); // Frame 150.
    const auto grown = height("grown crate rests at ");
    expect(grown >= 97 && grown <= 100, "a script's scale change rebuilds the collider: " + std::to_string(grown));
    expect(logged(engine, "body removed yes") && !engine.physics().velocity(scene, built), "a removed body stops simulating");
    const auto settled = scene.get(built)->transform.position;
    engine.step(10);
    expect(scene.get(built)->transform.position == settled, "a node without a body stays put");

    expect(engine.stop_game() && scene.entities().size() == authored && !scene.get(item)->parent.valid() &&
               scene.get(item)->transform.position.z == 5.0 && scene.get(builder)->scripts.size() == 1U,
           "Stop Game restores components, parents and scripts");
    scene.restore_state(earlier);
}

} // namespace

int main() {
    const auto original = std::filesystem::current_path();
    const auto temporary = std::filesystem::temp_directory_path() /
        ("relay-scripts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    std::filesystem::current_path(temporary);
    const auto trust_file = temporary / "trusted-script-projects";
#ifdef _WIN32
    _putenv_s("RELAY_SCRIPT_TRUST_PATH", trust_file.string().c_str());
#else
    setenv("RELAY_SCRIPT_TRUST_PATH", trust_file.c_str(), 1);
#endif
    int result = 0;
    try {
        relay::Engine engine({64, 48, 1.0 / 60.0, 0x52454c4159ULL, true});
        relay::ControlProtocol protocol(engine);
        if (!ok(request(protocol, "project.create",
                        "\"filename\":\"projects/scripts/scripts.relayproject\",\"name\":\"Scripts\"")))
            throw std::runtime_error("could not create the test project");
        untrusted_projects(engine, protocol);
        path_rules(protocol);
        lifecycle(engine, protocol);
        spawning(engine, protocol);
        material_parameters(engine, protocol);
        game_interface(engine, protocol);
        particle_scripts(engine, protocol);
        component_scripts(engine, protocol);
        compile_errors(engine, protocol);
        demo_first_person(engine, protocol);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::current_path(original);
    std::filesystem::remove_all(temporary);
    if (failures == 0) std::cout << "All Relay script tests passed\n";
    else result = 1;
    return result;
}
