#include "relay/scene/particles.hpp"

#include "relay/core/json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <functional>
#include <memory>

namespace relay {
namespace {

using Emitter = ParticleEmitter;

struct Accessor {
    std::function<ParticleValue(const Emitter&)> get;
    std::function<void(Emitter&, const ParticleValue&)> set;
};

// The field list with typed accessors, so the generic code never needs a switch over fields.
struct Table {
    std::vector<ParticleFieldInfo> fields;
    std::vector<Accessor> accessors;
    std::string_view group;

    Table& section(std::string_view name) {
        group = name;
        return *this;
    }
    template <class T>
    Table& add(ParticleFieldInfo info, T Emitter::*member) {
        info.group = group;
        fields.push_back(std::move(info));
        accessors.push_back({[member](const Emitter& e) { return ParticleValue{e.*member}; },
                             [member](Emitter& e, const ParticleValue& value) { e.*member = std::get<T>(value); }});
        return *this;
    }
    Table& boolean(std::string_view name, bool Emitter::*member, std::string_view description) {
        return add({name, ParticleFieldType::boolean, {}, 0.0, 1.0, {}, description}, member);
    }
    Table& number(std::string_view name, double Emitter::*member, double minimum, double maximum,
                  std::string_view description) {
        return add({name, ParticleFieldType::number, {}, minimum, maximum, {}, description}, member);
    }
    Table& integer(std::string_view name, std::int32_t Emitter::*member, double minimum, double maximum,
                   std::string_view description) {
        fields.push_back({name, ParticleFieldType::integer, group, minimum, maximum, {}, description});
        accessors.push_back(
            {[member](const Emitter& e) { return ParticleValue{static_cast<double>(e.*member)}; },
             [member](Emitter& e, const ParticleValue& value) {
                 e.*member = static_cast<std::int32_t>(std::llround(std::get<double>(value)));
             }});
        return *this;
    }
    Table& vec3(std::string_view name, Vec3 Emitter::*member, double minimum, double maximum,
                std::string_view description) {
        return add({name, ParticleFieldType::vec3, {}, minimum, maximum, {}, description}, member);
    }
    Table& range(std::string_view name, ParticleRange Emitter::*member, double minimum, double maximum,
                 std::string_view description) {
        return add({name, ParticleFieldType::range, {}, minimum, maximum, {}, description}, member);
    }
    Table& color(std::string_view name, UiColor Emitter::*member, std::string_view description) {
        return add({name, ParticleFieldType::color, {}, 0.0, 1.0, {}, description}, member);
    }
    Table& text(std::string_view name, std::string Emitter::*member, std::size_t bytes,
                std::string_view description) {
        return add({name, ParticleFieldType::text, {}, 0.0, static_cast<double>(bytes), {}, description}, member);
    }
    Table& asset(std::string_view name, std::string Emitter::*member, std::string_view description) {
        return add({name, ParticleFieldType::asset, {}, 0.0, 128.0, {}, description}, member);
    }
    Table& curve(std::string_view name, ParticleCurve Emitter::*member, double minimum, double maximum,
                 std::string_view description) {
        return add({name, ParticleFieldType::curve, {}, minimum, maximum, {}, description}, member);
    }
    Table& gradient(std::string_view name, ParticleGradient Emitter::*member, std::string_view description) {
        return add({name, ParticleFieldType::gradient, {}, 0.0, 1.0, {}, description}, member);
    }
    Table& bursts(std::string_view name, std::vector<ParticleBurst> Emitter::*member, std::string_view description) {
        return add({name, ParticleFieldType::bursts, {}, 0.0, 100000.0, {}, description}, member);
    }
    template <class E>
    Table& choice(std::string_view name, E Emitter::*member, std::vector<std::string_view> names,
                  std::string_view description) {
        fields.push_back({name, ParticleFieldType::choice, group, 0.0, 0.0, names, description});
        accessors.push_back({[member, names](const Emitter& e) {
                                 const auto index = static_cast<std::size_t>(e.*member);
                                 return ParticleValue{std::string(index < names.size() ? names[index] : names.front())};
                             },
                             [member, names](Emitter& e, const ParticleValue& value) {
                                 const auto& wanted = std::get<std::string>(value);
                                 const auto found = std::find(names.begin(), names.end(), wanted);
                                 if (found != names.end()) e.*member = static_cast<E>(found - names.begin());
                             }});
        return *this;
    }
};

const Table& table() {
    static const Table fields = [] {
        Table t;
        t.section("emitter")
            .boolean("play_on_start", &Emitter::play_on_start,
                     "Start emitting when Run Game starts or the node is spawned; otherwise scripts start it")
            .boolean("looping", &Emitter::looping, "Start a new cycle when one ends, for as long as it plays")
            .number("duration", &Emitter::duration, 0.05, 3600.0,
                    "Seconds in one cycle. Bursts repeat each cycle; a one-shot emitter stops after one")
            .boolean("prewarm", &Emitter::prewarm, "Looping emitters start full, as if a cycle had already run")
            .number("start_delay", &Emitter::start_delay, 0.0, 3600.0, "Seconds to wait before emitting")
            .integer("max_particles", &Emitter::max_particles, 1.0, maximum_particles_per_emitter,
                     "At most this many particles live at once; more are not born")
            .choice("simulation_space", &Emitter::simulation_space, {"local", "world"},
                    "world leaves particles behind as the node moves (smoke, trails); local carries them "
                    "with it (a jet flame)")
            .number("simulation_speed", &Emitter::simulation_speed, 0.0, 100.0, "Plays the effect faster or slower")
            .integer("seed", &Emitter::seed, 0.0, 2147483647.0,
                     "Random seed: the same seed gives the same particles every run; 0 takes one from the node");
        t.section("emission")
            .number("rate", &Emitter::rate, 0.0, 100000.0, "Particles born per second")
            .number("rate_over_distance", &Emitter::rate_over_distance, 0.0, 10000.0,
                    "Particles born per metre the node moves, for trails")
            .bursts("bursts", &Emitter::bursts,
                    "Groups of particles at set times in each cycle: time, count, cycles (0 repeats all "
                    "cycle), interval and probability");
        t.section("shape")
            .choice("shape", &Emitter::shape, {"point", "sphere", "hemisphere", "cone", "box", "circle", "edge"},
                    "Where particles are born and which way they go: out of a point, sphere or hemisphere, "
                    "up a cone, up from a box, out across a circle, or up from a line along X")
            .number("radius", &Emitter::radius, 0.0, 10000.0,
                    "Sphere, hemisphere, cone base and circle radius; half the edge's length")
            .number("radius_thickness", &Emitter::radius_thickness, 0.0, 1.0,
                    "0 emits from the surface or rim only, 1 from anywhere inside")
            .number("angle", &Emitter::angle, 0.0, 90.0, "Cone: degrees between its axis and its side")
            .number("arc", &Emitter::arc, 0.0, 360.0, "Degrees around the axis the circle, cone or sphere covers")
            .vec3("box_size", &Emitter::box_size, 0.0, 10000.0, "Box: its size along X, Y and Z")
            .vec3("shape_offset", &Emitter::shape_offset, -10000.0, 10000.0, "Moves the shape within the node")
            .vec3("shape_rotation", &Emitter::shape_rotation, -3600.0, 3600.0,
                  "Turns the shape within the node, in degrees; the shape emits along its +Y")
            .number("direction_randomness", &Emitter::direction_randomness, 0.0, 1.0,
                    "0 follows the shape's direction, 1 sends particles any way")
            .number("spherize", &Emitter::spherize, 0.0, 1.0, "1 sends particles straight out from the shape's centre");
        t.section("particle")
            .range("lifetime", &Emitter::lifetime, 0.01, 3600.0, "Seconds each particle lives, between these two")
            .range("speed", &Emitter::speed, -10000.0, 10000.0, "Start speed along the shape's direction, m/s")
            .range("size", &Emitter::size, 0.0, 10000.0, "Start size in metres")
            .number("aspect", &Emitter::aspect, 0.01, 100.0, "Width over height; 1 is square")
            .range("rotation", &Emitter::rotation, -3600.0, 3600.0, "Start rotation in degrees")
            .range("angular_velocity", &Emitter::angular_velocity, -36000.0, 36000.0, "Spin in degrees per second")
            .color("color", &Emitter::color, "Start color and opacity")
            .boolean("random_color", &Emitter::random_color, "Give each particle a random mix of color and color_alt")
            .color("color_alt", &Emitter::color_alt, "The other end of the random mix, with random_color")
            .number("inherit_velocity", &Emitter::inherit_velocity, 0.0, 2.0,
                    "How much of the node's own velocity new particles keep (world space)");
        t.section("motion")
            .number("gravity", &Emitter::gravity, -100.0, 100.0,
                    "Times the world's gravity (9.81 m/s² down); negative rises")
            .vec3("acceleration", &Emitter::acceleration, -10000.0, 10000.0, "A constant push in world space, m/s², like wind")
            .vec3("velocity", &Emitter::velocity, -10000.0, 10000.0, "Added to every particle's velocity, in simulation space")
            .number("drag", &Emitter::drag, 0.0, 100.0, "Share of speed lost per second")
            .curve("speed_over_lifetime", &Emitter::speed_over_lifetime, 0.0, 100.0,
                   "Multiplies each particle's speed over its life")
            .number("noise_strength", &Emitter::noise_strength, 0.0, 1000.0, "Turbulence: how hard swirls push, m/s")
            .number("noise_frequency", &Emitter::noise_frequency, 0.001, 100.0, "Turbulence: swirls per metre")
            .number("noise_scroll", &Emitter::noise_scroll, 0.0, 100.0, "Turbulence: how fast the swirls change");
        t.section("collision")
            .choice("collision", &Emitter::collision, {"none", "plane", "world"},
                    "none passes through everything; plane bounces off a floor at plane_height; world "
                    "bounces off the scene's colliders during Run Game")
            .number("plane_height", &Emitter::plane_height, -1e6, 1e6, "Plane: the floor's world height (Y)")
            .number("bounce", &Emitter::bounce, 0.0, 2.0, "Share of speed into the surface kept after a hit")
            .number("friction", &Emitter::friction, 0.0, 1.0, "Share of sliding speed lost at each hit")
            .number("lifetime_loss", &Emitter::lifetime_loss, 0.0, 1.0,
                    "Share of its lifetime a particle loses at each hit; 1 dies on the first")
            .number("collision_radius", &Emitter::collision_radius, 0.0, 100.0, "How far from surfaces particles stop");
        t.section("lifetime")
            .curve("size_over_lifetime", &Emitter::size_over_lifetime, 0.0, 100.0,
                   "Multiplies each particle's size over its life")
            .gradient("color_over_lifetime", &Emitter::color_over_lifetime,
                      "Multiplies each particle's color and alpha over its life");
        t.section("sheet")
            .integer("sheet_columns", &Emitter::sheet_columns, 1.0, 64.0, "Flipbook: frames across the texture")
            .integer("sheet_rows", &Emitter::sheet_rows, 1.0, 64.0, "Flipbook: frames down the texture")
            .choice("sheet_mode", &Emitter::sheet_mode, {"over_lifetime", "fps", "random"},
                    "Play the frames over each particle's life, at sheet_fps, or show one at random")
            .number("sheet_fps", &Emitter::sheet_fps, 0.0, 240.0, "Flipbook: frames per second, for fps")
            .number("sheet_cycles", &Emitter::sheet_cycles, 0.01, 100.0,
                    "Flipbook: times the frames play over a life, for over_lifetime")
            .boolean("sheet_random_start", &Emitter::sheet_random_start, "Flipbook: each particle starts on a random frame")
            .boolean("sheet_blend", &Emitter::sheet_blend, "Flipbook: cross-fade between frames");
        t.section("renderer")
            .asset("texture", &Emitter::texture, "A project PNG or JPEG; empty uses the built-in texture")
            .choice("builtin_texture", &Emitter::builtin_texture,
                    {"soft_dot", "dot", "ring", "star", "smoke", "spark", "square"},
                    "The sprite drawn without a texture")
            .choice("blend", &Emitter::blend, {"alpha", "additive", "premultiplied"},
                    "alpha covers what is behind; additive adds light, for fire, sparks and magic; "
                    "premultiplied suits textures whose colors carry their alpha")
            .choice("alignment", &Emitter::alignment, {"billboard", "stretched", "horizontal", "vertical", "local"},
                    "billboard faces the camera; stretched streaks along the velocity; horizontal lies flat; "
                    "vertical stands upright facing the camera; local faces along the node's Z axis")
            .number("stretch_speed", &Emitter::stretch_speed, 0.0, 100.0, "Stretched: extra length per m/s of speed")
            .number("stretch_length", &Emitter::stretch_length, 0.01, 100.0, "Stretched: length multiplier")
            .number("emission", &Emitter::emission, 0.0, 1000.0, "Brightness; above 1 glows, and blooms with a Bloom effect")
            .boolean("lit", &Emitter::lit, "Shade particles with the scene's lights and sky, for smoke and dust")
            .number("soft_distance", &Emitter::soft_distance, 0.0, 100.0,
                    "Fade particles out this close to surfaces behind them; 0 shows hard edges")
            .choice("sort", &Emitter::sort, {"distance", "oldest_first", "youngest_first", "none"},
                    "Drawing order: farthest first (right for alpha), oldest or youngest first, or as stored");
        t.section("sub_emitter")
            .text("sub_emitter", &Emitter::sub_emitter, 128U,
                  "A child node's name: its particle emitter fires where these particles are born, "
                  "collide or die, for fireworks and impacts")
            .choice("sub_emitter_trigger", &Emitter::sub_emitter_trigger, {"death", "collision", "birth"},
                    "When the sub emitter fires")
            .integer("sub_emitter_count", &Emitter::sub_emitter_count, 1.0, 1000.0, "Particles it emits each time")
            .number("sub_emitter_inherit", &Emitter::sub_emitter_inherit, 0.0, 1.0,
                    "Share of the particle's velocity its sub particles keep");
        return t;
    }();
    return fields;
}

std::string number_json(const double value) {
    if (!std::isfinite(value)) return "0";
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string("0");
}

bool finite_within(const double value, const double minimum, const double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20U) {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00";
            out += hex[(static_cast<unsigned char>(c) >> 4U) & 0xFU];
            out += hex[static_cast<unsigned char>(c) & 0xFU];
        } else {
            out += c;
        }
    }
    return out;
}

double lerp(const double a, const double b, const double t) { return a + (b - a) * t; }

// JSON arrays of exactly `count` numbers.
std::optional<std::vector<double>> numbers_of(const JsonValue& value, const std::size_t count) {
    const auto* array = value.array();
    if (!array || array->size() != count) return std::nullopt;
    std::vector<double> numbers;
    for (const auto& item : *array) {
        const auto* number = item.number();
        if (!number) return std::nullopt;
        numbers.push_back(*number);
    }
    return numbers;
}

} // namespace

double ParticleCurve::sample(const double time) const {
    if (keys.empty()) return 1.0;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto& next = keys[index];
        if (time > next.time) continue;
        const auto& previous = keys[index - 1U];
        const double span = next.time - previous.time;
        return span <= 0.0 ? next.value : lerp(previous.value, next.value, (time - previous.time) / span);
    }
    return keys.back().value;
}

UiColor ParticleGradient::sample(const double time) const {
    if (keys.empty()) return {};
    if (time <= keys.front().time) return keys.front().color;
    if (time >= keys.back().time) return keys.back().color;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto& next = keys[index];
        if (time > next.time) continue;
        const auto& previous = keys[index - 1U];
        const double span = next.time - previous.time;
        const double t = span <= 0.0 ? 1.0 : (time - previous.time) / span;
        return {lerp(previous.color.r, next.color.r, t), lerp(previous.color.g, next.color.g, t),
                lerp(previous.color.b, next.color.b, t), lerp(previous.color.a, next.color.a, t)};
    }
    return keys.back().color;
}

const std::vector<ParticleFieldInfo>& particle_fields() { return table().fields; }

std::optional<std::size_t> particle_field_index(const std::string_view name) {
    const auto& fields = particle_fields();
    for (std::size_t index = 0; index < fields.size(); ++index)
        if (fields[index].name == name) return index;
    return std::nullopt;
}

ParticleValue particle_value(const ParticleEmitter& emitter, const std::size_t index) {
    return table().accessors[index].get(emitter);
}

void set_particle_value(ParticleEmitter& emitter, const std::size_t index, const ParticleValue& value) {
    table().accessors[index].set(emitter, value);
}

bool check_particle_value(const ParticleFieldInfo& field, const ParticleValue& value, std::string& error) {
    const auto fail = [&](std::string message) {
        error = std::string(field.name) + ' ' + message;
        return false;
    };
    const auto range = [&] { return "must be from " + number_json(field.minimum) + " to " + number_json(field.maximum); };
    const auto within = [&](double number) { return finite_within(number, field.minimum, field.maximum); };
    switch (field.type) {
    case ParticleFieldType::boolean:
        return std::holds_alternative<bool>(value) || fail("must be true or false");
    case ParticleFieldType::number:
    case ParticleFieldType::integer: {
        const auto* number = std::get_if<double>(&value);
        if (!number) return fail("must be a number");
        if (!within(*number)) return fail(range());
        if (field.type == ParticleFieldType::integer && std::floor(*number) != *number)
            return fail("must be a whole number");
        return true;
    }
    case ParticleFieldType::vec3: {
        const auto* vector = std::get_if<Vec3>(&value);
        if (!vector) return fail("must be three numbers");
        return (within(vector->x) && within(vector->y) && within(vector->z)) || fail("values " + range());
    }
    case ParticleFieldType::range: {
        const auto* pair = std::get_if<ParticleRange>(&value);
        if (!pair) return fail("must be two numbers, min and max");
        if (!within(pair->min) || !within(pair->max)) return fail("values " + range());
        return pair->min <= pair->max || fail("min must not be more than max");
    }
    case ParticleFieldType::color: {
        const auto* color = std::get_if<UiColor>(&value);
        if (!color) return fail("must be a color: red, green, blue and alpha");
        for (const double channel : {color->r, color->g, color->b, color->a})
            if (!finite_within(channel, 0.0, 1.0)) return fail("channels must be from 0 to 1");
        return true;
    }
    case ParticleFieldType::text: {
        const auto* text = std::get_if<std::string>(&value);
        if (!text) return fail("must be text");
        return static_cast<double>(text->size()) <= field.maximum ||
               fail("must be at most " + number_json(field.maximum) + " bytes");
    }
    case ParticleFieldType::asset: {
        const auto* path = std::get_if<std::string>(&value);
        if (!path) return fail("must be a project file path");
        return path->empty() || valid_ui_asset_path(*path, {"image"}) ||
               fail("must be empty or a project-relative PNG or JPEG file");
    }
    case ParticleFieldType::choice: {
        const auto* name = std::get_if<std::string>(&value);
        if (name && std::find(field.choices.begin(), field.choices.end(), *name) != field.choices.end()) return true;
        std::string names;
        for (const auto choice : field.choices) names += (names.empty() ? "" : ", ") + std::string(choice);
        return fail("must be one of " + names);
    }
    case ParticleFieldType::curve: {
        const auto* curve = std::get_if<ParticleCurve>(&value);
        if (!curve) return fail("must be a list of [time, value] keys");
        if (curve->keys.size() > maximum_particle_curve_keys) return fail("has at most 8 keys");
        for (std::size_t index = 0; index < curve->keys.size(); ++index) {
            const auto& key = curve->keys[index];
            if (!finite_within(key.time, 0.0, 1.0)) return fail("key times must be from 0 to 1");
            if (!within(key.value)) return fail("key values " + range());
            if (index > 0U && key.time < curve->keys[index - 1U].time) return fail("keys must be in time order");
        }
        return true;
    }
    case ParticleFieldType::gradient: {
        const auto* gradient = std::get_if<ParticleGradient>(&value);
        if (!gradient) return fail("must be a list of [time, red, green, blue, alpha] keys");
        if (gradient->keys.size() > maximum_particle_curve_keys) return fail("has at most 8 keys");
        for (std::size_t index = 0; index < gradient->keys.size(); ++index) {
            const auto& key = gradient->keys[index];
            if (!finite_within(key.time, 0.0, 1.0)) return fail("key times must be from 0 to 1");
            for (const double channel : {key.color.r, key.color.g, key.color.b, key.color.a})
                if (!finite_within(channel, 0.0, 1.0)) return fail("channels must be from 0 to 1");
            if (index > 0U && key.time < gradient->keys[index - 1U].time) return fail("keys must be in time order");
        }
        return true;
    }
    case ParticleFieldType::bursts: {
        const auto* bursts = std::get_if<std::vector<ParticleBurst>>(&value);
        if (!bursts) return fail("must be a list of bursts");
        if (bursts->size() > maximum_particle_bursts) return fail("has at most 8 bursts");
        for (const auto& burst : *bursts) {
            if (!finite_within(burst.time, 0.0, 3600.0)) return fail("times must be from 0 to 3600 seconds");
            if (burst.count < 0 || burst.count > maximum_particles_per_emitter)
                return fail("counts must be from 0 to 100000");
            if (burst.cycles < 0 || burst.cycles > 10000) return fail("cycles must be from 0 (all cycle) to 10000");
            if (!finite_within(burst.interval, 0.01, 3600.0)) return fail("intervals must be from 0.01 to 3600 seconds");
            if (!finite_within(burst.probability, 0.0, 1.0)) return fail("probabilities must be from 0 to 1");
        }
        return true;
    }
    }
    return fail("has an unknown type");
}

std::optional<ParticleValue> particle_value_from_json(const ParticleFieldInfo& field, const JsonValue& value,
                                                      std::string& error) {
    const auto fail = [&](std::string message) -> std::optional<ParticleValue> {
        error = std::string(field.name) + ' ' + message;
        return std::nullopt;
    };
    std::optional<ParticleValue> result;
    switch (field.type) {
    case ParticleFieldType::boolean:
        if (!value.boolean()) return fail("must be true or false");
        result = ParticleValue{*value.boolean()};
        break;
    case ParticleFieldType::number:
    case ParticleFieldType::integer:
        if (!value.number()) return fail("must be a number");
        result = ParticleValue{*value.number()};
        break;
    case ParticleFieldType::vec3: {
        const auto numbers = numbers_of(value, 3U);
        if (!numbers) return fail("must be three numbers [x, y, z]");
        result = ParticleValue{Vec3{(*numbers)[0], (*numbers)[1], (*numbers)[2]}};
        break;
    }
    case ParticleFieldType::range: {
        if (const auto* number = value.number()) {
            result = ParticleValue{ParticleRange{*number, *number}};
            break;
        }
        const auto numbers = numbers_of(value, 2U);
        if (!numbers) return fail("must be [min, max] or one number");
        result = ParticleValue{ParticleRange{(*numbers)[0], (*numbers)[1]}};
        break;
    }
    case ParticleFieldType::color: {
        auto numbers = numbers_of(value, 4U);
        if (!numbers) {
            numbers = numbers_of(value, 3U);
            if (numbers) numbers->push_back(1.0);
        }
        if (!numbers) return fail("must be [red, green, blue, alpha] from 0 to 1");
        result = ParticleValue{UiColor{(*numbers)[0], (*numbers)[1], (*numbers)[2], (*numbers)[3]}};
        break;
    }
    case ParticleFieldType::choice:
    case ParticleFieldType::asset:
    case ParticleFieldType::text:
        if (!value.string()) return fail("must be a string");
        result = ParticleValue{*value.string()};
        break;
    case ParticleFieldType::curve: {
        const auto* array = value.array();
        if (!array) return fail("must be a list of [time, value] keys");
        ParticleCurve curve;
        for (const auto& item : *array) {
            const auto numbers = numbers_of(item, 2U);
            if (!numbers) return fail("keys must be [time, value]");
            curve.keys.push_back({(*numbers)[0], (*numbers)[1]});
        }
        std::stable_sort(curve.keys.begin(), curve.keys.end(),
                         [](const auto& a, const auto& b) { return a.time < b.time; });
        result = ParticleValue{std::move(curve)};
        break;
    }
    case ParticleFieldType::gradient: {
        const auto* array = value.array();
        if (!array) return fail("must be a list of [time, red, green, blue, alpha] keys");
        ParticleGradient gradient;
        for (const auto& item : *array) {
            const auto numbers = numbers_of(item, 5U);
            if (!numbers) return fail("keys must be [time, red, green, blue, alpha]");
            gradient.keys.push_back({(*numbers)[0], {(*numbers)[1], (*numbers)[2], (*numbers)[3], (*numbers)[4]}});
        }
        std::stable_sort(gradient.keys.begin(), gradient.keys.end(),
                         [](const auto& a, const auto& b) { return a.time < b.time; });
        result = ParticleValue{std::move(gradient)};
        break;
    }
    case ParticleFieldType::bursts: {
        const auto* array = value.array();
        if (!array) return fail("must be a list of bursts");
        std::vector<ParticleBurst> bursts;
        for (const auto& item : *array) {
            const auto* object = item.object();
            if (!object) return fail("bursts must be objects");
            ParticleBurst burst;
            for (const auto& [key, entry] : *object) {
                const auto* number = entry.number();
                if (!number) return fail("burst " + key + " must be a number");
                if (key == "time") burst.time = *number;
                else if (key == "interval") burst.interval = *number;
                else if (key == "probability") burst.probability = *number;
                else if (key == "count" || key == "cycles") {
                    if (std::floor(*number) != *number || std::abs(*number) > 1e9)
                        return fail("burst " + key + " must be a whole number");
                    (key == "count" ? burst.count : burst.cycles) = static_cast<std::int32_t>(*number);
                } else {
                    return fail("bursts have no field " + key);
                }
            }
            bursts.push_back(burst);
        }
        result = ParticleValue{std::move(bursts)};
        break;
    }
    }
    if (result && !check_particle_value(field, *result, error)) return std::nullopt;
    return result;
}

std::string particle_value_json(const ParticleValue& value) {
    return std::visit(
        [](const auto& item) -> std::string {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) {
                return item ? "true" : "false";
            } else if constexpr (std::is_same_v<T, double>) {
                return number_json(item);
            } else if constexpr (std::is_same_v<T, Vec3>) {
                return '[' + number_json(item.x) + ',' + number_json(item.y) + ',' + number_json(item.z) + ']';
            } else if constexpr (std::is_same_v<T, ParticleRange>) {
                return '[' + number_json(item.min) + ',' + number_json(item.max) + ']';
            } else if constexpr (std::is_same_v<T, UiColor>) {
                return '[' + number_json(item.r) + ',' + number_json(item.g) + ',' + number_json(item.b) + ',' +
                       number_json(item.a) + ']';
            } else if constexpr (std::is_same_v<T, std::string>) {
                return '"' + escape(item) + '"';
            } else if constexpr (std::is_same_v<T, ParticleCurve>) {
                std::string out = "[";
                for (std::size_t index = 0; index < item.keys.size(); ++index)
                    out += std::string(index ? "," : "") + '[' + number_json(item.keys[index].time) + ',' +
                           number_json(item.keys[index].value) + ']';
                return out + ']';
            } else if constexpr (std::is_same_v<T, ParticleGradient>) {
                std::string out = "[";
                for (std::size_t index = 0; index < item.keys.size(); ++index) {
                    const auto& key = item.keys[index];
                    out += std::string(index ? "," : "") + '[' + number_json(key.time) + ',' + number_json(key.color.r) +
                           ',' + number_json(key.color.g) + ',' + number_json(key.color.b) + ',' +
                           number_json(key.color.a) + ']';
                }
                return out + ']';
            } else {
                std::string out = "[";
                for (std::size_t index = 0; index < item.size(); ++index) {
                    const auto& burst = item[index];
                    out += std::string(index ? "," : "") + "{\"time\":" + number_json(burst.time) +
                           ",\"count\":" + std::to_string(burst.count) + ",\"cycles\":" + std::to_string(burst.cycles) +
                           ",\"interval\":" + number_json(burst.interval) +
                           ",\"probability\":" + number_json(burst.probability) + '}';
                }
                return out + ']';
            }
        },
        value);
}

std::string particle_emitter_json(const ParticleEmitter& emitter) {
    std::string out = "{";
    const auto& fields = particle_fields();
    for (std::size_t index = 0; index < fields.size(); ++index)
        out += std::string(index ? "," : "") + '"' + std::string(fields[index].name) +
               "\":" + particle_value_json(particle_value(emitter, index));
    return out + '}';
}

bool apply_particle_values(ParticleEmitter& emitter, const JsonValue& values, std::string& error) {
    if (values.is_null()) return true;
    const auto* object = values.object();
    if (!object) {
        error = "values must be an object of field names and values";
        return false;
    }
    auto updated = emitter;
    for (const auto& [name, value] : *object) {
        const auto index = particle_field_index(name);
        if (!index) {
            error = "particle emitters have no field " + name;
            return false;
        }
        const auto parsed = particle_value_from_json(particle_fields()[*index], value, error);
        if (!parsed) return false;
        set_particle_value(updated, *index, *parsed);
    }
    if (!valid_particle_emitter(updated, &error)) return false;
    emitter = std::move(updated);
    return true;
}

bool read_particle_emitter(const JsonValue& value, ParticleEmitter& emitter, std::string& error) {
    emitter = ParticleEmitter{};
    if (!value.object()) {
        error = "particle_emitter must be an object";
        return false;
    }
    return apply_particle_values(emitter, value, error);
}

bool valid_particle_emitter(const ParticleEmitter& emitter, std::string* error) {
    const auto& fields = particle_fields();
    std::string reason;
    for (std::size_t index = 0; index < fields.size(); ++index)
        if (!check_particle_value(fields[index], particle_value(emitter, index), reason)) {
            if (error) *error = reason;
            return false;
        }
    return true;
}

} // namespace relay
