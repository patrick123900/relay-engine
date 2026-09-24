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

    engine.step(19); // Frame 39; both bullets reached 30 updates by frame 33.
    expect(count_logged(engine, "bullet destroyed after 30 updates") == 2U &&
               named(engine, "Bullet") == 0U && !scene.contains(bullet),
           "behaviours destroy their own entity after on_destroy");
    const auto instances_after = request(protocol, "scripts.status");
    expect(instances_after.find("\"instances\":2") != std::string::npos,
           "destroyed entities' script instances are removed: " + instances_after.substr(0, 300));

    expect(engine.stop_game() && scene.entities().size() == authored && scene.contains(crate) &&
               named(engine, "Bullets") == 0U,
           "Stop Game removes spawned entities and restores destroyed ones");
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
