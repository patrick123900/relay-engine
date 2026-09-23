// Native gameplay scripts: trust, background builds, lifecycle, contacts, errors and hot reload.
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
        compile_errors(engine, protocol);
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
