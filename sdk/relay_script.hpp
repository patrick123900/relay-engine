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
// Frame order: every behaviour's on_update, then animation and physics, then contact callbacks for
// the contacts that step produced. Run Game calls on_start on every instance before the first
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

    RelayEntity handle_{0};
};

struct RayHit {
    Entity entity;
    double distance{};
    Vec3 point;
    Vec3 normal;
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
// Pointer position in window pixels (z is 0).
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
    // A script destroyed this entity (or an ancestor) during the game; it is still in the scene.
    // Stop Game calls on_stop instead.
    virtual void on_destroy() {}
    // Runs after hot reload replaces this instance with newly built code.
    virtual void on_reload() { on_start(); }

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
