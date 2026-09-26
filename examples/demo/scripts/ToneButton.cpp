#include "relay_script.hpp"

#include <array>
#include <cmath>

// A button that plays the next note of a scale from the node's audio source. Look at it and press
// interact (E, or X on a gamepad) within reach, or hit it with a thrown ball. The button dips
// while it sounds, and the music ducks under the note for a moment.
class ToneButton : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("reach", reach);
        p.add("travel", travel);
        p.add("player_name", player_name);
        p.add("camera_name", camera_name);
        p.add("duck_bus", duck_bus);
        p.add("duck_db", duck_db);
    }

    void on_start() override {
        rest = self().position();
        player = relay::world::find(player_name);
        if (player) camera = player.child(camera_name);
        if (!camera)
            relay::world::warn("no " + player_name + " with a " + camera_name +
                               " child, so only thrown balls press this button");
    }

    void on_update(double dt) override {
        if (held > 0.0) {
            held -= dt;
            self().set_position(held > 0.0 ? rest - relay::Vec3{0.0, travel, 0.0} : rest);
        }
        if (ducked > 0.0) {
            ducked -= dt;
            if (ducked <= 0.0) relay::audio::set_bus_volume(duck_bus, music_volume, 1.0);
        }
        if (!camera || !relay::input::pressed("interact")) return;
        // Cameras look down -Z; the controller turns its camera by pitch (X) then yaw (Y).
        constexpr double radians = 3.14159265358979323846 / 180.0;
        const auto angles = camera.rotation();
        const double pitch = angles.x * radians, yaw = angles.y * radians;
        const relay::Vec3 forward{-std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                  -std::cos(pitch) * std::cos(yaw)};
        const auto hit = relay::world::raycast(camera.world_position(), forward, reach,
                                               0xffffffffu, player);
        if (hit && hit->entity == self()) press();
    }

    void on_contact_begin(relay::Entity other) override {
        if (other != player) press();
    }

private:
    void press() {
        // A major pentatonic scale, as multiples of the clip's own pitch.
        static constexpr std::array<double, 6> notes{1.0, 9.0 / 8.0, 5.0 / 4.0, 3.0 / 2.0,
                                                     5.0 / 3.0, 2.0};
        self().play_sound(0.0, notes[next % notes.size()]);
        ++next;
        held = 0.15;
        // Duck the music quickly, then bring it back once the note has rung.
        if (!duck_bus.empty()) {
            if (ducked <= 0.0) music_volume = relay::audio::bus_volume(duck_bus).value_or(0.0);
            relay::audio::set_bus_volume(duck_bus, music_volume + duck_db, 0.1);
            ducked = 0.8;
        }
    }

    double reach = 3.0;   // Metres from the camera.
    double travel = 0.04; // How far the button dips, in metres.
    std::string player_name = "First Person Controller";
    std::string camera_name = "Camera";
    std::string duck_bus = "Music"; // Turned down while a note rings; empty leaves it alone.
    double duck_db = -10.0;
    relay::Entity player, camera;
    double music_volume = 0.0;
    double ducked = 0.0;
    relay::Vec3 rest;
    double held = 0.0;
    std::size_t next = 0;
};
RELAY_BEHAVIOUR(ToneButton)
