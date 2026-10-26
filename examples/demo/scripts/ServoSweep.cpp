#include "relay_script.hpp"

#include <cmath>

// Sweeps a hinge between two angles by moving its servo target every half period. The hinge's
// motor must be a servo (Motor mode: Servo) with a motor force strong enough to carry the load.
class ServoSweep : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("swing", swing);
        p.add("period", period);
    }

    void on_update(double dt) override {
        time += dt;
        const double target = std::fmod(time, period) < period * 0.5 ? swing : -swing;
        if (target == last) return;
        last = target;
        self().set_joint_target(target);
    }

private:
    double swing = 70.0;  // Degrees either side of the start.
    double period = 4.0;  // Seconds for a full sweep there and back.
    double time = 0.0;
    double last = 1e9;
};
RELAY_BEHAVIOUR(ServoSweep)
