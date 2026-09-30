#pragma once

// The particle emitter component: sparks, smoke, fire, dust, rain, magic trails and explosions.
//
// An emitter spawns camera-facing sprites from a shape (a point, sphere, cone, box, circle or
// line) at a rate, in bursts or as it moves, gives each one a lifetime, speed, size, rotation and
// color, and moves them with gravity, drag, turbulence and collisions. Over its life a particle's
// size, speed and color follow curves and gradients, and its sprite can play a flipbook.
//
// Every field is described once in particle_fields(): scene files, the control protocol, scripts
// and the editor's Inspector all read and write emitters through that table.

#include "relay/scene/ui.hpp"
#include "relay/scene/vec3.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace relay {

struct JsonValue;

// A value picked at random between two ends, for each particle as it is born.
struct ParticleRange {
    double min{};
    double max{};
    auto operator<=>(const ParticleRange&) const = default;
};

struct ParticleCurveKey {
    double time{};  // 0 at birth, 1 at death.
    double value{};
    auto operator<=>(const ParticleCurveKey&) const = default;
};

// A value over a particle's life, straight lines between keys in time order. No keys means 1
// throughout; one key holds its value.
struct ParticleCurve {
    std::vector<ParticleCurveKey> keys;
    [[nodiscard]] double sample(double time) const;
    auto operator<=>(const ParticleCurve&) const = default;
};

struct ParticleGradientKey {
    double time{};
    UiColor color{}; // sRGB with straight alpha, each 0 to 1.
    auto operator<=>(const ParticleGradientKey&) const = default;
};

// A color over a particle's life, blended between keys; it multiplies the particle's start
// color. No keys is opaque white throughout.
struct ParticleGradient {
    std::vector<ParticleGradientKey> keys;
    [[nodiscard]] UiColor sample(double time) const;
    auto operator<=>(const ParticleGradient&) const = default;
};

// `count` particles at `time` seconds into each emitter cycle, repeated `cycles` times (0 for as
// long as the cycle lasts) every `interval` seconds, each time with this probability.
struct ParticleBurst {
    double time{};
    std::int32_t count{30};
    std::int32_t cycles{1};
    double interval{0.5};
    double probability{1.0};
    auto operator<=>(const ParticleBurst&) const = default;
};

struct ParticleEmitter {
    // Emitter.
    bool play_on_start{true}; // Starts when Run Game starts or the node is spawned.
    bool looping{true};
    double duration{5.0};     // Seconds per cycle; bursts repeat each cycle.
    bool prewarm{};           // Looping emitters start as if a whole cycle had already run.
    double start_delay{};
    std::int32_t max_particles{1000};
    // world leaves particles behind as the node moves; local carries them with it.
    enum class Space : std::uint8_t { local, world };
    Space simulation_space{Space::world};
    double simulation_speed{1.0};
    // Particles come out the same way every run for the same seed; 0 takes one from the node.
    std::int32_t seed{};

    // Emission.
    double rate{10.0};             // Particles per second.
    double rate_over_distance{};   // Particles per metre the node moves.
    std::vector<ParticleBurst> bursts;

    // Shape, in the node's space, turned and moved by shape_rotation and shape_offset.
    enum class Shape : std::uint8_t { point, sphere, hemisphere, cone, box, circle, edge };
    Shape shape{Shape::cone};
    double radius{0.25};          // Sphere, hemisphere, cone base, circle; half the edge's length.
    double radius_thickness{1.0}; // 0 emits from the surface or rim, 1 from anywhere inside.
    double angle{25.0};           // Cone: degrees between its axis (+Y) and its side.
    double arc{360.0};            // Degrees around the axis that the circle, cone or sphere covers.
    Vec3 box_size{1.0, 1.0, 1.0};
    Vec3 shape_offset{};
    Vec3 shape_rotation{};        // Euler degrees, like a node's rotation.
    double direction_randomness{}; // 0 follows the shape's direction, 1 is any direction.
    double spherize{};            // 1 points particles straight out from the shape's centre.

    // Each particle's start.
    ParticleRange lifetime{1.0, 2.0};
    ParticleRange speed{2.0, 3.0};
    ParticleRange size{0.2, 0.3};
    double aspect{1.0};           // Width over height.
    ParticleRange rotation{};     // Degrees.
    ParticleRange angular_velocity{}; // Degrees per second.
    UiColor color{};
    bool random_color{};          // Each particle mixes color and color_alt at random.
    UiColor color_alt{};
    double inherit_velocity{};    // How much of the node's own velocity particles keep.

    // Motion.
    double gravity{};             // Times the world's gravity, 9.81 m/s² downwards.
    Vec3 acceleration{};          // World space, m/s².
    Vec3 velocity{};              // Added to every particle's own, in simulation space.
    double drag{};                // Speed lost per second, as a fraction.
    ParticleCurve speed_over_lifetime;
    double noise_strength{};      // Turbulence, m/s.
    double noise_frequency{0.5};  // Swirls per metre.
    double noise_scroll{0.25};    // How fast the turbulence changes, per second.

    // Collision: none, a horizontal plane at plane_height (world Y), or the scene's colliders
    // during Run Game.
    enum class Collision : std::uint8_t { none, plane, world };
    Collision collision{Collision::none};
    double plane_height{};
    double bounce{0.4};           // Share of speed kept into the surface.
    double friction{0.2};         // Share of sliding speed lost at each hit.
    double lifetime_loss{};       // Share of its lifetime a particle loses at each hit; 1 kills.
    double collision_radius{0.05};

    // Over each particle's life.
    ParticleCurve size_over_lifetime;
    ParticleGradient color_over_lifetime;

    // Flipbook: the texture is a grid of frames, left to right and top to bottom.
    std::int32_t sheet_columns{1};
    std::int32_t sheet_rows{1};
    enum class SheetMode : std::uint8_t { over_lifetime, fps, random };
    SheetMode sheet_mode{SheetMode::over_lifetime};
    double sheet_fps{10.0};
    double sheet_cycles{1.0};     // Over lifetime: how many times the frames play.
    bool sheet_random_start{};
    bool sheet_blend{true};       // Cross-fades between frames.

    // Drawing.
    std::string texture;          // A project PNG or JPEG; empty uses builtin_texture.
    enum class Builtin : std::uint8_t { soft_dot, dot, ring, star, smoke, spark, square };
    Builtin builtin_texture{Builtin::soft_dot};
    // alpha blends over the scene, additive adds light (fire, sparks, magic), premultiplied is for
    // textures whose colors already carry their alpha.
    enum class Blend : std::uint8_t { alpha, additive, premultiplied };
    Blend blend{Blend::alpha};
    // billboard faces the camera; stretched draws streaks along the velocity; horizontal lies flat;
    // vertical stands upright facing the camera; local faces along the node's Z axis.
    enum class Alignment : std::uint8_t { billboard, stretched, horizontal, vertical, local };
    Alignment alignment{Alignment::billboard};
    double stretch_speed{0.1};    // Stretched: extra length per m/s.
    double stretch_length{1.0};   // Stretched: length multiplier.
    double emission{1.0};         // Brightness; above 1 glows (with bloom).
    bool lit{};                   // Shaded by the scene's lights and sky instead of full bright.
    double soft_distance{0.2};    // Fades particles out this close to surfaces behind them; 0 is off.
    enum class Sort : std::uint8_t { distance, oldest_first, youngest_first, none };
    Sort sort{Sort::distance};

    // Sub emitter: a child node's emitter, by name, fired where particles are born, collide or die.
    std::string sub_emitter;
    enum class Trigger : std::uint8_t { death, collision, birth };
    Trigger sub_emitter_trigger{Trigger::death};
    std::int32_t sub_emitter_count{10};
    double sub_emitter_inherit{}; // Share of the particle's velocity the new ones keep.

    auto operator<=>(const ParticleEmitter&) const = default;
};

inline constexpr std::int32_t maximum_particles_per_emitter = 100000;
inline constexpr std::size_t maximum_particle_curve_keys = 8U;
inline constexpr std::size_t maximum_particle_bursts = 8U;
inline constexpr std::uint32_t particle_emitter_stable_id = 0x1fU;

enum class ParticleFieldType : std::uint8_t {
    boolean, number, integer, vec3, range, color, choice, asset, text, curve, gradient, bursts
};

// One value of any field type. Integers are numbers; choices, text and asset paths are strings.
using ParticleValue = std::variant<bool, double, Vec3, ParticleRange, UiColor, std::string, ParticleCurve,
                                   ParticleGradient, std::vector<ParticleBurst>>;

struct ParticleFieldInfo {
    std::string_view name;  // Wire name, also the Inspector label with spaces.
    ParticleFieldType type{};
    std::string_view group; // The Inspector section: emitter, emission, shape, particle, motion,
                            // collision, lifetime, sheet, renderer, sub_emitter.
    double minimum{};       // Numbers, and every component of vectors, ranges and curves.
    double maximum{};       // Also the byte limit of text.
    std::vector<std::string_view> choices;
    std::string_view description;
};

// In the order the Inspector shows them.
[[nodiscard]] const std::vector<ParticleFieldInfo>& particle_fields();
[[nodiscard]] std::optional<std::size_t> particle_field_index(std::string_view name);
// Unchecked access by field index.
[[nodiscard]] ParticleValue particle_value(const ParticleEmitter& emitter, std::size_t index);
void set_particle_value(ParticleEmitter& emitter, std::size_t index, const ParticleValue& value);

// Whether a value suits a field: its type, range, choice, length, key count and order.
[[nodiscard]] bool check_particle_value(const ParticleFieldInfo& field, const ParticleValue& value,
                                        std::string& error);
// A field's value from JSON: booleans, numbers, strings, [x, y, z] vectors, [min, max] ranges (or
// one number for both), [r, g, b, a] sRGB colors, curves as [[time, value], ...], gradients as
// [[time, r, g, b, a], ...] and bursts as [{"time", "count", "cycles", "interval",
// "probability"}, ...]. Curve and gradient keys are put in time order.
[[nodiscard]] std::optional<ParticleValue> particle_value_from_json(const ParticleFieldInfo& field,
                                                                    const JsonValue& value, std::string& error);
[[nodiscard]] std::string particle_value_json(const ParticleValue& value);

// An object with every field.
[[nodiscard]] std::string particle_emitter_json(const ParticleEmitter& emitter);
// Reads what particle_emitter_json writes; fields left out keep their defaults, unknown fields are
// errors.
[[nodiscard]] bool read_particle_emitter(const JsonValue& value, ParticleEmitter& emitter, std::string& error);
// A partial update: `values` holds field names and JSON values.
[[nodiscard]] bool apply_particle_values(ParticleEmitter& emitter, const JsonValue& values, std::string& error);
[[nodiscard]] bool valid_particle_emitter(const ParticleEmitter& emitter, std::string* error = nullptr);

} // namespace relay
