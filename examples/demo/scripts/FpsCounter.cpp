#include "relay_script.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// The showcase's frame rate readout. Put it on a Label in the HUD canvas; it writes the rate the
// game is drawing at into that label a few times a second, so the number stays readable.
//
// Scripts update at a fixed 60 Hz whatever the display does, so the rate comes from
// relay::world::frame_rate(), which counts drawn frames. The label turns amber below `warn_below` and red
// below `bad_below`, and `visible` lets a project hide it without removing the script.
class FpsCounter : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("visible", visible);
        p.add("refresh_seconds", refresh_seconds);
        p.add("warn_below", warn_below);
        p.add("bad_below", bad_below);
    }

    void on_start() override {
        label = self();
        label.set_visible(visible);
        next_refresh = relay::world::time();
    }

    void on_update(double) override {
        if (!visible || relay::world::time() < next_refresh) return;
        next_refresh = relay::world::time() + std::max(0.05, refresh_seconds);
        const double rate = relay::world::frame_rate();
        if (rate <= 0.0) return;
        label.set_text("FPS " + std::to_string(static_cast<int>(std::lround(rate))));
        // Green while smooth, amber and red as it drops.
        if (rate < bad_below) label.set_ui("label.color", {1.0, 0.35, 0.3, 1.0});
        else if (rate < warn_below) label.set_ui("label.color", {1.0, 0.78, 0.3, 1.0});
        else label.set_ui("label.color", {0.55, 0.95, 0.55, 1.0});
    }

private:
    relay::Entity label;
    bool visible = true;
    double refresh_seconds = 0.25;
    double warn_below = 60.0;
    double bad_below = 30.0;
    double next_refresh = 0.0;
};
RELAY_BEHAVIOUR(FpsCounter)
