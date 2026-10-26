#pragma once

#include "relay_script.hpp"

#include <algorithm>
#include <cmath>
#include <string>

// First-person movement for the First Person Controller starter template.
//
// Put it on a plain node standing on the floor (its position is the player's feet) with a child
// camera node (the camera property) at eye height. The script makes the node a kinematic
// character: it walks up steps, refuses steep slopes, follows the ground down small drops and
// crouches under low ceilings, and gravity and jumping are done here. The node never turns:
// looking turns the camera, and walking follows where the camera faces.
//
// Input map names (Edit > Game Configuration > Input): move_x and move_y walk, look_x and look_y
// look with a gamepad, jump, sprint and crouch, and fire shoots. A jump is still allowed for a
// moment after walking off a ledge (coyote time), and pressed slightly before landing (buffered). The mouse looks around while the game has
// input; turn on "Lock the mouse cursor" there so the cursor stays inside the window.
//
// While a menu frees the cursor (relay::input::set_mouse_locked(false) from another script), the
// player stands still and neither looks nor shoots.
//
// Shooting spawns the ball template (a Template property) just in front of the camera and launches it
// where the camera looks. The character has no collider of its own, so balls pass through the
// player who shot them.
class FirstPersonController : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("walk_speed", walk_speed);
        p.add("sprint_speed", sprint_speed);
        p.add("jump_speed", jump_speed);
        p.add("mouse_sensitivity", mouse_sensitivity);
        p.add("stick_look_speed", stick_look_speed);
        p.add("invert_y", invert_y);
        p.add("gravity", gravity);
        p.add("coyote_time", coyote_time);
        p.add("jump_buffer", jump_buffer);
        p.add("height", height);
        p.add("crouch_height", crouch_height);
        p.add("crouch_speed", crouch_speed);
        p.add("eye_height", eye_height);
        p.add("radius", radius);
        p.add("step_height", step_height);
        p.add("max_slope_degrees", max_slope_degrees);
        p.add("camera", camera, relay::Only::component(relay::Component::camera));
        p.add("ball_template", ball_template);
        p.add("ball_speed", ball_speed);
        p.add("shot_sound", shot_sound);
    }

    void on_start() override {
        settings.radius = radius;
        settings.height = height;
        settings.step_height = step_height;
        settings.max_slope_degrees = max_slope_degrees;
        if (!self().make_character(settings))
            relay::world::warn("the character settings were refused, so the player cannot move");
        if (!camera) {
            relay::world::warn("no camera is assigned, so looking around is off");
            return;
        }
        camera.make_active_camera();
        started_locked = relay::input::mouse_locked();
        const auto rotation = camera.rotation();
        pitch = rotation.x;
        yaw = rotation.y;
    }

    void on_update(double dt) override {
        if (started_locked && !relay::input::mouse_locked()) {
            // A menu has the cursor: stop walking, keeping any fall.
            velocity.x = velocity.z = 0.0;
            fall(dt);
            return;
        }
        look(dt);
        crouch(dt);
        move(dt);
        if (relay::input::pressed("fire")) shoot();
    }

    // The camera this player looks through, for other scripts that aim from the player's view.
    relay::Entity view() const { return camera; }

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

    // Crouching is the character at a lower height; standing up again waits for headroom.
    void crouch(double dt) {
        crouching = relay::input::held("crouch");
        const double wanted = crouching ? crouch_height : height;
        if (wanted != settings.height) {
            auto next = settings;
            next.height = wanted;
            if (self().make_character(next)) settings = next;
            else crouching = true; // No room to stand: stay down.
        }
        if (camera) {
            // The eye follows the head, easing so crouching is not a jolt.
            const double target = eye_height - (height - settings.height);
            const double step = 6.0 * dt * height;
            eye = eye == 0.0 ? target : eye + std::clamp(target - eye, -step, step);
            auto at = camera.position();
            at.y = eye;
            camera.set_position(at);
        }
    }

    void move(double dt) {
        // Cameras look down -Z; yaw turns about +Y, so these are the camera's flat forward and right.
        const double heading = (self().rotation().y + yaw) * 3.14159265358979323846 / 180.0;
        const relay::Vec3 forward{-std::sin(heading), 0.0, -std::cos(heading)};
        const relay::Vec3 right{std::cos(heading), 0.0, -std::sin(heading)};
        const auto input = relay::input::vector("move_x", "move_y");
        double speed = relay::input::held("sprint") ? sprint_speed : walk_speed;
        if (settings.height < height) speed = std::min(speed, crouch_speed);
        const auto walk = (right * input.x + forward * input.y) * speed;
        velocity.x = walk.x;
        velocity.z = walk.z;

        // Jumping: a press is remembered for a moment, and the ground is still "under" the player for
        // a moment after walking off it.
        if (relay::input::pressed("jump")) since_jump_pressed = 0.0;
        if (since_jump_pressed <= jump_buffer && since_grounded <= coyote_time) {
            velocity.y = jump_speed;
            since_jump_pressed = jump_buffer + 1.0;
            since_grounded = coyote_time + 1.0;
        }
        fall(dt);
    }

    // Gravity, then the character moves; the step it actually made decides what is ground.
    void fall(double dt) {
        since_jump_pressed += dt;
        since_grounded += dt;
        if (grounded && velocity.y <= 0.0) velocity.y = 0.0;
        else velocity.y -= gravity * dt;
        const auto state = self().move_character(velocity, dt);
        if (!state) return;
        grounded = state->grounded;
        if (grounded) since_grounded = 0.0;
        if (!grounded && state->velocity.y < velocity.y - 0.01 && velocity.y > 0.0)
            velocity.y = state->velocity.y; // Bumped a ceiling: stop rising.
    }

    // Where the camera looks: -Z turned by pitch about X, then by the heading about Y.
    relay::Vec3 look_direction() const {
        const double radians = 3.14159265358979323846 / 180.0;
        const double heading = (self().rotation().y + yaw) * radians;
        const double up = pitch * radians;
        return {-std::sin(heading) * std::cos(up), std::sin(up), -std::cos(heading) * std::cos(up)};
    }

    void shoot() {
        if (!camera || !ball_template) return;
        const auto direction = look_direction();
        const auto ball = relay::world::instantiate(ball_template,
                                                    camera.world_position() + direction * 0.5);
        if (!ball) return; // The log says why, for example a missing template.
        // The ball keeps the player's own motion, so shots fired while running fly true.
        ball.set_velocity(velocity + direction * ball_speed);
        // A one-shot from where the ball leaves, a little lower each shot so repeats differ.
        if (shot_sound) {
            relay::audio::OneShot sound;
            sound.volume_db = -6.0;
            sound.pitch = 1.0 - 0.03 * static_cast<double>(shots++ % 4U);
            relay::audio::play(shot_sound, camera.world_position() + direction * 0.5, sound);
        }
    }

    double walk_speed = 4.0;         // Metres per second.
    double sprint_speed = 7.0;
    double jump_speed = 5.0;         // Upward speed when jumping.
    double mouse_sensitivity = 0.12; // Degrees per pixel of mouse movement.
    double stick_look_speed = 150.0; // Degrees per second at full stick.
    bool invert_y = false;
    double gravity = 20.0;           // Metres per second squared; above real life, for a snappier jump.
    double coyote_time = 0.12;       // Seconds after leaving the ground that a jump still works.
    double jump_buffer = 0.1;        // Seconds before landing that a jump press is remembered.
    double height = 1.8;             // Feet to head, standing.
    double crouch_height = 1.0;
    double crouch_speed = 2.0;       // The fastest it walks crouched.
    double eye_height = 1.6;         // The camera's height above the feet, standing.
    double radius = 0.35;
    double step_height = 0.4;
    double max_slope_degrees = 50.0;
    relay::Entity camera;               // The eye: turned to look, and the view the game renders through.
    relay::Template ball_template{"templates/Ball.relay-template.json"}; // Shot on fire; none turns shooting off.
    double ball_speed = 20.0;           // Metres per second along the view.
    relay::AudioClip shot_sound{"sounds/shot.wav"}; // Played on each shot; none is silent.
    unsigned shots = 0;

    relay::CharacterSettings settings;
    relay::Vec3 velocity;
    bool grounded = false;
    bool crouching = false;
    double since_grounded = 1000.0;
    double since_jump_pressed = 1000.0;
    double eye = 0.0;
    bool started_locked = false;
    double yaw = 0.0;
    double pitch = 0.0;
};
