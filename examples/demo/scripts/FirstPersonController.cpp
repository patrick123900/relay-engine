#include "relay_script.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// First-person movement for the First Person Controller starter template.
//
// Put it on a node with a dynamic Physics body (rotation locked) and a Collider, with a child
// camera node named by camera_name at eye height. The body never turns: looking turns the
// camera, and walking follows where the camera faces.
//
// Input map names (Edit > Game Configuration > Input): move_x and move_y walk, look_x and look_y
// look with a gamepad, jump and sprint, and fire shoots. The mouse looks around while the game has
// input; turn on "Lock the mouse cursor" there so the cursor stays inside the window.
//
// Shooting spawns the ball_template project template just in front of the camera and launches it
// where the camera looks. The demo's Ball template sits on collider layer 2, which the player's
// collider mask leaves out, so balls never knock into the player who shot them.
class FirstPersonController : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("walk_speed", walk_speed);
        p.add("sprint_speed", sprint_speed);
        p.add("jump_speed", jump_speed);
        p.add("mouse_sensitivity", mouse_sensitivity);
        p.add("stick_look_speed", stick_look_speed);
        p.add("invert_y", invert_y);
        p.add("ground_distance", ground_distance);
        p.add("camera_name", camera_name);
        p.add("ball_template", ball_template);
        p.add("ball_speed", ball_speed);
    }

    void on_start() override {
        camera = self().child(camera_name);
        if (!camera) {
            relay::world::warn("no child node named " + camera_name + ", so looking around is off");
            return;
        }
        camera.make_active_camera();
        const auto rotation = camera.rotation();
        pitch = rotation.x;
        yaw = rotation.y;
    }

    void on_update(double dt) override {
        look(dt);
        move();
        if (relay::input::pressed("fire")) shoot();
    }

private:
    void look(double dt) {
        if (!camera) return;
        const auto mouse = relay::input::mouse_delta();
        const double vertical = invert_y ? -mouse.y : mouse.y;
        yaw -= mouse.x * mouse_sensitivity + relay::input::axis("look_x") * stick_look_speed * dt;
        pitch -= vertical * mouse_sensitivity;
        pitch += relay::input::axis("look_y") * stick_look_speed * dt * (invert_y ? -1.0 : 1.0);
        pitch = std::clamp(pitch, -89.0, 89.0);
        yaw = std::remainder(yaw, 360.0);
        camera.set_rotation({pitch, yaw, 0.0});
    }

    void move() {
        // Cameras look down -Z; yaw turns about +Y, so these are the camera's flat forward and right.
        const double heading = (self().rotation().y + yaw) * 3.14159265358979323846 / 180.0;
        const relay::Vec3 forward{-std::sin(heading), 0.0, -std::cos(heading)};
        const relay::Vec3 right{std::cos(heading), 0.0, -std::sin(heading)};
        const auto input = relay::input::vector("move_x", "move_y");
        const double speed = relay::input::held("sprint") ? sprint_speed : walk_speed;
        const auto walk = (right * input.x + forward * input.y) * speed;

        // Walking sets the horizontal velocity directly; gravity keeps the vertical part.
        auto velocity = self().velocity();
        velocity.x = walk.x;
        velocity.z = walk.z;
        if (relay::input::pressed("jump") && on_ground()) velocity.y = jump_speed;
        self().set_velocity(velocity);
    }

    // Where the camera looks: -Z turned by pitch about X, then by the heading about Y.
    relay::Vec3 look_direction() const {
        const double radians = 3.14159265358979323846 / 180.0;
        const double heading = (self().rotation().y + yaw) * radians;
        const double up = pitch * radians;
        return {-std::sin(heading) * std::cos(up), std::sin(up), -std::cos(heading) * std::cos(up)};
    }

    void shoot() {
        if (!camera || ball_template.empty()) return;
        const auto direction = look_direction();
        const auto ball = relay::world::instantiate(ball_template,
                                                    camera.world_position() + direction * 0.5);
        if (!ball) return; // The log says why, for example a missing template.
        // The ball keeps the player's own motion, so shots fired while running fly true.
        ball.set_velocity(self().velocity() + direction * ball_speed);
    }

    bool on_ground() const {
        const auto hit = relay::world::raycast(self().world_position(), {0.0, -1.0, 0.0},
                                               ground_distance, 0xffffffffu, self());
        return hit.has_value();
    }

    double walk_speed = 4.0;         // Metres per second.
    double sprint_speed = 7.0;
    double jump_speed = 5.0;         // Upward speed when jumping.
    double mouse_sensitivity = 0.12; // Degrees per pixel of mouse movement.
    double stick_look_speed = 150.0; // Degrees per second at full stick.
    bool invert_y = false;
    double ground_distance = 1.0;    // From the body's centre down to just below its feet.
    std::string camera_name = "Camera";
    std::string ball_template = "Ball"; // Project template to shoot; empty turns shooting off.
    double ball_speed = 20.0;           // Metres per second along the view.

    relay::Entity camera;
    double yaw = 0.0;
    double pitch = 0.0;
};
RELAY_BEHAVIOUR(FirstPersonController)
