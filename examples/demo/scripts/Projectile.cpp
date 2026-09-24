#include "relay_script.hpp"

// Removes its entity a while after it appears, so shots from the First Person Controller do not
// pile up. The demo's Ball template carries it.
class Projectile : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override { p.add("lifetime", lifetime); }

    void on_update(double dt) override {
        age += dt;
        if (age >= lifetime) self().destroy();
    }

private:
    double lifetime = 6.0; // Seconds.
    double age = 0.0;
};
RELAY_BEHAVIOUR(Projectile)
