// Relay gameplay scripting SDK. This header is the complete API available to project scripts.
//
// A script is a C++20 file in the project's scripts/ folder. Each file defines one or more
// behaviours: classes deriving from relay::Behaviour, registered with RELAY_BEHAVIOUR. Attach a
// behaviour to an entity with its Script component; during Run Game Relay creates one instance per
// scripted entity and calls its callbacks on the engine thread at the fixed 60 Hz game step.
//
//     #include "relay_script.hpp"
//
//     class Spinner : public relay::Behaviour {
//     public:
//         void on_update(double dt) override {
//             auto rotation = self().rotation();
//             rotation.y += degrees_per_second * dt;
//             self().set_rotation(rotation);
//         }
//     private:
//         double degrees_per_second = 90.0;
//     };
//     RELAY_BEHAVIOUR(Spinner)
//
// Frame order: interface events (on_ui), every behaviour's on_update, then animation and physics,
// then contact callbacks for the contacts that step produced. Run Game calls on_start on every instance before the first
// update; Stop Game calls on_stop and then restores the authored scene, so scripts may change the
// scene freely during a run.
//
// Properties: override properties() to expose fields in the editor's Inspector. Each node stores
// the values people or agents set there; fields left alone keep the default written in code, so
// changing a default in code updates every node that has not overridden it. Relay assigns stored
// values after construction and before on_start. Supported field types: bool, int, float, double,
// relay::Vec3 and std::string.
//
//     class Mover : public relay::Behaviour {
//     public:
//         void properties(relay::Properties& p) override { p.add("speed", speed); }
//         void on_update(double dt) override { ... speed ... }
//     private:
//         double speed = 2.0;
//     };
//
// Input: relay::input reads the project's input map (Game Configuration > Input in the editor).
// Actions are buttons such as "jump"; axes are values in [-1, 1] such as "move_x". Input is
// latched once per game step, so pressed() is true for exactly one on_update per press.
//
//     if (relay::input::pressed("jump")) self().apply_impulse({0, 5, 0});
//     const auto move = relay::input::vector("move_x", "move_y"); // length at most 1
//
// Spawning: world::instantiate copies a project template (a prefab saved from the editor),
// Entity::clone copies an entity, and world::create makes an empty node. New entities exist at
// once, with physics bodies, and their scripts' on_start runs before their first on_update.
// Entity::destroy removes an entity and its descendants once the current round of callbacks
// finishes (after every on_update, or after contact callbacks), calling on_destroy first, so a
// behaviour may destroy its own entity. Stop Game removes everything the run spawned.
//
//     void on_update(double) override {
//         if (relay::input::pressed("fire"))
//             relay::world::instantiate("Bullet", self().world_position() + relay::Vec3{0, 1, 0});
//     }
//     void on_contact_begin(relay::Entity) override { self().destroy(); }
//
// Components: scripts add, remove and configure a node's components while the game runs, and move
// nodes between parents. Fields are named "<component>.<field>" as scenes store them. Changes to
// colliders, physics bodies and joints rebuild the node's physics body at once, keeping its velocity.
//
//     auto crate = relay::world::create("Crate");
//     crate.add_component("collider");
//     crate.set_field("collider.half_extents", relay::Vec3{0.5, 0.5, 0.5});
//     crate.add_component("physics_body");
//     crate.set_field("physics_body.mass", 20.0);
//     held.set_parent(self());          // Carry it, keeping its place in the world.
//
// Shape casts sweep a sphere, box or capsule through the physics world, for ground checks,
// melee hits and cover tests that a thin ray would miss:
//
//     if (auto hit = relay::world::sphere_cast(origin, 0.3, {0, -1, 0}, 1.0, ~0u, self()))
//         relay::world::log("standing on " + hit->entity.name());
//
// Interface: Canvas, Control, Label, Button and the other UI nodes draw over the game's view
// during Run Game. Scripts change them through Entity (set_text, set_visible, set_value, set_ui for
// any field) and hear clicks, toggles and slider moves through on_ui, which reaches the control's
// own scripts and every ancestor's, so one script on a menu can handle all of its buttons.
// Controls react to the pointer only while the cursor is unlocked: call
// relay::input::set_mouse_locked(false) when a menu opens and true when it closes.
//
//     void on_ui(const relay::ui::Event& event) override {
//         if (event.clicked() && event.control.name() == "Resume") close_menu();
//     }
//
// Hot reload: when scripts are rebuilt during Run Game, each instance is destroyed without on_stop
// and recreated from the new code, then on_reload runs (by default it calls on_start). Member
// variables do not survive a reload; state kept in the scene (transforms, velocities) does.
//
// Errors: an exception escaping a callback disables that one instance and is reported by
// scripts.status and the editor. Scripts are native code with the editor's full privileges; a
// crash or an endless loop takes the editor down with it.
#pragma once

#include "relay_script_abi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

namespace detail {
inline const RelayHostApi*& host() {
    static const RelayHostApi* api = nullptr;
    return api;
}
// Runs a query that fills a buffer and reports the full count, growing the buffer until it fits.
template <class Query>
std::vector<RelayEntity> collect(Query query) {
    std::vector<RelayEntity> handles(16);
    for (;;) {
        const size_t total = query(handles.data(), handles.size());
        const bool fits = total <= handles.size();
        handles.resize(total);
        if (fits) return handles;
    }
}
} // namespace detail

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
    friend constexpr Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    friend constexpr Vec3 operator*(double s, Vec3 a) { return a * s; }
    friend constexpr Vec3 operator/(Vec3 a, double s) { return {a.x / s, a.y / s, a.z / s}; }
    constexpr Vec3& operator+=(Vec3 b) { return *this = *this + b; }
    constexpr Vec3& operator-=(Vec3 b) { return *this = *this - b; }
    constexpr Vec3& operator*=(double s) { return *this = *this * s; }
    friend constexpr bool operator==(Vec3, Vec3) = default;

    [[nodiscard]] double length() const { return std::sqrt(dot(*this, *this)); }
    // Returns zero for a zero-length vector.
    [[nodiscard]] Vec3 normalized() const {
        const double size = length();
        return size > 0.0 ? *this / size : Vec3{};
    }
    [[nodiscard]] static constexpr double dot(Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    [[nodiscard]] static constexpr Vec3 cross(Vec3 a, Vec3 b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
};

// A generation-checked scene entity. Handles to destroyed entities stay invalid rather than
// aliasing newer ones. Relay's world is Y-up; rotations are Euler degrees applied X, then Y, then Z.
namespace audio {
// When a music change happens: at once, or on the current track's next beat, bar or ending.
// `player` uses the music player's own setting.
enum class Sync { player = -1, immediate = 0, beat = 1, bar = 2, track_end = 3 };
} // namespace audio

class Entity {
public:
    constexpr Entity() = default;
    constexpr explicit Entity(RelayEntity handle) : handle_(handle) {}

    [[nodiscard]] constexpr RelayEntity handle() const { return handle_; }
    [[nodiscard]] constexpr explicit operator bool() const { return handle_ != 0; }
    friend constexpr bool operator==(Entity, Entity) = default;

    [[nodiscard]] bool alive() const { return api().alive(api().context, handle_) != 0; }
    [[nodiscard]] std::string name() const {
        char buffer[256];
        const auto length = api().name(api().context, handle_, buffer, sizeof buffer);
        return std::string(buffer, std::min(length, sizeof buffer));
    }
    [[nodiscard]] Entity parent() const { return Entity{api().parent(api().context, handle_)}; }
    // The first direct child with this exact name, or an empty Entity.
    [[nodiscard]] Entity child(std::string_view name) const {
        return Entity{api().child(api().context, handle_, name.data(), name.size())};
    }
    // Direct children, in scene order.
    [[nodiscard]] std::vector<Entity> children() const {
        return wrap(detail::collect([this](RelayEntity* out, size_t capacity) {
            return api().children(api().context, handle_, out, capacity);
        }));
    }
    // Renders the game through this entity's camera until Stop Game. False without a camera.
    bool make_active_camera() const { return api().activate_camera(api().context, handle_) != 0; }

    // A copy of this entity and its descendants beside it, with the same name, components and
    // script values. Physics velocities start at zero. Empty if it fails.
    [[nodiscard]] Entity clone() const { return Entity{api().clone(api().context, handle_)}; }
    // Removes this entity and its descendants once the current callbacks finish, after their
    // on_destroy. Handles to them then stop being alive(). False if it is already gone.
    bool destroy() const { return api().destroy(api().context, handle_) != 0; }

    // This entity's joint while the game runs: a hinge's angle in degrees, a slider's travel in
    // metres from where it started, or a distance joint's length. Hinge angles and slider travel
    // follow the right-hand rule about the joint's axis. Nothing without such a joint.
    [[nodiscard]] std::optional<double> joint_position() const {
        double value{};
        if (!api().joint_position(api().context, handle_, &value)) return std::nullopt;
        return value;
    }
    // Drives this entity's hinge (degrees per second) or slider (metres per second) joint with its
    // motor, up to the motor force set in the editor; positive speeds follow the right-hand rule
    // about the axis. False without a hinge or slider joint.
    bool set_joint_motor(double speed) const {
        return api().set_joint_motor(api().context, handle_, 1, speed) != 0;
    }
    bool stop_joint_motor() const { return api().set_joint_motor(api().context, handle_, 0, 0.0) != 0; }

    // Plays this entity's audio source from the start, as set up in the Inspector, restarting it
    // if it is already playing. `volume_db` is added to the authored volume and `pitch` multiplies
    // the authored pitch, for this playing only: vary them so repeated sounds do not all match.
    // False without an audio source and clip.
    bool play_sound(double volume_db = 0.0, double pitch = 1.0) const {
        return api().audio_play(api().context, handle_, volume_db, pitch) != 0;
    }
    // Stops this entity's sound. False if nothing was playing.
    bool stop_sound() const { return api().audio_stop(api().context, handle_) != 0; }
    [[nodiscard]] bool sound_playing() const { return api().audio_playing(api().context, handle_) != 0; }
    // Seconds into this entity's playing sound; nothing when it is not playing.
    [[nodiscard]] std::optional<double> sound_position() const {
        const double seconds = api().audio_position(api().context, handle_);
        if (seconds < 0.0) return std::nullopt;
        return seconds;
    }

    // This entity's music player: play a playlist track (-1 for the next one), crossfading from
    // the current track at the sync point; stop with a fade; and which track is playing (-1 for
    // none). Music plays during the game.
    bool play_music(int track = -1, audio::Sync sync = audio::Sync::player) const {
        return api().music_play(api().context, handle_, track, static_cast<int>(sync)) != 0;
    }
    bool stop_music(double fade_seconds = 1.0) const {
        return api().music_stop(api().context, handle_, fade_seconds) != 0;
    }
    [[nodiscard]] int music_track() const { return api().music_track(api().context, handle_); }

    // This object's own value for a parameter of its shader material (a uniform in the material's
    // .relay-shader), without changing the material file or other objects that use it: make one
    // enemy flash, fade a door out, pulse a light. Colors are linear RGB. The Inspector shows and
    // edits the same per-object values; Stop Game restores the authored ones. False when the
    // entity has no shader material, or the shader has no such number, vector or bool uniform.
    //
    //     self().set_material_parameter("flash", 1.0);
    //     self().set_material_parameter("tint", relay::Vec3{1.0, 0.1, 0.1});
    bool set_material_parameter(std::string_view name, double value) const {
        return api().set_material_parameter(api().context, handle_, name.data(), name.size(), &value, 1U) != 0;
    }
    bool set_material_parameter(std::string_view name, Vec3 value) const {
        const double values[3] = {value.x, value.y, value.z};
        return api().set_material_parameter(api().context, handle_, name.data(), name.size(), values, 3U) != 0;
    }
    // For vec2, vec4 or any uniform: its numbers in order.
    bool set_material_parameter(std::string_view name, std::initializer_list<double> values) const {
        return api().set_material_parameter(api().context, handle_, name.data(), name.size(), values.begin(),
                                            values.size()) != 0;
    }
    // The parameter's current numbers (this object's own value, else the material's); empty when
    // there is no such parameter.
    [[nodiscard]] std::vector<double> material_parameter(std::string_view name) const {
        double values[16];
        const size_t count =
            api().get_material_parameter(api().context, handle_, name.data(), name.size(), values, 16U);
        return std::vector<double>(values, values + std::min<size_t>(count, 16U));
    }
    // Goes back to the material's own value.
    bool clear_material_parameter(std::string_view name) const {
        return api().clear_material_parameter(api().context, handle_, name.data(), name.size()) != 0;
    }

    // Interface fields, named "<component>.<field>" as the Inspector shows them: "label.text",
    // "control.visible", "panel.color", "slider.value", "control.offset_min". Colors are sRGB red,
    // green, blue and alpha from 0 to 1; vectors are two numbers; choices are their names. Changes
    // last until Stop Game. False, with a log message, when the entity lacks the component or the
    // value does not fit the field.
    //
    //     self().set_ui("panel.color", {1.0, 0.2, 0.2, 0.9});
    //     title.set_ui("label.horizontal_align", "center");
    bool set_ui(std::string_view field, double value) const { return set_ui(field, {value}); }
    bool set_ui(std::string_view field, int value) const { return set_ui(field, {static_cast<double>(value)}); }
    bool set_ui(std::string_view field, bool value) const { return set_ui(field, {value ? 1.0 : 0.0}); }
    bool set_ui(std::string_view field, std::initializer_list<double> values) const {
        return api().ui_set_numbers(api().context, handle_, field.data(), field.size(), values.begin(),
                                    values.size()) != 0;
    }
    bool set_ui(std::string_view field, std::string_view text) const {
        return api().ui_set_text(api().context, handle_, field.data(), field.size(), text.data(), text.size()) != 0;
    }
    bool set_ui(std::string_view field, const char* text) const { return set_ui(field, std::string_view{text}); }
    // A field's numbers (booleans read 0 or 1); empty without the component or for text fields.
    [[nodiscard]] std::vector<double> ui_numbers(std::string_view field) const {
        double values[4];
        const size_t count = api().ui_get_numbers(api().context, handle_, field.data(), field.size(), values, 4U);
        return std::vector<double>(values, values + std::min<size_t>(count, 4U));
    }
    [[nodiscard]] std::string ui_text(std::string_view field) const {
        std::string text(api().ui_get_text(api().context, handle_, field.data(), field.size(), nullptr, 0U), '\0');
        if (!text.empty())
            (void)api().ui_get_text(api().context, handle_, field.data(), field.size(), text.data(), text.size());
        return text;
    }
    // Shortcuts for the fields scripts change most.
    bool set_text(std::string_view text) const { return set_ui("label.text", text); }
    [[nodiscard]] std::string text() const { return ui_text("label.text"); }
    bool set_visible(bool visible) const { return set_ui("control.visible", visible); }
    [[nodiscard]] bool visible() const { return first("control.visible") != 0.0; }
    // A slider's or progress bar's value, kept within its range.
    bool set_value(double value) const {
        return set_ui(has_ui("slider.value") ? "slider.value" : "progress_bar.value", value);
    }
    [[nodiscard]] double value() const {
        return has_ui("slider.value") ? first("slider.value") : first("progress_bar.value");
    }
    // A check box's state, or a toggle button's.
    bool set_checked(bool checked) const {
        return set_ui(has_ui("toggle.checked") ? "toggle.checked" : "button.pressed", checked);
    }
    [[nodiscard]] bool checked() const {
        return (has_ui("toggle.checked") ? first("toggle.checked") : first("button.pressed")) != 0.0;
    }
    // Whether the pointer is over this button, check box or slider, or holding it down, this step.
    [[nodiscard]] bool hovered() const { return (api().ui_state(api().context, handle_) & RELAY_UI_HOVERED) != 0; }
    [[nodiscard]] bool held() const { return (api().ui_state(api().context, handle_) & RELAY_UI_HELD) != 0; }

    // The entity's particle emitter. play_particles starts its cycle (restart also clears the
    // particles alive); stop_particles ends emission, letting the particles finish unless `clear`;
    // emit_particles adds a burst now, from the emitter's shape, and returns how many were born.
    //
    //     explosion.emit_particles(80);
    //     self().stop_particles();
    bool play_particles(bool restart = false) const {
        return api().particles_play(api().context, handle_, restart ? 1 : 0) != 0;
    }
    bool stop_particles(bool clear = false) const {
        return api().particles_stop(api().context, handle_, clear ? 1 : 0) != 0;
    }
    bool pause_particles(bool paused = true) const {
        return api().particles_pause(api().context, handle_, paused ? 1 : 0) != 0;
    }
    size_t emit_particles(size_t count) const { return api().particles_emit(api().context, handle_, count); }
    [[nodiscard]] size_t particle_count() const { return api().particles_count(api().context, handle_); }
    [[nodiscard]] bool particles_playing() const { return api().particles_playing(api().context, handle_) != 0; }
    // Emitter fields by the names the Inspector shows: "rate", "gravity", "color" (sRGB red, green,
    // blue, alpha), "speed" and other ranges (min, max), "acceleration" and other vectors, and
    // choices or paths as text. Changes last until Stop Game.
    //
    //     torch.set_particles("rate", 80.0);
    //     torch.set_particles("color", {1.0, 0.4, 0.1, 1.0});
    //     torch.set_particles("blend", "additive");
    bool set_particles(std::string_view field, double value) const { return set_particles(field, {value}); }
    bool set_particles(std::string_view field, int value) const {
        return set_particles(field, {static_cast<double>(value)});
    }
    bool set_particles(std::string_view field, bool value) const { return set_particles(field, {value ? 1.0 : 0.0}); }
    bool set_particles(std::string_view field, Vec3 value) const {
        return set_particles(field, {value.x, value.y, value.z});
    }
    bool set_particles(std::string_view field, std::initializer_list<double> values) const {
        return api().particles_set_numbers(api().context, handle_, field.data(), field.size(), values.begin(),
                                           values.size()) != 0;
    }
    bool set_particles(std::string_view field, std::string_view text) const {
        return api().particles_set_text(api().context, handle_, field.data(), field.size(), text.data(),
                                        text.size()) != 0;
    }
    bool set_particles(std::string_view field, const char* text) const {
        return set_particles(field, std::string_view{text});
    }
    // A field's numbers (booleans read 0 or 1); empty without an emitter or for text fields.
    [[nodiscard]] std::vector<double> particle_numbers(std::string_view field) const {
        double values[4];
        const size_t count =
            api().particles_get_numbers(api().context, handle_, field.data(), field.size(), values, 4U);
        return std::vector<double>(values, values + std::min<size_t>(count, 4U));
    }

    // Moves this entity under `parent` (an empty Entity for the top level). By default it keeps
    // its place, turn and size in the world; with keep_world false it keeps its local transform
    // and moves with the new parent. False if the parent is this entity or below it.
    bool set_parent(Entity parent, bool keep_world = true) const {
        return api().set_parent(api().context, handle_, parent.handle(), keep_world ? 1 : 0) != 0;
    }

    // Engine components by id, as the Add Component window lists them: "collider",
    // "physics_body", "joint", "light", "camera", "mesh_renderer", "audio_source",
    // "particle_emitter", "ui_label" and so on. Added components start with the editor's defaults.
    // Stop Game restores the authored components. False, with a log message, when the component
    // cannot be added or removed.
    [[nodiscard]] bool has_component(std::string_view id) const {
        return api().has_component(api().context, handle_, id.data(), id.size()) != 0;
    }
    bool add_component(std::string_view id) const {
        return api().add_component(api().context, handle_, id.data(), id.size()) != 0;
    }
    bool remove_component(std::string_view id) const {
        return api().remove_component(api().context, handle_, id.data(), id.size()) != 0;
    }
    // Attaches a behaviour with its code defaults; it starts before its first update.
    bool add_script(std::string_view behaviour) const {
        return api().add_script(api().context, handle_, behaviour.data(), behaviour.size()) != 0;
    }
    // Removes the first script component running `behaviour` once the current callbacks finish,
    // after its on_destroy, so a behaviour may remove itself.
    bool remove_script(std::string_view behaviour) const {
        return api().remove_script(api().context, handle_, behaviour.data(), behaviour.size()) != 0;
    }

    // Component fields, named "<component>.<field>": "collider.radius", "collider.layer",
    // "physics_body.mass", "physics_body.type" ("static" or "dynamic"), "light.color",
    // "light.intensity", "camera.field_of_view_y_degrees", "mesh_renderer.material",
    // "joint.limit_max", "audio_source.volume_db", "keyframes.playing". Choices are text. Interface
    // controls use set_ui and particle emitters set_particles. False, with a log message, when
    // the entity lacks the component or the value does not fit.
    bool set_field(std::string_view field, double value) const {
        return api().component_set_numbers(api().context, handle_, field.data(), field.size(), &value, 1U) != 0;
    }
    bool set_field(std::string_view field, int value) const { return set_field(field, static_cast<double>(value)); }
    bool set_field(std::string_view field, std::uint32_t value) const {
        return set_field(field, static_cast<double>(value));
    }
    bool set_field(std::string_view field, bool value) const { return set_field(field, value ? 1.0 : 0.0); }
    bool set_field(std::string_view field, Vec3 value) const {
        const double values[3] = {value.x, value.y, value.z};
        return api().component_set_numbers(api().context, handle_, field.data(), field.size(), values, 3U) != 0;
    }
    bool set_field(std::string_view field, std::string_view text) const {
        return api().component_set_text(api().context, handle_, field.data(), field.size(), text.data(),
                                        text.size()) != 0;
    }
    bool set_field(std::string_view field, const char* text) const { return set_field(field, std::string_view{text}); }
    // A field's numbers (booleans read 0 or 1, vectors three); empty without the component or for
    // text and choices.
    [[nodiscard]] std::vector<double> field_numbers(std::string_view field) const {
        double values[3];
        const size_t count =
            api().component_get_numbers(api().context, handle_, field.data(), field.size(), values, 3U);
        return std::vector<double>(values, values + std::min<size_t>(count, 3U));
    }
    [[nodiscard]] std::string field_text(std::string_view field) const {
        std::string text(api().component_get_text(api().context, handle_, field.data(), field.size(), nullptr, 0U),
                         '\0');
        if (!text.empty())
            (void)api().component_get_text(api().context, handle_, field.data(), field.size(), text.data(),
                                           text.size());
        return text;
    }
    // Connects this entity's joint to another entity's physics body, or to the world with an empty
    // Entity, from the joint's current pose. False without a joint.
    bool connect_joint(Entity other) const {
        return api().joint_connect(api().context, handle_, other.handle()) != 0;
    }

    // Enabled colliders touching this entity's enabled collider, sorted. Each side's layer must
    // be in the other's mask.
    [[nodiscard]] std::vector<Entity> overlaps() const {
        return wrap(detail::collect([this](RelayEntity* out, size_t capacity) {
            return api().overlaps(api().context, handle_, out, capacity);
        }));
    }

    // Local transform, relative to the parent.
    [[nodiscard]] Vec3 position() const { return read(api().get_position); }
    bool set_position(Vec3 value) const { return write(api().set_position, value); }
    [[nodiscard]] Vec3 rotation() const { return read(api().get_rotation); }
    bool set_rotation(Vec3 euler_degrees) const { return write(api().set_rotation, euler_degrees); }
    [[nodiscard]] Vec3 scale() const { return read(api().get_scale); }
    bool set_scale(Vec3 value) const { return write(api().set_scale, value); }
    [[nodiscard]] Vec3 world_position() const { return read(api().world_position); }

    // Dynamic physics bodies only. Velocities are world-space, angular velocity in radians/second.
    [[nodiscard]] Vec3 velocity() const { return read(api().get_velocity); }
    bool set_velocity(Vec3 value) const { return write(api().set_velocity, value); }
    [[nodiscard]] Vec3 angular_velocity() const { return read(api().get_angular_velocity); }
    bool set_angular_velocity(Vec3 value) const {
        return write(api().set_angular_velocity, value);
    }
    // World-space impulse; a world-space point off the centre of mass also adds spin.
    bool apply_impulse(Vec3 impulse) const {
        return api().apply_impulse(api().context, handle_, raw(impulse), nullptr) != 0;
    }
    bool apply_impulse(Vec3 impulse, Vec3 world_point) const {
        const auto point = raw(world_point);
        return api().apply_impulse(api().context, handle_, raw(impulse), &point) != 0;
    }

private:
    static const RelayHostApi& api() { return *detail::host(); }
    static RelayVec3 raw(Vec3 value) { return {value.x, value.y, value.z}; }
    Vec3 read(int (*getter)(void*, RelayEntity, RelayVec3*)) const {
        RelayVec3 value{};
        (void)getter(api().context, handle_, &value);
        return {value.x, value.y, value.z};
    }
    bool write(int (*setter)(void*, RelayEntity, RelayVec3), Vec3 value) const {
        return setter(api().context, handle_, raw(value)) != 0;
    }

    static std::vector<Entity> wrap(const std::vector<RelayEntity>& handles) {
        return {handles.begin(), handles.end()};
    }
    bool has_ui(std::string_view field) const {
        double value{};
        return api().ui_get_numbers(api().context, handle_, field.data(), field.size(), &value, 1U) != 0U;
    }
    double first(std::string_view field) const {
        double value{};
        return api().ui_get_numbers(api().context, handle_, field.data(), field.size(), &value, 1U) != 0U ? value : 0.0;
    }

    RelayEntity handle_{0};
};

namespace ui {
enum class EventType {
    clicked = RELAY_UI_CLICKED,             // Pressed and released over a button, check box or slider.
    toggled = RELAY_UI_TOGGLED,             // A check box or toggle button flipped; value 1 on, 0 off.
    value_changed = RELAY_UI_VALUE_CHANGED, // A slider moved; value is its new value.
    pressed = RELAY_UI_PRESSED,
    released = RELAY_UI_RELEASED,
};
struct Event {
    EventType type{};
    Entity control; // The button, check box or slider.
    double value{};
    [[nodiscard]] bool clicked() const { return type == EventType::clicked; }
    [[nodiscard]] bool toggled() const { return type == EventType::toggled; }
    [[nodiscard]] bool value_changed() const { return type == EventType::value_changed; }
};
} // namespace ui

// A ray or shape cast's first hit. For shape casts `distance` is how far the shape's centre
// travelled, `point` where it touched and `normal` the touched surface's, facing the shape.
struct RayHit {
    Entity entity;
    double distance{};
    Vec3 point;
    Vec3 normal;
};

// A sphere, box or capsule for shape casts and overlaps. Capsules stand along their local Y axis;
// rotations are Euler degrees, like transforms.
struct Shape {
    static Shape sphere(double radius) { return {{RELAY_SHAPE_SPHERE, radius, 0.0, {}, {}}}; }
    static Shape box(Vec3 half_extents, Vec3 rotation = {}) {
        return {{RELAY_SHAPE_BOX, 0.0, 0.0, {half_extents.x, half_extents.y, half_extents.z},
                 {rotation.x, rotation.y, rotation.z}}};
    }
    // `half_height` is half the straight part, between the round ends.
    static Shape capsule(double radius, double half_height, Vec3 rotation = {}) {
        return {{RELAY_SHAPE_CAPSULE, radius, half_height, {}, {rotation.x, rotation.y, rotation.z}}};
    }
    RelayShape raw{};
};

// Scene-wide queries and services.
namespace world {
inline const RelayHostApi& api() { return *detail::host(); }
// Seconds of game time since Run Game, advancing by exactly 1/60 per frame.
inline double time() { return api().time(api().context); }
inline std::uint64_t frame() { return api().frame(api().context); }
// First entity with this exact name, or an empty Entity. It scans the scene, so look entities up
// once in on_start rather than every frame.
inline Entity find(std::string_view name) {
    return Entity{api().find(api().context, name.data(), name.size())};
}
// Nearest enabled collider hit by a ray against the live physics world. `direction` need not be
// normalized; layer_mask filters on collider layers. A ray starting inside a collider hits it at
// distance zero, so pass the caller's own entity as `ignore` when casting from inside it.
inline std::optional<RayHit> raycast(Vec3 origin, Vec3 direction, double maximum_distance,
                                     std::uint32_t layer_mask = 0xffffffffu, Entity ignore = {}) {
    RelayRayHit hit{};
    if (!api().raycast(api().context, {origin.x, origin.y, origin.z},
                       {direction.x, direction.y, direction.z}, maximum_distance, layer_mask,
                       ignore.handle(), &hit))
        return std::nullopt;
    return RayHit{Entity{hit.entity}, hit.distance, {hit.point.x, hit.point.y, hit.point.z},
                  {hit.normal.x, hit.normal.y, hit.normal.z}};
}
// Enabled colliders on `layer_mask` layers that overlap a world-space sphere, sorted.
inline std::vector<Entity> overlap_sphere(Vec3 center, double radius,
                                          std::uint32_t layer_mask = 0xffffffffu,
                                          Entity ignore = {}) {
    const auto handles = detail::collect([&](RelayEntity* out, size_t capacity) {
        return api().overlap_sphere(api().context, {center.x, center.y, center.z}, radius,
                                    layer_mask, ignore.handle(), out, capacity);
    });
    return {handles.begin(), handles.end()};
}
// Sweeps `shape` from `origin` along `direction` (which need not be normalized) and returns the
// first enabled collider on `layer_mask` layers that it touches. A shape that starts touching a
// collider hits it at distance zero, so ignore the caller's own collider when casting from it.
inline std::optional<RayHit> shape_cast(const Shape& shape, Vec3 origin, Vec3 direction,
                                        double maximum_distance, std::uint32_t layer_mask = 0xffffffffu,
                                        Entity ignore = {}) {
    RelayRayHit hit{};
    if (!api().shape_cast(api().context, &shape.raw, {origin.x, origin.y, origin.z},
                          {direction.x, direction.y, direction.z}, maximum_distance, layer_mask,
                          ignore.handle(), &hit))
        return std::nullopt;
    return RayHit{Entity{hit.entity}, hit.distance, {hit.point.x, hit.point.y, hit.point.z},
                  {hit.normal.x, hit.normal.y, hit.normal.z}};
}
inline std::optional<RayHit> sphere_cast(Vec3 origin, double radius, Vec3 direction, double maximum_distance,
                                         std::uint32_t layer_mask = 0xffffffffu, Entity ignore = {}) {
    return shape_cast(Shape::sphere(radius), origin, direction, maximum_distance, layer_mask, ignore);
}
// Enabled colliders on `layer_mask` layers that overlap `shape` centred at `center`, sorted.
inline std::vector<Entity> overlap(const Shape& shape, Vec3 center, std::uint32_t layer_mask = 0xffffffffu,
                                   Entity ignore = {}) {
    const auto handles = detail::collect([&](RelayEntity* out, size_t capacity) {
        return api().overlap_shape(api().context, &shape.raw, {center.x, center.y, center.z}, layer_mask,
                                   ignore.handle(), out, capacity);
    });
    return {handles.begin(), handles.end()};
}
// An empty node (just a Transform), a child of `parent` when given. Empty if it fails.
inline Entity create(std::string_view name, Entity parent = {}) {
    return Entity{api().create_entity(api().context, name.data(), name.size(), parent.handle())};
}
// A copy of the project template templates/<name>.relay-template.json, as saved from the editor's
// "Save as template...". It keeps the template's saved transform, relative to `parent` when given.
// Empty if the template is missing or invalid; the log says why.
inline Entity instantiate(std::string_view template_name, Entity parent = {}) {
    return Entity{api().instantiate(api().context, template_name.data(), template_name.size(),
                                    parent.handle(), nullptr, nullptr)};
}
// As above, placed at `position` (and turned to `rotation`, Euler degrees, when given), relative
// to `parent` when given.
inline Entity instantiate(std::string_view template_name, Vec3 position,
                          std::optional<Vec3> rotation = std::nullopt, Entity parent = {}) {
    const RelayVec3 at{position.x, position.y, position.z};
    RelayVec3 turn{};
    if (rotation) turn = {rotation->x, rotation->y, rotation->z};
    return Entity{api().instantiate(api().context, template_name.data(), template_name.size(),
                                    parent.handle(), &at, rotation ? &turn : nullptr)};
}
// Messages appear in the editor log, prefixed with the calling behaviour and entity.
inline void log(std::string_view text) {
    api().log(api().context, RELAY_LOG_INFO, text.data(), text.size());
}
inline void warn(std::string_view text) {
    api().log(api().context, RELAY_LOG_WARNING, text.data(), text.size());
}
inline void error(std::string_view text) {
    api().log(api().context, RELAY_LOG_ERROR, text.data(), text.size());
}
} // namespace world

namespace audio {

// A one-shot sound playing: stop it, or ask whether and where it plays. Empty when it failed.
class Sound {
public:
    constexpr Sound() = default;
    constexpr explicit Sound(std::uint64_t handle) : handle_(handle) {}
    [[nodiscard]] constexpr explicit operator bool() const { return handle_ != 0; }
    [[nodiscard]] constexpr std::uint64_t handle() const { return handle_; }
    bool stop() const { return api().audio_sound_stop(api().context, handle_) != 0; }
    [[nodiscard]] bool playing() const { return api().audio_sound_playing(api().context, handle_) != 0; }
    // Seconds into the sound; nothing once it has finished.
    [[nodiscard]] std::optional<double> position() const {
        const double seconds = api().audio_sound_position(api().context, handle_);
        if (seconds < 0.0) return std::nullopt;
        return seconds;
    }

private:
    static const RelayHostApi& api() { return *detail::host(); }
    std::uint64_t handle_{0};
};

struct OneShot {
    double volume_db = 0.0;
    double pitch = 1.0;
    std::string bus = "SFX";
    double min_distance = 1.0; // Placed sounds: full volume inside, silent beyond max_distance.
    double max_distance = 50.0;
};

namespace detail_audio {
inline Sound play(std::string_view clip, const RelayVec3* position, const OneShot& options) {
    const auto& host = *detail::host();
    return Sound{host.audio_play_clip(host.context, clip.data(), clip.size(), position,
                                      options.volume_db, options.pitch, options.bus.data(),
                                      options.bus.size(), options.min_distance, options.max_distance)};
}
} // namespace detail_audio

// Plays a project sound file once at a place in the world, like an explosion or a footstep.
inline Sound play(std::string_view clip, Vec3 position, const OneShot& options = {}) {
    const RelayVec3 at{position.x, position.y, position.z};
    return detail_audio::play(clip, &at, options);
}
// Plays a project sound file once, flat: interface sounds and stingers.
inline Sound play_flat(std::string_view clip, const OneShot& options = {}) {
    return detail_audio::play(clip, nullptr, options);
}

// Game-time mixer changes, such as ducking music in a menu. Stop Game undoes them; the Mixer
// panel changes the saved mixer.
inline bool set_bus_volume(std::string_view bus, double volume_db, double fade_seconds = 0.0) {
    const auto& host = *detail::host();
    return host.audio_bus_volume(host.context, bus.data(), bus.size(), volume_db, fade_seconds) != 0;
}
inline std::optional<double> bus_volume(std::string_view bus) {
    const auto& host = *detail::host();
    const double value = host.audio_get_bus_volume(host.context, bus.data(), bus.size());
    if (value < -999.0) return std::nullopt;
    return value;
}
inline bool set_bus_mute(std::string_view bus, bool mute) {
    const auto& host = *detail::host();
    return host.audio_bus_mute(host.context, bus.data(), bus.size(), mute ? 1 : 0) != 0;
}
// Sets one parameter of the effect at `index` in the bus's chain, by its name in the Mixer
// (for example "mix", "cutoff_hz" or "room_size").
inline bool set_bus_effect(std::string_view bus, std::uint32_t index, std::string_view parameter,
                           double value) {
    const auto& host = *detail::host();
    return host.audio_bus_effect(host.context, bus.data(), bus.size(), index, parameter.data(),
                                 parameter.size(), value) != 0;
}

} // namespace audio

// Collects a behaviour's editable fields. Names must be C++ identifiers and unique per behaviour.
class Properties {
public:
    void add(std::string_view name, bool& value) {
        push(name, RELAY_PROPERTY_BOOLEAN, Storage::boolean, &value);
    }
    void add(std::string_view name, int& value) {
        push(name, RELAY_PROPERTY_NUMBER, Storage::integer, &value);
    }
    void add(std::string_view name, float& value) {
        push(name, RELAY_PROPERTY_NUMBER, Storage::single, &value);
    }
    void add(std::string_view name, double& value) {
        push(name, RELAY_PROPERTY_NUMBER, Storage::number, &value);
    }
    void add(std::string_view name, Vec3& value) {
        push(name, RELAY_PROPERTY_VECTOR, Storage::vector, &value);
    }
    void add(std::string_view name, std::string& value) {
        push(name, RELAY_PROPERTY_TEXT, Storage::text, &value);
    }

private:
    enum class Storage { boolean, integer, single, number, vector, text };
    struct Field {
        std::string name;
        int type;
        Storage storage;
        void* address;
    };
    void push(std::string_view name, int type, Storage storage, void* address) {
        fields_.push_back({std::string{name}, type, storage, address});
    }
    friend struct detail_access;
    std::vector<Field> fields_;
};

// Player input for the current game step.
namespace input {
inline const RelayHostApi& api() { return *detail::host(); }
// Actions from the project's input map.
inline bool held(std::string_view action) {
    return api().input_action(api().context, action.data(), action.size(), RELAY_INPUT_HELD) != 0;
}
inline bool pressed(std::string_view action) {
    return api().input_action(api().context, action.data(), action.size(),
                            RELAY_INPUT_PRESSED) != 0;
}
inline bool released(std::string_view action) {
    return api().input_action(api().context, action.data(), action.size(),
                            RELAY_INPUT_RELEASED) != 0;
}
// An axis from the input map, in [-1, 1] after its deadzone.
inline double axis(std::string_view name) {
    return api().input_axis(api().context, name.data(), name.size());
}
// Two axes as a direction in the XY plane (z is 0), scaled down so diagonals are not faster.
inline Vec3 vector(std::string_view x_axis, std::string_view y_axis) {
    Vec3 value{axis(x_axis), axis(y_axis), 0.0};
    return value.length() > 1.0 ? value.normalized() : value;
}
// Raw keys by physical position with US layout names: "w", "space", "left_shift", "f1", "up".
inline bool key_held(std::string_view key) {
    const auto control = "key:" + std::string{key};
    return api().input_control(api().context, control.data(), control.size(),
                            RELAY_INPUT_HELD) != 0;
}
inline bool key_pressed(std::string_view key) {
    const auto control = "key:" + std::string{key};
    return api().input_control(api().context, control.data(), control.size(),
                            RELAY_INPUT_PRESSED) != 0;
}
// Mouse buttons: "left", "middle", "right", "x1", "x2".
inline bool mouse_held(std::string_view button) {
    const auto control = "mouse:" + std::string{button};
    return api().input_control(api().context, control.data(), control.size(),
                            RELAY_INPUT_HELD) != 0;
}
inline bool mouse_pressed(std::string_view button) {
    const auto control = "mouse:" + std::string{button};
    return api().input_control(api().context, control.data(), control.size(),
                            RELAY_INPUT_PRESSED) != 0;
}
// Pointer position in the game view in pixels from its top-left corner (z is 0).
inline Vec3 mouse_position() {
    RelayVec3 position{}, delta{};
    api().input_mouse(api().context, &position, &delta);
    return {position.x, position.y, 0.0};
}
// Pointer movement during this step in pixels (z is 0); works while the cursor is locked.
inline Vec3 mouse_delta() {
    RelayVec3 position{}, delta{};
    api().input_mouse(api().context, &position, &delta);
    return {delta.x, delta.y, 0.0};
}
inline double mouse_wheel() {
    RelayVec3 position{}, delta{};
    api().input_mouse(api().context, &position, &delta);
    return delta.z;
}
// Hides and locks the cursor for mouse look (true), or shows it so the player can use interface
// controls (false), for the rest of the game. Starts as the input map's "Lock the mouse" setting.
inline void set_mouse_locked(bool locked) { (void)api().set_mouse_locked(api().context, locked ? 1 : 0); }
inline bool mouse_locked() { return api().mouse_locked(api().context) != 0; }
} // namespace input

class Behaviour {
public:
    virtual ~Behaviour() = default;

    // Declares the fields the Inspector edits and scenes save. See Properties above.
    virtual void properties(Properties& /*properties*/) {}

    // The entity this instance is attached to. Not yet set inside the constructor; initialise
    // scene-dependent state in on_start.
    [[nodiscard]] Entity self() const { return self_; }

    virtual void on_start() {}
    virtual void on_update(double /*delta_seconds*/) {}
    // Another collider started or stopped touching this entity's collider.
    virtual void on_contact_begin(Entity /*other*/) {}
    virtual void on_contact_end(Entity /*other*/) {}
    virtual void on_stop() {}
    // A script destroyed this entity (or an ancestor), or removed this behaviour, during the game;
    // the entity is still in the scene. Stop Game calls on_stop instead.
    virtual void on_destroy() {}
    // Runs after hot reload replaces this instance with newly built code.
    virtual void on_reload() { on_start(); }
    // An interface event on this entity's control or a control below it: a click, a check box
    // or toggle button flipping, a slider moving, or a press or release.
    virtual void on_ui(const ui::Event& /*event*/) {}

private:
    friend struct detail_access;
    Entity self_;
};

struct detail_access {
    static void bind(Behaviour& behaviour, Entity entity) { behaviour.self_ = entity; }
    static const auto& fields(const Properties& properties) { return properties.fields_; }
    using Storage = Properties::Storage;
};

namespace detail {
struct Registration {
    const char* name;
    Behaviour* (*create)();
};
inline std::vector<Registration>& registry() {
    static std::vector<Registration> registrations;
    return registrations;
}
struct Registrar {
    Registrar(const char* name, Behaviour* (*create)()) { registry().push_back({name, create}); }
};
} // namespace detail

} // namespace relay

// Registers a behaviour under its class name, which is what a Script component refers to. Use it
// once, at namespace scope, with an unqualified class name.
#define RELAY_BEHAVIOUR(Type)                                                                     \
    static const ::relay::detail::Registrar relay_behaviour_registrar_##Type{                    \
        #Type, []() -> ::relay::Behaviour* { return new Type(); }};

#ifdef RELAY_SCRIPT_DEFINE_ENTRY
// Compiled once per script library by Relay's build; scripts never define this.
namespace relay::detail {
inline void copy_error(char* error, size_t capacity, const char* message) {
    if (capacity == 0) return;
    const auto length = std::min(std::strlen(message), capacity - 1);
    std::memcpy(error, message, length);
    error[length] = '\0';
}
// A behaviour's declared properties with their code defaults, read once from a fresh instance.
struct PropertyDefaults {
    bool ready = false;
    std::vector<std::string> names;
    std::vector<std::string> texts;
    std::vector<RelayPropertyInfo> infos;
};
inline RelayPropertyValue read_field(int type, detail_access::Storage storage, const void* address) {
    using S = detail_access::Storage;
    RelayPropertyValue value{};
    value.type = type;
    switch (storage) {
    case S::boolean: value.boolean = *static_cast<const bool*>(address) ? 1 : 0; break;
    case S::integer: value.number = *static_cast<const int*>(address); break;
    case S::single: value.number = *static_cast<const float*>(address); break;
    case S::number: value.number = *static_cast<const double*>(address); break;
    case S::vector: {
        const auto& v = *static_cast<const relay::Vec3*>(address);
        value.vector = {v.x, v.y, v.z};
        break;
    }
    case S::text: break;
    }
    return value;
}
inline std::vector<Registration>& sorted() {
    static std::vector<Registration> result = [] {
        auto list = registry();
        std::sort(list.begin(), list.end(), [](const Registration& a, const Registration& b) {
            return std::strcmp(a.name, b.name) < 0;
        });
        return list;
    }();
    return result;
}
inline const PropertyDefaults* property_defaults(uint32_t index) {
    static std::vector<PropertyDefaults> cache(sorted().size());
    if (index >= cache.size()) return nullptr;
    auto& entry = cache[index];
    if (entry.ready) return &entry;
    entry.ready = true;
    // A throwing constructor or properties() leaves the behaviour without editable properties.
    try {
        const std::unique_ptr<relay::Behaviour> behaviour(sorted()[index].create());
        relay::Properties properties;
        behaviour->properties(properties);
        const auto& fields = relay::detail_access::fields(properties);
        entry.names.reserve(fields.size());
        entry.texts.reserve(fields.size());
        for (const auto& field : fields) {
            entry.names.push_back(field.name);
            entry.texts.push_back(field.storage == relay::detail_access::Storage::text
                                      ? *static_cast<const std::string*>(field.address)
                                      : std::string{});
        }
        for (std::size_t item = 0; item < fields.size(); ++item) {
            auto value = read_field(fields[item].type, fields[item].storage, fields[item].address);
            value.text = entry.texts[item].data();
            value.text_length = entry.texts[item].size();
            entry.infos.push_back({entry.names[item].c_str(), value});
        }
    } catch (...) {
        entry.names.clear();
        entry.texts.clear();
        entry.infos.clear();
    }
    return &entry;
}
} // namespace relay::detail

extern "C" __attribute__((visibility("default"))) const RelayScriptModule*
relay_script_module_v1(const RelayHostApi* host) {
    using namespace relay::detail;
    if (!host || host->abi_version != RELAY_SCRIPT_ABI_VERSION || host->size < sizeof(RelayHostApi))
        return nullptr;
    relay::detail::host() = host;
    static const RelayScriptModule module{
        RELAY_SCRIPT_ABI_VERSION,
        sizeof(RelayScriptModule),
        [] { return static_cast<uint32_t>(sorted().size()); },
        [](uint32_t index) { return index < sorted().size() ? sorted()[index].name : nullptr; },
        [](uint32_t index, RelayEntity entity, char* error, size_t capacity) -> void* {
            if (index >= sorted().size()) {
                copy_error(error, capacity, "unknown behaviour");
                return nullptr;
            }
            try {
                auto* behaviour = sorted()[index].create();
                relay::detail_access::bind(*behaviour, relay::Entity{entity});
                return behaviour;
            } catch (const std::exception& exception) {
                copy_error(error, capacity, exception.what());
            } catch (...) {
                copy_error(error, capacity, "unknown exception in constructor");
            }
            return nullptr;
        },
        [](void* instance) { delete static_cast<relay::Behaviour*>(instance); },
        [](void* instance, int callback, double delta, RelayEntity other, char* error,
           size_t capacity) -> int {
            auto& behaviour = *static_cast<relay::Behaviour*>(instance);
            try {
                switch (callback) {
                case RELAY_CALLBACK_START: behaviour.on_start(); break;
                case RELAY_CALLBACK_UPDATE: behaviour.on_update(delta); break;
                case RELAY_CALLBACK_CONTACT_BEGIN: behaviour.on_contact_begin(relay::Entity{other}); break;
                case RELAY_CALLBACK_CONTACT_END: behaviour.on_contact_end(relay::Entity{other}); break;
                case RELAY_CALLBACK_STOP: behaviour.on_stop(); break;
                case RELAY_CALLBACK_RELOAD: behaviour.on_reload(); break;
                case RELAY_CALLBACK_DESTROY: behaviour.on_destroy(); break;
                case RELAY_CALLBACK_UI: {
                    int type = 0;
                    RelayEntity control = other;
                    double value = 0.0;
                    const auto& api = *relay::detail::host();
                    if (api.ui_event(api.context, &type, &control, &value))
                        behaviour.on_ui({static_cast<relay::ui::EventType>(type), relay::Entity{control}, value});
                    break;
                }
                default: copy_error(error, capacity, "unknown callback"); return 0;
                }
                return 1;
            } catch (const std::exception& exception) {
                copy_error(error, capacity, exception.what());
            } catch (...) {
                copy_error(error, capacity, "unknown exception");
            }
            return 0;
        },
        [](uint32_t index) -> uint32_t {
            const auto* defaults = property_defaults(index);
            return defaults ? static_cast<uint32_t>(defaults->infos.size()) : 0u;
        },
        [](uint32_t index, uint32_t property, RelayPropertyInfo* info) -> int {
            const auto* defaults = property_defaults(index);
            if (!defaults || property >= defaults->infos.size()) return 0;
            *info = defaults->infos[property];
            return 1;
        },
        [](void* instance, const char* name, size_t length, const RelayPropertyValue* value,
           char* error, size_t capacity) -> int {
            using S = relay::detail_access::Storage;
            try {
                relay::Properties properties;
                static_cast<relay::Behaviour*>(instance)->properties(properties);
                const std::string_view wanted{name, length};
                for (const auto& field : relay::detail_access::fields(properties)) {
                    if (field.name != wanted) continue;
                    if (field.type != value->type) {
                        copy_error(error, capacity, "property type changed in code");
                        return 0;
                    }
                    switch (field.storage) {
                    case S::boolean: *static_cast<bool*>(field.address) = value->boolean != 0; break;
                    case S::integer:
                        *static_cast<int*>(field.address) = static_cast<int>(std::llround(
                            std::clamp(value->number, -2147483648.0, 2147483647.0)));
                        break;
                    case S::single: *static_cast<float*>(field.address) = static_cast<float>(value->number); break;
                    case S::number: *static_cast<double*>(field.address) = value->number; break;
                    case S::vector:
                        *static_cast<relay::Vec3*>(field.address) =
                            {value->vector.x, value->vector.y, value->vector.z};
                        break;
                    case S::text:
                        static_cast<std::string*>(field.address)->assign(value->text, value->text_length);
                        break;
                    }
                    return 1;
                }
                copy_error(error, capacity, "property is not declared in code");
            } catch (const std::exception& exception) {
                copy_error(error, capacity, exception.what());
            } catch (...) {
                copy_error(error, capacity, "unknown exception");
            }
            return 0;
        },
    };
    return &module;
}
#endif
