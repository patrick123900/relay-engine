#include "relay_script.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Plays the node's audio source when its physics body hits something, louder the harder the hit.
// The strength of a hit is how much the body's velocity changed in that game step, so a ball
// dropped from higher thumps louder, and bodies resting against each other stay quiet. Each hit
// varies the pitch a little so a pile of crates does not sound like one sample repeated.
class ImpactSound : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("quiet_speed", quiet_speed);
        p.add("loud_speed", loud_speed);
        p.add("quietest_db", quietest_db);
        p.add("pitch_spread", pitch_spread);
        p.add("cooldown", cooldown);
    }

    void on_update(double dt) override {
        // Scripts update before physics steps, so this is the velocity going into the step.
        before = self().velocity();
        since_last += dt;
    }

    void on_contact_begin(relay::Entity) override {
        const double change = (self().velocity() - before).length();
        if (change < quiet_speed || since_last < cooldown) return;
        since_last = 0.0;
        const double strength =
            std::clamp((change - quiet_speed) / std::max(loud_speed - quiet_speed, 1e-6), 0.0, 1.0);
        self().play_sound(quietest_db * (1.0 - strength), 1.0 + pitch_spread * variation());
    }

private:
    // A repeatable value in [-1, 1] from this node and the current frame.
    double variation() const {
        std::uint64_t mix = self().handle() * 0x9E3779B97F4A7C15ULL ^ relay::world::frame();
        mix ^= mix >> 31U;
        mix *= 0xBF58476D1CE4E5B9ULL;
        mix ^= mix >> 29U;
        return static_cast<double>(mix % 2001U) / 1000.0 - 1.0;
    }

    double quiet_speed = 0.6;   // m/s of velocity change below which a touch is silent.
    double loud_speed = 8.0;    // m/s at which a hit plays at the source's full volume.
    double quietest_db = -30.0; // Volume of the softest audible hit, relative to the source.
    double pitch_spread = 0.08; // Pitch varies by up to this fraction either way.
    double cooldown = 0.06;     // Seconds between sounds, so one landing is one thump.
    relay::Vec3 before;
    double since_last = 1.0;
};
RELAY_BEHAVIOUR(ImpactSound)
