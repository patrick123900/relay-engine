#include "relay_script.hpp"

#include <cmath>

// A pad that moves the soundtrack on to its next track, crossfading on the next bar so the
// change stays in time. Look at it and press interact (E, or X on a gamepad) within reach, or hit
// it with a thrown ball.
class MusicSwitch : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("music_name", music_name);
        p.add("reach", reach);
        p.add("player_name", player_name);
        p.add("camera_name", camera_name);
    }

    void on_start() override {
        rest = self().position();
        music = relay::world::find(music_name);
        player = relay::world::find(player_name);
        if (player) camera = player.child(camera_name);
        if (!music) relay::world::warn("no node named " + music_name + " to switch");
    }

    void on_update(double dt) override {
        if (held > 0.0) {
            held -= dt;
            self().set_position(held > 0.0 ? rest - relay::Vec3{0.0, 0.04, 0.0} : rest);
        }
        if (!camera || !relay::input::pressed("interact")) return;
        constexpr double radians = 3.14159265358979323846 / 180.0;
        const auto angles = camera.rotation();
        const double pitch = angles.x * radians, yaw = angles.y * radians;
        const relay::Vec3 forward{-std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                  -std::cos(pitch) * std::cos(yaw)};
        const auto hit = relay::world::raycast(camera.world_position(), forward, reach, 0xffffffffu, player);
        if (hit && hit->entity == self()) press();
    }

    void on_contact_begin(relay::Entity other) override {
        if (other != player) press();
    }

private:
    void press() {
        if (held > 0.0 || !music) return;
        music.play_music(-1, relay::audio::Sync::bar);
        held = 0.3;
    }

    std::string music_name = "Soundtrack";
    double reach = 3.0;
    std::string player_name = "First Person Controller";
    std::string camera_name = "Camera";
    relay::Entity music, player, camera;
    relay::Vec3 rest;
    double held = 0.0;
};
RELAY_BEHAVIOUR(MusicSwitch)
