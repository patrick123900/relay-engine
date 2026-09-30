#pragma once

// Particle simulation: every node with a particle emitter spawns, moves and retires its particles
// here, one fixed step at a time, deterministically for a given seed. During Run Game emitters
// follow their play_on_start setting and scripts or the protocol; in the editor every emitter
// previews continuously, so effects can be tuned while they play.
//
// Particles live apart from the scene: they are not saved, not undoable and never block edits.
// Renderers ask for a draw list of camera-facing sprites in world space, sorted for the view.

#include "relay/physics/collision.hpp"
#include "relay/scene/particles.hpp"
#include "relay/scene/scene.hpp"
#include "relay/ui/ui_font.hpp"
#include "relay/ui/ui_render.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace relay {

// One particle as the renderer draws it, laid out for a std430 storage buffer (64 bytes).
struct ParticleSprite {
    std::array<float, 3> position{}; // World space.
    float width{};
    std::array<float, 3> axis{};     // Stretched: the direction of travel, world space.
    float height{};
    std::array<float, 4> color{};    // Linear RGB times the emitter's brightness, straight alpha.
    float rotation{};                // Radians, about the view direction or the sprite's normal.
    float frame{};                   // Flipbook frame; the fraction blends into the next one.
    float age{};                     // 0 at birth, 1 at death.
    float padding{};
};
static_assert(sizeof(ParticleSprite) == 64U);

// One emitter's sprites in a draw list, with how to draw them.
struct ParticleBatch {
    Entity entity{};
    std::shared_ptr<const UiTexture> texture; // sRGB, straight alpha.
    ParticleEmitter::Blend blend{ParticleEmitter::Blend::alpha};
    ParticleEmitter::Alignment alignment{ParticleEmitter::Alignment::billboard};
    bool lit{};
    float soft_distance{};
    std::uint32_t sheet_columns{1};
    std::uint32_t sheet_rows{1};
    bool sheet_blend{};
    // Local alignment: the node's X and Y axes in world space, normalized.
    std::array<float, 3> local_x{1.0F, 0.0F, 0.0F};
    std::array<float, 3> local_y{0.0F, 1.0F, 0.0F};
    std::uint32_t first{};
    std::uint32_t count{};
    float depth{}; // Distance of the batch's centre along the view, for ordering batches.
};

struct ParticleRenderList {
    std::vector<ParticleBatch> batches; // Farthest first.
    std::vector<ParticleSprite> sprites;
    [[nodiscard]] bool empty() const { return sprites.empty(); }
};

// Where the view is, for sorting.
struct ParticleView {
    Vec3 camera_position{0.0, 0.0, 5.0};
    Vec3 camera_forward{0.0, 0.0, -1.0};
};

struct ParticleEmitterStatus {
    Entity entity{};
    bool playing{};   // Its clock runs and it emits.
    bool paused{};
    bool sub_emitter{}; // Fired by a parent's particles rather than on its own.
    bool orphan{};    // Its node is gone; the last particles are finishing.
    std::size_t particles{};
    double time{};    // Seconds since it started playing, at its simulation speed.
    std::uint32_t cycle{};
};

// A texture made in code, for emitters without one of their own.
[[nodiscard]] std::shared_ptr<const UiTexture> builtin_particle_texture(ParticleEmitter::Builtin builtin);

class ParticleSystem {
public:
    // A ray cast against the scene's colliders, for world collisions during Run Game.
    using Raycast = std::function<CollisionRaycast(Vec3 origin, Vec3 direction, double distance)>;
    static constexpr std::size_t maximum_total_particles = 1000000U;
    // World collisions cast about this many rays each step; with more colliding particles, each
    // is tested every few steps.
    static constexpr std::size_t maximum_collision_rays = 4096U;

    ParticleSystem();
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // The project folder that texture paths resolve against.
    void set_root(const std::filesystem::path& root);
    // Advances every emitter by one step. `game` is true during Run Game: emitters start as their
    // play_on_start says and collide with the world through `raycast`. Otherwise every emitter
    // previews, one-shot effects replaying a second after they finish.
    void update(const Scene& scene, double delta_seconds, bool game, const Raycast& raycast = {});
    // Forgets every emitter and particle, as Run Game and Stop Game do.
    void reset();

    // Starts (or restarts, when `restart`) an emitter's cycle. False without an emitter.
    bool play(const Scene& scene, Entity entity, bool restart = false);
    // Stops emitting; the particles alive finish their lives unless `clear` removes them now.
    bool stop(Entity entity, bool clear = false);
    bool set_paused(Entity entity, bool paused);
    // Emits `count` particles now from the emitter's shape, playing or not. Returns how many
    // were born, which max_particles can limit.
    std::size_t emit(const Scene& scene, Entity entity, std::size_t count);

    [[nodiscard]] std::optional<ParticleEmitterStatus> status(Entity entity) const;
    [[nodiscard]] std::vector<ParticleEmitterStatus> statuses() const;
    [[nodiscard]] std::size_t particle_count() const;
    // JSON for particles.status: every emitter, or one.
    [[nodiscard]] std::string status_json(const Scene& scene, std::optional<Entity> only = std::nullopt) const;

    // The sprites to draw this frame: positions blended `alpha` of the way from the previous step
    // to the current one, sorted for the view. The list stays valid until the next call.
    [[nodiscard]] const ParticleRenderList& render_list(const ParticleView& view, double alpha = 1.0);
    // Texture files that could not be used, each reported once.
    [[nodiscard]] std::vector<std::string> take_warnings();

    struct Particle;
    struct State;

private:
    State* state_for(const Scene& scene, Entity entity);
    void begin(State& state, bool prewarm);
    void advance(State& state, double delta_seconds, const Raycast* raycast);
    void spawn(State& state, std::size_t count, double first_fraction, double spacing, bool use_previous);
    void refresh(const Scene& scene, Entity entity, State& state, bool is_sub);

    std::unordered_map<std::uint64_t, std::unique_ptr<State>> states_;
    std::vector<std::uint64_t> order_; // First seen first, for deterministic updates.
    struct SubEvent {
        std::uint64_t target;
        Vec3 position;
        Vec3 velocity;
        std::int32_t count;
    };
    std::vector<SubEvent> sub_events_;
    std::size_t collision_turn_{};
    std::size_t collision_stride_{1};
    bool game_{};
    UiImageCache images_;
    std::set<std::string, std::less<>> reported_;
    std::vector<std::string> warnings_;
    ParticleRenderList list_;
};

} // namespace relay
