#include "relay/particles/particle_system.hpp"

#include "relay/editor/editor_math.hpp"
#include "relay/observe/profiler.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numeric>
#include <sstream>

namespace relay {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double to_radians = pi / 180.0;
constexpr double standard_gravity = 9.81;
// Emitters of nodes that are gone keep their last particles this long at most.
constexpr double orphan_lifetime = 60.0;
// An editor preview of a one-shot effect waits this long after it finishes, then replays.
constexpr double preview_replay_seconds = 1.0;
// Prewarming simulates at most this much of a cycle, in steps of this size.
constexpr double maximum_prewarm_seconds = 30.0;
constexpr double prewarm_step = 1.0 / 30.0;

Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3 a, const double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 normalized(const Vec3 a, const Vec3 fallback) {
    const double size = length(a);
    return size > 1e-12 ? a * (1.0 / size) : fallback;
}
Vec3 lerp(const Vec3 a, const Vec3 b, const double t) { return a + (b - a) * t; }
double lerp(const double a, const double b, const double t) { return a + (b - a) * t; }

// An affine transform: three axis columns and a translation, in double precision.
struct Affine {
    Vec3 x{1.0, 0.0, 0.0};
    Vec3 y{0.0, 1.0, 0.0};
    Vec3 z{0.0, 0.0, 1.0};
    Vec3 t{};
};

Vec3 rotate(const Affine& m, const Vec3 v) { return m.x * v.x + m.y * v.y + m.z * v.z; }
Vec3 apply(const Affine& m, const Vec3 p) { return rotate(m, p) + m.t; }

Affine multiply(const Affine& a, const Affine& b) {
    return {rotate(a, b.x), rotate(a, b.y), rotate(a, b.z), apply(a, b.t)};
}

Affine from_matrix(const EditorMatrix& m) {
    return {{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}, {m[12], m[13], m[14]}};
}

Affine compose(const Vec3 position, const Vec3 rotation_degrees, const Vec3 scale) {
    return from_matrix(editor_compose(position, rotation_degrees, scale));
}

// The inverse of the rotation and scale part; a collapsed transform inverts to the identity.
Affine inverse(const Affine& m) {
    const double a = m.x.x, b = m.y.x, c = m.z.x;
    const double d = m.x.y, e = m.y.y, f = m.z.y;
    const double g = m.x.z, h = m.y.z, i = m.z.z;
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-18) return {};
    const double s = 1.0 / determinant;
    Affine result;
    result.x = {(e * i - f * h) * s, (f * g - d * i) * s, (d * h - e * g) * s};
    result.y = {(c * h - b * i) * s, (a * i - c * g) * s, (b * g - a * h) * s};
    result.z = {(b * f - c * e) * s, (c * d - a * f) * s, (a * e - b * d) * s};
    result.t = rotate(result, m.t) * -1.0;
    return result;
}

// The node's world transform, with keyframed transforms, as the renderer draws it.
std::optional<Affine> world_of(const Scene& scene, const Entity entity) {
    std::vector<const EntityRecord*> chain;
    for (auto current = entity; current.valid();) {
        const auto* record = scene.get(current);
        if (!record || chain.size() > 4096U) return std::nullopt;
        chain.push_back(record);
        current = record->parent;
    }
    Affine world;
    for (auto item = chain.rbegin(); item != chain.rend(); ++item) {
        const auto* record = *item;
        const auto transform = record->transform_animation && !record->transform_animation->keys.empty()
                                   ? sample_transform_animation(*record->transform_animation, record->transform)
                                   : record->transform;
        world = multiply(world, compose(transform.position, transform.rotation_degrees, transform.scale));
    }
    return world;
}

// PCG32: small, fast and the same on every platform.
struct Random {
    std::uint64_t state{0x853c49e6748fea9bULL};
    std::uint64_t increment{0xda3e39cb94b95bdbULL};

    void seed(const std::uint64_t value, const std::uint64_t stream) {
        state = 0U;
        increment = (stream << 1U) | 1U;
        (void)next();
        state += value;
        (void)next();
    }
    std::uint32_t next() {
        const auto old = state;
        state = old * 6364136223846793005ULL + increment;
        const auto shifted = static_cast<std::uint32_t>(((old >> 18U) ^ old) >> 27U);
        const auto rotation = static_cast<std::uint32_t>(old >> 59U);
        return (shifted >> rotation) | (shifted << ((32U - rotation) & 31U));
    }
    double uniform() { return static_cast<double>(next()) * (1.0 / 4294967296.0); }
    double between(const double a, const double b) { return lerp(a, b, uniform()); }
    double between(const ParticleRange& range) { return between(range.min, range.max); }
    Vec3 unit() {
        const double z = between(-1.0, 1.0);
        const double angle = uniform() * 2.0 * pi;
        const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        return {r * std::cos(angle), r * std::sin(angle), z};
    }
};

// Improved Perlin gradient noise, hashed rather than tabled, roughly -1 to 1.
double fade(const double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

std::uint32_t hash(const std::int64_t x, const std::int64_t y, const std::int64_t z, const std::uint32_t seed) {
    auto h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^ static_cast<std::uint32_t>(y) * 0xd8163841U ^
             static_cast<std::uint32_t>(z) * 0xcb1ab31fU ^ seed * 0x9E3779B9U;
    h ^= h >> 13U;
    h *= 0x5bd1e995U;
    h ^= h >> 15U;
    return h;
}

double gradient(const std::uint32_t h, const double x, const double y, const double z) {
    switch (h & 15U) {
    case 0: return x + y;
    case 1: return -x + y;
    case 2: return x - y;
    case 3: return -x - y;
    case 4: return x + z;
    case 5: return -x + z;
    case 6: return x - z;
    case 7: return -x - z;
    case 8: return y + z;
    case 9: return -y + z;
    case 10: return y - z;
    case 11: return -y - z;
    case 12: return x + y;
    case 13: return -y + z;
    case 14: return -x + y;
    default: return -y - z;
    }
}

double perlin(const Vec3 p, const std::uint32_t seed) {
    const double fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const auto x0 = static_cast<std::int64_t>(fx), y0 = static_cast<std::int64_t>(fy), z0 = static_cast<std::int64_t>(fz);
    const double x = p.x - fx, y = p.y - fy, z = p.z - fz;
    const double u = fade(x), v = fade(y), w = fade(z);
    const auto corner = [&](std::int64_t i, std::int64_t j, std::int64_t k) {
        return gradient(hash(x0 + i, y0 + j, z0 + k, seed), x - static_cast<double>(i), y - static_cast<double>(j),
                        z - static_cast<double>(k));
    };
    const double x00 = lerp(corner(0, 0, 0), corner(1, 0, 0), u);
    const double x10 = lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const double x01 = lerp(corner(0, 0, 1), corner(1, 0, 1), u);
    const double x11 = lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
}

Vec3 turbulence(const Vec3 p, const std::uint32_t seed) {
    return {perlin(p, seed), perlin(p + Vec3{31.416, 17.123, 5.309}, seed + 1U),
            perlin(p + Vec3{-12.713, 41.339, 27.917}, seed + 2U)};
}

float srgb_to_linear(const double value) {
    const double v = std::clamp(value, 0.0, 1.0);
    return static_cast<float>(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
}

std::string number_json(const double value) {
    if (!std::isfinite(value)) return "0";
    std::array<char, 32> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return {buffer.data(), result.ptr};
}

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20U) out += c;
    }
    return out;
}

} // namespace

struct ParticleSystem::Particle {
    Vec3 position{};
    Vec3 previous{};
    Vec3 velocity{};
    double age{};
    double lifetime{1.0};
    float size{};
    float rotation{};
    float spin{};
    float random{};
    // The share of its birth step a new particle lives: the rest had passed before it was born.
    float birth_step{1.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F}; // Linear.
};

struct ParticleSystem::State {
    Entity entity{};
    ParticleEmitter settings;
    std::vector<Particle> particles;
    Random random;
    bool playing{};
    bool paused{};
    bool stopped_by_hand{};
    bool sub{};
    bool orphan{};
    bool seen{};
    double time{};         // Since the cycle clock started, including the start delay.
    double emit_carry{};
    double distance_carry{};
    double finished_for{};
    double orphan_for{};
    std::uint32_t cycle{};
    std::uint64_t plays{};
    Affine world;
    Affine previous_world;
    bool has_world{};
    Vec3 node_velocity{};
    std::optional<std::uint64_t> sub_target;
};

std::shared_ptr<const UiTexture> builtin_particle_texture(const ParticleEmitter::Builtin builtin) {
    static std::array<std::shared_ptr<const UiTexture>, 7> made{};
    const auto index = static_cast<std::size_t>(builtin);
    if (index >= made.size()) return nullptr;
    if (made[index]) return made[index];
    constexpr std::uint32_t side = 128U;
    auto texture = std::make_shared<UiTexture>();
    texture->id = next_ui_texture_id();
    texture->revision = 1U;
    texture->width = texture->height = side;
    texture->rgba.resize(static_cast<std::size_t>(side) * side * 4U);
    Random random;
    random.seed(0x5eedULL, index);
    for (std::uint32_t row = 0; row < side; ++row)
        for (std::uint32_t column = 0; column < side; ++column) {
            // -1 to 1 across the texture, y up.
            const double x = (static_cast<double>(column) + 0.5) / side * 2.0 - 1.0;
            const double y = 1.0 - (static_cast<double>(row) + 0.5) / side * 2.0;
            const double r = std::sqrt(x * x + y * y);
            const double pixel = 2.0 / side;
            double alpha = 0.0;
            double shade = 1.0;
            switch (builtin) {
            case ParticleEmitter::Builtin::soft_dot: {
                const double falloff = std::clamp(1.0 - r, 0.0, 1.0);
                alpha = falloff * falloff * (3.0 - 2.0 * falloff);
                break;
            }
            case ParticleEmitter::Builtin::dot:
                alpha = std::clamp((0.9 - r) / pixel + 0.5, 0.0, 1.0);
                break;
            case ParticleEmitter::Builtin::ring: {
                const double d = (r - 0.72) / 0.13;
                alpha = std::exp(-d * d) * std::clamp((1.0 - r) / pixel, 0.0, 1.0);
                break;
            }
            case ParticleEmitter::Builtin::star: {
                const double core = std::exp(-r * r / 0.02);
                const double glow = 0.35 * std::exp(-r * 4.5);
                const double fade_out = std::clamp(1.0 - r, 0.0, 1.0);
                const double rays = (std::exp(-std::abs(y) * 45.0) + std::exp(-std::abs(x) * 45.0)) * fade_out * fade_out;
                const double diagonal_a = std::abs(x - y) * 0.7071, diagonal_b = std::abs(x + y) * 0.7071;
                const double diagonals = 0.35 * (std::exp(-diagonal_a * 70.0) + std::exp(-diagonal_b * 70.0)) *
                                         std::pow(fade_out, 3.0);
                alpha = std::clamp(core + glow + rays + diagonals, 0.0, 1.0) * std::clamp((1.0 - r) / pixel, 0.0, 1.0);
                break;
            }
            case ParticleEmitter::Builtin::smoke: {
                double noise = 0.0, amplitude = 0.5, frequency = 2.5;
                for (int octave = 0; octave < 4; ++octave) {
                    noise += amplitude * perlin({x * frequency + 7.1, y * frequency + 3.3, 0.5}, 11U);
                    amplitude *= 0.5;
                    frequency *= 2.0;
                }
                const double edge = std::clamp(1.0 - (r + noise * 0.35), 0.0, 1.0);
                alpha = std::pow(edge * edge * (3.0 - 2.0 * edge), 1.3) * (0.75 + 0.5 * noise);
                // Lit from above: puffs are lighter on top, like a sphere.
                shade = std::clamp(0.78 + 0.22 * y + 0.25 * noise, 0.45, 1.0);
                alpha = std::clamp(alpha, 0.0, 1.0);
                break;
            }
            case ParticleEmitter::Builtin::spark: {
                // A streak along the sprite's height, brightest in the middle.
                const double across = x / 0.16;
                const double along = std::clamp(1.0 - std::abs(y), 0.0, 1.0);
                alpha = std::exp(-across * across) * std::pow(along, 0.8);
                break;
            }
            case ParticleEmitter::Builtin::square:
                alpha = std::clamp((0.96 - std::max(std::abs(x), std::abs(y))) / pixel + 0.5, 0.0, 1.0);
                break;
            }
            const auto at = (static_cast<std::size_t>(row) * side + column) * 4U;
            const auto value = static_cast<std::uint8_t>(std::lround(std::clamp(shade, 0.0, 1.0) * 255.0));
            texture->rgba[at] = texture->rgba[at + 1U] = texture->rgba[at + 2U] = value;
            texture->rgba[at + 3U] = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0));
        }
    made[index] = texture;
    return made[index];
}

ParticleSystem::ParticleSystem() = default;
ParticleSystem::~ParticleSystem() = default;

void ParticleSystem::set_root(const std::filesystem::path& root) { images_.set_root(root); }

void ParticleSystem::reset() {
    states_.clear();
    order_.clear();
    sub_events_.clear();
    collision_turn_ = 0U;
    list_ = {};
}

ParticleSystem::State* ParticleSystem::state_for(const Scene& scene, const Entity entity) {
    const auto* record = scene.get(entity);
    if (!record || !record->particle_emitter) return nullptr;
    auto& slot = states_[entity.packed()];
    if (!slot) {
        slot = std::make_unique<State>();
        slot->entity = entity;
        slot->settings = *record->particle_emitter;
        order_.push_back(entity.packed());
    }
    return slot.get();
}

// Starts a cycle from the beginning, reseeding so a replay looks the same as the first play.
void ParticleSystem::begin(State& state, const bool prewarm) {
    const auto& s = state.settings;
    ++state.plays;
    const std::uint64_t seed = s.seed != 0 ? static_cast<std::uint64_t>(s.seed) : 0x9E37U + state.entity.index;
    state.random.seed(seed * 0x2545F4914F6CDD1DULL + state.plays, seed);
    state.playing = true;
    state.stopped_by_hand = false;
    state.time = 0.0;
    state.cycle = 0U;
    state.emit_carry = 0.0;
    state.distance_carry = 0.0;
    state.finished_for = 0.0;
    if (!prewarm || !s.looping) return;
    // Simulate the tail of a cycle, long enough for the oldest particles to be as old as they
    // would be after a whole one, so the emitter starts full.
    const double span = std::min({s.duration, s.lifetime.max, maximum_prewarm_seconds});
    state.time = s.start_delay + s.duration - span;
    state.previous_world = state.world;
    for (double done = 0.0; done + 1e-9 < span; done += prewarm_step)
        advance(state, std::min(prewarm_step, span - done), nullptr);
}

// Emits `count` particles, the first `first_fraction` of the way through this step and each
// `spacing` further, from the shape at the node's blended transform. Each is aged by the part of
// the step it has already lived, so a stream from a moving node stays even.
void ParticleSystem::spawn(State& state, const std::size_t count, const double first_fraction, const double spacing,
                           const bool use_previous) {
    const auto& s = state.settings;
    const bool world_space = s.simulation_space == ParticleEmitter::Space::world;
    const auto capacity = static_cast<std::size_t>(std::max(0, s.max_particles));
    const auto shape = compose(s.shape_offset, s.shape_rotation, {1.0, 1.0, 1.0});
    for (std::size_t n = 0; n < count && state.particles.size() < capacity; ++n) {
        const double fraction = std::clamp(first_fraction + spacing * static_cast<double>(n), 0.0, 1.0);
        auto& random = state.random;
        Vec3 position{};
        Vec3 direction{0.0, 1.0, 0.0};
        const double thickness = std::clamp(s.radius_thickness, 0.0, 1.0);
        const double arc = std::clamp(s.arc, 0.0, 360.0) * to_radians;
        // A radius between the inner edge the thickness leaves and the rim, even over the area
        // (power 2) or the volume (power 3).
        const auto radius = [&](double power) {
            const double inner = std::pow(1.0 - thickness, power);
            return s.radius * std::pow(lerp(inner, 1.0, random.uniform()), 1.0 / power);
        };
        switch (s.shape) {
        case ParticleEmitter::Shape::point:
            direction = random.unit();
            break;
        case ParticleEmitter::Shape::sphere:
        case ParticleEmitter::Shape::hemisphere: {
            const double around = random.uniform() * arc;
            const double up = s.shape == ParticleEmitter::Shape::hemisphere ? random.uniform() : random.between(-1.0, 1.0);
            const double flat = std::sqrt(std::max(0.0, 1.0 - up * up));
            direction = {flat * std::cos(around), up, flat * std::sin(around)};
            position = direction * radius(3.0);
            break;
        }
        case ParticleEmitter::Shape::cone: {
            const double around = random.uniform() * arc;
            const double spread = std::clamp(s.angle, 0.0, 90.0) * to_radians;
            const Vec3 outward{std::cos(around), 0.0, std::sin(around)};
            double tilt = 0.0;
            if (s.radius > 1e-9) {
                const double r = radius(2.0);
                position = outward * r;
                tilt = spread * (r / s.radius);
            } else {
                // A point cone: directions spread evenly over the cap.
                tilt = std::acos(lerp(1.0, std::cos(spread), random.uniform()));
            }
            direction = normalized(Vec3{0.0, std::cos(tilt), 0.0} + outward * std::sin(tilt), {0.0, 1.0, 0.0});
            break;
        }
        case ParticleEmitter::Shape::box: {
            const Vec3 half = s.box_size * 0.5;
            position = {random.between(-half.x, half.x), random.between(-half.y, half.y), random.between(-half.z, half.z)};
            if (random.uniform() >= thickness) {
                // Onto a face, chosen by its area.
                const double xy = s.box_size.x * s.box_size.y, yz = s.box_size.y * s.box_size.z,
                             xz = s.box_size.x * s.box_size.z;
                const double pick = random.uniform() * std::max(xy + yz + xz, 1e-12);
                const double side = random.uniform() < 0.5 ? -1.0 : 1.0;
                if (pick < xy) position.z = half.z * side;
                else if (pick < xy + yz) position.x = half.x * side;
                else position.y = half.y * side;
            }
            break;
        }
        case ParticleEmitter::Shape::circle: {
            const double around = random.uniform() * arc;
            const Vec3 outward{std::cos(around), 0.0, std::sin(around)};
            position = outward * radius(2.0);
            direction = outward;
            break;
        }
        case ParticleEmitter::Shape::edge:
            position = {random.between(-s.radius, s.radius), 0.0, 0.0};
            break;
        }
        if (s.spherize > 0.0)
            direction = normalized(lerp(direction, normalized(position, random.unit()), s.spherize), direction);
        if (s.direction_randomness > 0.0)
            direction = normalized(lerp(direction, random.unit(), s.direction_randomness), direction);
        position = apply(shape, position);
        direction = rotate(shape, direction);

        Particle particle;
        particle.lifetime = std::max(0.01, random.between(s.lifetime));
        const double speed = random.between(s.speed);
        particle.size = static_cast<float>(random.between(s.size));
        particle.rotation = static_cast<float>(random.between(s.rotation) * to_radians);
        particle.spin = static_cast<float>(random.between(s.angular_velocity) * to_radians);
        particle.random = static_cast<float>(random.uniform());
        const double mix = s.random_color ? random.uniform() : 0.0;
        particle.color = {srgb_to_linear(lerp(s.color.r, s.color_alt.r, mix)),
                          srgb_to_linear(lerp(s.color.g, s.color_alt.g, mix)),
                          srgb_to_linear(lerp(s.color.b, s.color_alt.b, mix)),
                          static_cast<float>(lerp(s.color.a, s.color_alt.a, mix))};
        if (world_space) {
            // Between where the node was at the start of the step and where it is now.
            const auto& from = use_previous ? state.previous_world : state.world;
            const Vec3 start = apply(from, position), end = apply(state.world, position);
            particle.position = lerp(start, end, fraction);
            const Vec3 axis = normalized(lerp(rotate(from, direction), rotate(state.world, direction), fraction),
                                         {0.0, 1.0, 0.0});
            particle.velocity = axis * speed + state.node_velocity * s.inherit_velocity;
        } else {
            particle.position = position;
            particle.velocity = normalized(direction, {0.0, 1.0, 0.0}) * speed;
        }
        particle.previous = particle.position;
        particle.birth_step = static_cast<float>(1.0 - fraction);
        state.particles.push_back(particle);
        if (s.sub_emitter_trigger == ParticleEmitter::Trigger::birth && state.sub_target) {
            const Vec3 world_position = world_space ? particle.position : apply(state.world, particle.position);
            const Vec3 world_velocity = world_space ? particle.velocity : rotate(state.world, particle.velocity);
            sub_events_.push_back({*state.sub_target, world_position, world_velocity * s.sub_emitter_inherit,
                                   s.sub_emitter_count});
        }
    }
}

void ParticleSystem::advance(State& state, const double delta_seconds, const Raycast* raycast) {
    if (state.paused) return;
    const auto& s = state.settings;
    const double dt = delta_seconds * s.simulation_speed;
    if (dt <= 0.0) return;
    const bool world_space = s.simulation_space == ParticleEmitter::Space::world;
    const std::size_t born_before = state.particles.size();

    // Emission over this step's slice of the emitter's clock.
    if (state.playing && !state.orphan) {
        const double before = state.time - s.start_delay;
        state.time += dt;
        const double after = state.time - s.start_delay;
        if (after > 0.0) {
            const double from = std::max(before, 0.0);
            const double to = s.looping ? after : std::min(after, s.duration);
            const auto fraction_of = [&](double clock) { return std::clamp((clock - before) / dt, 0.0, 1.0); };
            if (to > from) {
                // Rate: evenly spaced through the slice, carrying the remainder to the next step.
                // A hair over, so a rate that fills whole particles per step never loses one to
                // rounding.
                const double wanted = s.rate * (to - from) + state.emit_carry;
                const auto count = static_cast<std::size_t>(std::floor(wanted + 1e-9));
                state.emit_carry = wanted - static_cast<double>(count);
                if (count > 0U) {
                    const double spacing = (to - from) / static_cast<double>(count);
                    spawn(state, count, fraction_of(from + spacing * 0.5), spacing / dt, true);
                }
                // Bursts: every repeat time that falls in the slice, cycle by cycle. Cycles are
                // counted in whole numbers, so a slice ending on a cycle's boundary never stalls.
                for (auto cycle = static_cast<std::int64_t>(std::floor(from / s.duration + 1e-9));; ++cycle) {
                    const double cycle_start = static_cast<double>(cycle) * s.duration;
                    if (cycle_start >= to) break;
                    const double low = std::max(from, cycle_start);
                    const double high = std::min(to, cycle_start + s.duration);
                    state.cycle = static_cast<std::uint32_t>(std::clamp<std::int64_t>(cycle, 0, 4000000000));
                    for (const auto& burst : s.bursts) {
                        for (std::int64_t repeat = 0; burst.cycles == 0 || repeat < burst.cycles; ++repeat) {
                            const double at = cycle_start + burst.time + static_cast<double>(repeat) * burst.interval;
                            if (at >= high || burst.time + static_cast<double>(repeat) * burst.interval >= s.duration)
                                break;
                            if (at < low) continue;
                            if (burst.probability < 1.0 && state.random.uniform() >= burst.probability) continue;
                            spawn(state, static_cast<std::size_t>(std::max(0, burst.count)), fraction_of(at), 0.0, true);
                        }
                    }
                }
            }
            // Over distance, spread along the path the node took this step.
            if (s.rate_over_distance > 0.0 && state.has_world) {
                const double moved = length(state.world.t - state.previous_world.t);
                const double wanted = moved * s.rate_over_distance + state.distance_carry;
                const auto count = static_cast<std::size_t>(std::floor(wanted + 1e-9));
                state.distance_carry = wanted - static_cast<double>(count);
                if (count > 0U)
                    spawn(state, count, 0.5 / static_cast<double>(count), 1.0 / static_cast<double>(count), true);
            }
            if (!s.looping && after >= s.duration) state.playing = false;
        }
    }

    // Movement. Forces act in world space; a local-space emitter feels them turned into its own.
    const Affine to_local = world_space ? Affine{} : inverse(state.world);
    Vec3 acceleration = s.acceleration + Vec3{0.0, -standard_gravity * s.gravity, 0.0};
    if (!world_space) acceleration = rotate(to_local, acceleration);
    const double keep = std::exp(-s.drag * dt);
    const bool noisy = s.noise_strength > 0.0;
    const auto noise_seed = static_cast<std::uint32_t>(s.seed) * 7919U + 17U;
    const Vec3 scroll{0.0, state.time * s.noise_scroll, state.time * s.noise_scroll * 0.37};
    const bool plane = s.collision == ParticleEmitter::Collision::plane;
    const bool world_hits = s.collision == ParticleEmitter::Collision::world && raycast && *raycast;
    const auto trigger = [&](const Particle& particle, ParticleEmitter::Trigger when) {
        if (s.sub_emitter_trigger != when || !state.sub_target) return;
        const Vec3 position = world_space ? particle.position : apply(state.world, particle.position);
        const Vec3 velocity = world_space ? particle.velocity : rotate(state.world, particle.velocity);
        sub_events_.push_back({*state.sub_target, position, velocity * s.sub_emitter_inherit, s.sub_emitter_count});
    };
    std::size_t kept = 0;
    const std::size_t total = state.particles.size();
    for (std::size_t index = 0; index < total; ++index) {
        auto particle = state.particles[index];
        // Particles born this step have lived only the part of it after their birth.
        const double step = index >= born_before ? dt * particle.birth_step : dt;
        particle.previous = particle.position;
        particle.age += step;
        if (particle.age >= particle.lifetime) {
            trigger(particle, ParticleEmitter::Trigger::death);
            continue;
        }
        const double t = particle.age / particle.lifetime;
        particle.velocity = (particle.velocity + acceleration * step) * (step == dt ? keep : std::exp(-s.drag * step));
        Vec3 motion = particle.velocity * s.speed_over_lifetime.sample(t) + s.velocity;
        if (noisy) {
            const Vec3 where = particle.position * s.noise_frequency + scroll;
            motion = motion + turbulence(where, noise_seed) * s.noise_strength;
        }
        particle.position = particle.position + motion * step;
        particle.rotation += static_cast<float>(particle.spin * step);
        bool hit = false;
        if (plane || world_hits) {
            Vec3 now = world_space ? particle.position : apply(state.world, particle.position);
            Vec3 velocity = world_space ? particle.velocity : rotate(state.world, particle.velocity);
            Vec3 normal{0.0, 1.0, 0.0};
            if (plane) {
                const double floor = s.plane_height + s.collision_radius;
                if (now.y < floor && velocity.y <= 0.0) {
                    now.y = floor;
                    hit = true;
                }
            } else if ((index + collision_turn_) % collision_stride_ == 0U) {
                // With more colliding particles than rays, each is tested every few steps along
                // the path it took since its last test.
                const Vec3 was = now - velocity * (step * static_cast<double>(collision_stride_));
                const Vec3 path = now - was;
                const double distance = length(path);
                if (distance > 1e-9) {
                    const auto result = (*raycast)(was, path * (1.0 / distance), distance + s.collision_radius);
                    if (result.hit) {
                        normal = normalized(result.normal, {0.0, 1.0, 0.0});
                        now = result.point + normal * s.collision_radius;
                        hit = dot(velocity, normal) < 0.0;
                    }
                }
            }
            if (hit) {
                const Vec3 into = normal * dot(velocity, normal);
                const Vec3 along = velocity - into;
                velocity = along * (1.0 - s.friction) - into * s.bounce;
                particle.position = world_space ? now : apply(to_local, now);
                particle.velocity = world_space ? velocity : rotate(to_local, velocity);
                particle.age += s.lifetime_loss * particle.lifetime;
                trigger(particle, ParticleEmitter::Trigger::collision);
                if (particle.age >= particle.lifetime) {
                    trigger(particle, ParticleEmitter::Trigger::death);
                    continue;
                }
            }
        }
        state.particles[kept++] = particle;
    }
    state.particles.resize(kept);
}

void ParticleSystem::refresh(const Scene& scene, const Entity entity, State& state, const bool is_sub) {
    state.settings = *scene.get(entity)->particle_emitter;
    state.sub = is_sub;
    state.seen = true;
    state.orphan = false;
    state.orphan_for = 0.0;
    const auto world = world_of(scene, entity).value_or(Affine{});
    state.previous_world = state.has_world ? state.world : world;
    state.world = world;
    state.has_world = true;
}

void ParticleSystem::update(const Scene& scene, const double delta_seconds, const bool game, const Raycast& raycast) {
    RELAY_PROFILE_SCOPE("Particles");
    game_ = game;
    for (auto& [key, state] : states_) state->seen = false;
    // Sub emitters: a named direct child with an emitter of its own.
    std::unordered_map<std::uint64_t, std::uint64_t> sub_of;
    std::set<std::uint64_t> subs;
    const auto entities = scene.entities();
    for (const auto entity : entities) {
        const auto* record = scene.get(entity);
        if (!record->particle_emitter || record->particle_emitter->sub_emitter.empty()) continue;
        for (const auto child : entities) {
            const auto* candidate = scene.get(child);
            if (child != entity && candidate->parent == entity && candidate->particle_emitter &&
                candidate->name == record->particle_emitter->sub_emitter) {
                sub_of[entity.packed()] = child.packed();
                subs.insert(child.packed());
                break;
            }
        }
    }
    for (const auto entity : entities) {
        if (!scene.get(entity)->particle_emitter) continue;
        const auto existing = states_.find(entity.packed());
        const bool fresh = existing == states_.end() || existing->second->orphan;
        auto* state = state_for(scene, entity);
        const bool is_sub = subs.contains(entity.packed());
        refresh(scene, entity, *state, is_sub);
        const auto found = sub_of.find(entity.packed());
        state->sub_target = found == sub_of.end() ? std::nullopt : std::optional<std::uint64_t>(found->second);
        if (fresh && !is_sub && (!game || state->settings.play_on_start)) begin(*state, state->settings.prewarm);
        state->node_velocity = delta_seconds > 0.0 ? (state->world.t - state->previous_world.t) * (1.0 / delta_seconds)
                                                   : Vec3{};
    }
    // Nodes that are gone, or lost their emitter: their particles finish in place during the game
    // and vanish in the editor.
    for (auto& [key, state] : states_)
        if (!state->seen) {
            state->orphan = true;
            state->playing = false;
            state->sub_target.reset();
        }
    // Particles take turns at world collisions when there are more than the ray budget.
    std::size_t colliding = 0;
    for (const auto& [key, state] : states_)
        if (state->settings.collision == ParticleEmitter::Collision::world) colliding += state->particles.size();
    collision_stride_ = std::max<std::size_t>(1U, (colliding + maximum_collision_rays - 1U) / maximum_collision_rays);
    ++collision_turn_;
    for (const auto key : order_) {
        auto& state = *states_[key];
        if (state.orphan && !game) {
            state.particles.clear();
            continue;
        }
        advance(state, delta_seconds, game ? &raycast : nullptr);
        if (state.orphan) state.orphan_for += delta_seconds;
        // Editor previews replay one-shot effects once they have played out.
        if (!game && !state.sub && !state.orphan && !state.playing && !state.paused && state.particles.empty()) {
            state.finished_for += delta_seconds;
            if (state.finished_for >= preview_replay_seconds && !state.stopped_by_hand) begin(state, state.settings.prewarm);
        }
    }
    // Sub emitters fire where their parents' particles were born, hit or died this step.
    auto events = std::move(sub_events_);
    sub_events_.clear();
    for (const auto& event : events) {
        const auto found = states_.find(event.target);
        if (found == states_.end() || found->second->orphan) continue;
        auto& target = *found->second;
        const auto saved_world = target.world, saved_previous = target.previous_world;
        const auto saved_velocity = target.node_velocity;
        const auto saved_inherit = target.settings.inherit_velocity;
        // The child's own shape, turned as the child is, placed at the event.
        target.world.t = event.position;
        target.previous_world = target.world;
        target.node_velocity = event.velocity;
        target.settings.inherit_velocity = 1.0;
        const bool local = target.settings.simulation_space == ParticleEmitter::Space::local;
        const std::size_t first = target.particles.size();
        spawn(target, static_cast<std::size_t>(std::max(0, event.count)), 1.0, 0.0, false);
        target.world = saved_world;
        target.previous_world = saved_previous;
        target.node_velocity = saved_velocity;
        target.settings.inherit_velocity = saved_inherit;
        if (local) {
            // Spawned in the child's space as if it sat at the event: move them there.
            const auto to_child = inverse(target.world);
            Affine at_event = target.world;
            at_event.t = event.position;
            for (std::size_t index = first; index < target.particles.size(); ++index) {
                auto& particle = target.particles[index];
                particle.position = apply(to_child, apply(at_event, particle.position));
                particle.previous = particle.position;
                particle.velocity = particle.velocity + rotate(to_child, event.velocity);
            }
        }
    }
    // Forget emitters with nothing left to show.
    for (auto it = order_.begin(); it != order_.end();) {
        const auto found = states_.find(*it);
        const bool done = found->second->orphan &&
                          (found->second->particles.empty() || found->second->orphan_for > orphan_lifetime);
        if (done) {
            states_.erase(found);
            it = order_.erase(it);
        } else {
            ++it;
        }
    }
    // Keep the whole system within its budget, trimming the oldest particles of the largest.
    std::size_t total = particle_count();
    while (total > maximum_total_particles) {
        State* largest = nullptr;
        for (auto& [key, state] : states_)
            if (!largest || state->particles.size() > largest->particles.size()) largest = state.get();
        const auto excess = std::min(total - maximum_total_particles, largest->particles.size());
        largest->particles.erase(largest->particles.begin(), largest->particles.begin() + static_cast<std::ptrdiff_t>(excess));
        total -= excess;
    }
}

bool ParticleSystem::play(const Scene& scene, const Entity entity, const bool restart) {
    auto* state = state_for(scene, entity);
    if (!state) return false;
    if (!state->has_world) refresh(scene, entity, *state, false);
    state->paused = false;
    if (restart) state->particles.clear();
    if (restart || !state->playing) begin(*state, state->settings.prewarm && restart);
    return true;
}

bool ParticleSystem::stop(const Entity entity, const bool clear) {
    const auto found = states_.find(entity.packed());
    if (found == states_.end()) return false;
    found->second->playing = false;
    found->second->stopped_by_hand = true;
    if (clear) found->second->particles.clear();
    return true;
}

bool ParticleSystem::set_paused(const Entity entity, const bool paused) {
    const auto found = states_.find(entity.packed());
    if (found == states_.end()) return false;
    found->second->paused = paused;
    return true;
}

std::size_t ParticleSystem::emit(const Scene& scene, const Entity entity, const std::size_t count) {
    auto* state = state_for(scene, entity);
    if (!state) return 0U;
    if (!state->has_world) refresh(scene, entity, *state, false);
    const auto before = state->particles.size();
    spawn(*state, std::min<std::size_t>(count, maximum_particles_per_emitter), 1.0, 0.0, false);
    return state->particles.size() - before;
}

std::optional<ParticleEmitterStatus> ParticleSystem::status(const Entity entity) const {
    const auto found = states_.find(entity.packed());
    if (found == states_.end()) return std::nullopt;
    const auto& state = *found->second;
    return ParticleEmitterStatus{state.entity, state.playing, state.paused, state.sub, state.orphan,
                                 state.particles.size(), state.time, state.cycle};
}

std::vector<ParticleEmitterStatus> ParticleSystem::statuses() const {
    std::vector<ParticleEmitterStatus> result;
    for (const auto key : order_)
        if (const auto found = states_.find(key); found != states_.end())
            result.push_back(*status(found->second->entity));
    return result;
}

std::size_t ParticleSystem::particle_count() const {
    std::size_t total = 0;
    for (const auto& [key, state] : states_) total += state->particles.size();
    return total;
}

std::string ParticleSystem::status_json(const Scene& scene, const std::optional<Entity> only) const {
    std::ostringstream out;
    out << "{\"mode\":\"" << (game_ ? "game" : "editor") << "\",\"particles\":" << particle_count()
        << ",\"emitters\":[";
    bool first = true;
    for (const auto& item : statuses()) {
        if (only && item.entity != *only) continue;
        const auto* record = scene.get(item.entity);
        out << (first ? "" : ",") << "{\"entity\":\"" << item.entity.to_string() << "\",\"name\":\""
            << escape(record ? record->name : std::string{}) << "\",\"playing\":" << (item.playing ? "true" : "false")
            << ",\"paused\":" << (item.paused ? "true" : "false")
            << ",\"sub_emitter\":" << (item.sub_emitter ? "true" : "false")
            << ",\"orphan\":" << (item.orphan ? "true" : "false") << ",\"particles\":" << item.particles
            << ",\"time\":" << number_json(item.time) << ",\"cycle\":" << item.cycle << '}';
        first = false;
    }
    out << "]}";
    return out.str();
}

const ParticleRenderList& ParticleSystem::render_list(const ParticleView& view, const double alpha) {
    list_.batches.clear();
    list_.sprites.clear();
    const double blend = std::clamp(alpha, 0.0, 1.0);
    const Vec3 forward = normalized(view.camera_forward, {0.0, 0.0, -1.0});
    std::vector<std::size_t> order;
    for (const auto key : order_) {
        const auto found = states_.find(key);
        if (found == states_.end() || found->second->particles.empty()) continue;
        const auto& state = *found->second;
        const auto& s = state.settings;
        const bool world_space = s.simulation_space == ParticleEmitter::Space::world;
        ParticleBatch batch;
        batch.entity = state.entity;
        if (!s.texture.empty()) {
            std::string error;
            batch.texture = images_.image(s.texture, &error);
            if (!batch.texture && reported_.insert(s.texture).second)
                warnings_.push_back(s.texture + ": " + (error.empty() ? "cannot load the image" : error));
        }
        if (!batch.texture) batch.texture = builtin_particle_texture(s.builtin_texture);
        batch.blend = s.blend;
        batch.alignment = s.alignment;
        batch.lit = s.lit;
        batch.soft_distance = static_cast<float>(s.soft_distance);
        batch.sheet_columns = static_cast<std::uint32_t>(std::max(1, s.sheet_columns));
        batch.sheet_rows = static_cast<std::uint32_t>(std::max(1, s.sheet_rows));
        batch.sheet_blend = s.sheet_blend;
        const Vec3 axis_x = normalized(state.world.x, {1.0, 0.0, 0.0});
        const Vec3 axis_y = normalized(state.world.y, {0.0, 1.0, 0.0});
        batch.local_x = {static_cast<float>(axis_x.x), static_cast<float>(axis_x.y), static_cast<float>(axis_x.z)};
        batch.local_y = {static_cast<float>(axis_y.x), static_cast<float>(axis_y.y), static_cast<float>(axis_y.z)};
        batch.first = static_cast<std::uint32_t>(list_.sprites.size());
        const double frames = static_cast<double>(batch.sheet_columns * batch.sheet_rows);
        Vec3 centre{};
        std::vector<double> keys;
        keys.reserve(state.particles.size());
        for (const auto& particle : state.particles) {
            const double t = std::clamp(particle.age / particle.lifetime, 0.0, 1.0);
            Vec3 position = lerp(particle.previous, particle.position, blend);
            Vec3 velocity = particle.velocity;
            if (!world_space) {
                position = apply(state.world, position);
                velocity = rotate(state.world, velocity);
            }
            ParticleSprite sprite;
            sprite.position = {static_cast<float>(position.x), static_cast<float>(position.y), static_cast<float>(position.z)};
            const double size = particle.size * s.size_over_lifetime.sample(t);
            double height = size;
            if (s.alignment == ParticleEmitter::Alignment::stretched) {
                const Vec3 direction = normalized(velocity, {0.0, 1.0, 0.0});
                height = size * s.stretch_length + length(velocity) * s.stretch_speed;
                sprite.axis = {static_cast<float>(direction.x), static_cast<float>(direction.y),
                               static_cast<float>(direction.z)};
            }
            sprite.width = static_cast<float>(size * s.aspect);
            sprite.height = static_cast<float>(height);
            const auto tint = s.color_over_lifetime.sample(t);
            const auto brightness = static_cast<float>(s.emission);
            sprite.color = {particle.color[0] * srgb_to_linear(tint.r) * brightness,
                            particle.color[1] * srgb_to_linear(tint.g) * brightness,
                            particle.color[2] * srgb_to_linear(tint.b) * brightness,
                            std::clamp(particle.color[3] * static_cast<float>(tint.a), 0.0F, 1.0F)};
            sprite.rotation = particle.rotation;
            double frame = 0.0;
            const double offset = s.sheet_random_start ? std::floor(particle.random * frames) : 0.0;
            switch (s.sheet_mode) {
            case ParticleEmitter::SheetMode::over_lifetime: frame = t * frames * s.sheet_cycles + offset; break;
            case ParticleEmitter::SheetMode::fps: frame = particle.age * s.sheet_fps + offset; break;
            case ParticleEmitter::SheetMode::random: frame = std::floor(particle.random * frames); break;
            }
            // The last frame of a single pass holds rather than wrapping to the first.
            frame = s.sheet_mode == ParticleEmitter::SheetMode::over_lifetime && s.sheet_cycles <= 1.0 && !s.sheet_random_start
                        ? std::min(frame, frames - 1.0)
                        : std::fmod(frame, frames);
            sprite.frame = static_cast<float>(frame);
            sprite.age = static_cast<float>(t);
            if (sprite.width <= 0.0F || sprite.height <= 0.0F || sprite.color[3] <= 0.0F) continue;
            centre = centre + position;
            switch (s.sort) {
            case ParticleEmitter::Sort::distance: keys.push_back(dot(position - view.camera_position, forward)); break;
            case ParticleEmitter::Sort::oldest_first: keys.push_back(particle.age); break;
            case ParticleEmitter::Sort::youngest_first: keys.push_back(-particle.age); break;
            case ParticleEmitter::Sort::none: keys.push_back(0.0); break;
            }
            list_.sprites.push_back(sprite);
        }
        batch.count = static_cast<std::uint32_t>(list_.sprites.size()) - batch.first;
        if (batch.count == 0U) continue;
        if (s.sort != ParticleEmitter::Sort::none) {
            // Larger keys draw first: farther, or older.
            order.resize(batch.count);
            std::iota(order.begin(), order.end(), std::size_t{0});
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] > keys[b]; });
            std::vector<ParticleSprite> sorted(batch.count);
            for (std::size_t index = 0; index < order.size(); ++index) sorted[index] = list_.sprites[batch.first + order[index]];
            std::copy(sorted.begin(), sorted.end(), list_.sprites.begin() + batch.first);
        }
        centre = centre * (1.0 / static_cast<double>(batch.count));
        batch.depth = static_cast<float>(dot(centre - view.camera_position, forward));
        list_.batches.push_back(std::move(batch));
    }
    std::stable_sort(list_.batches.begin(), list_.batches.end(),
                     [](const ParticleBatch& a, const ParticleBatch& b) { return a.depth > b.depth; });
    return list_;
}

std::vector<std::string> ParticleSystem::take_warnings() {
    auto result = std::move(warnings_);
    warnings_.clear();
    return result;
}

} // namespace relay
