#include "relay/core/first_person.hpp"

#include "relay/core/input.hpp"
#include "relay/core/log.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/physics/collision.hpp"

#include <algorithm>
#include <cmath>

namespace relay {
namespace {

constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;

std::optional<Vec3> world_position(const Scene& scene, Entity entity) {
    auto world = editor_identity();
    for (std::size_t depth = 0; entity.valid() && depth < 4096U; ++depth) {
        const auto* record = scene.get(entity);
        if (!record) return std::nullopt;
        const auto& local = record->transform;
        world = editor_multiply(editor_compose(local.position, local.rotation_degrees, local.scale),
                                world);
        entity = record->parent;
    }
    return Vec3{world[12], world[13], world[14]};
}

Entity camera_child(const Scene& scene, const Entity owner, const std::string& name) {
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (record->parent == owner && record->name == name && record->camera) return entity;
    }
    return {};
}

} // namespace

void FirstPersonControllers::start(Scene& scene, LogBuffer& logs) {
    states_.clear();
    bool camera_taken = false;
    for (const auto entity : scene.entities()) {
        const auto* record = scene.get(entity);
        if (!record->first_person_controller) continue;
        State state;
        state.camera = camera_child(scene, entity, record->first_person_controller->camera);
        if (!state.camera.valid()) {
            logs.write(LogLevel::warning,
                       "First person controller " + record->name + " has no child camera named " +
                           record->first_person_controller->camera + ", so looking around is off");
        } else {
            const auto rotation = scene.get(state.camera)->transform.rotation_degrees;
            state.pitch = rotation.x;
            state.yaw = rotation.y;
            if (!camera_taken) {
                auto camera = *scene.get(state.camera)->camera;
                camera.active = true;
                camera_taken = scene.set_camera(state.camera, camera);
            }
        }
        states_.emplace(entity, state);
    }
}

void FirstPersonControllers::update(Scene& scene, PhysicsWorld& physics, const InputState& input,
                                    const double delta_seconds) {
    for (auto& [entity, state] : states_) {
        const auto* record = scene.get(entity);
        if (!record || !record->first_person_controller) continue;
        const auto settings = *record->first_person_controller;

        // Look: the camera turns, the body stays upright.
        if (scene.contains(state.camera)) {
            const double vertical = settings.invert_y ? -input.mouse_dy() : input.mouse_dy();
            state.yaw -= input.mouse_dx() * settings.mouse_sensitivity +
                         input.axis("look_x") * settings.stick_look_speed * delta_seconds;
            state.pitch -= vertical * settings.mouse_sensitivity;
            state.pitch += input.axis("look_y") * settings.stick_look_speed * delta_seconds *
                           (settings.invert_y ? -1.0 : 1.0);
            state.pitch = std::clamp(state.pitch, -89.0, 89.0);
            state.yaw = std::remainder(state.yaw, 360.0);
            auto transform = scene.get(state.camera)->transform;
            transform.rotation_degrees = {state.pitch, state.yaw, 0.0};
            (void)scene.set_transform(state.camera, transform);
        }

        // Walk: cameras look down -Z and yaw turns about +Y, giving the flat forward and right.
        const auto current = physics.velocity(scene, entity);
        if (!current) continue; // Walking needs a dynamic physics body.
        const double heading =
            (record->transform.rotation_degrees.y + state.yaw) * degrees_to_radians;
        const Vec3 forward{-std::sin(heading), 0.0, -std::cos(heading)};
        const Vec3 right{std::cos(heading), 0.0, -std::sin(heading)};
        double move_x = input.axis("move_x"), move_y = input.axis("move_y");
        if (const double length = std::hypot(move_x, move_y); length > 1.0) {
            move_x /= length;
            move_y /= length;
        }
        const double speed = input.action("sprint", InputState::Query::held) ? settings.sprint_speed
                                                                             : settings.walk_speed;
        Vec3 velocity = *current;
        velocity.x = (right.x * move_x + forward.x * move_y) * speed;
        velocity.z = (right.z * move_x + forward.z * move_y) * speed;
        if (input.action("jump", InputState::Query::pressed)) {
            const auto origin = world_position(scene, entity);
            const bool grounded = origin && physics.raycast(scene, *origin, {0.0, -1.0, 0.0},
                                                            settings.ground_distance, 0xffffffffU,
                                                            entity).hit;
            if (grounded) velocity.y = settings.jump_speed;
        }
        (void)physics.set_velocity(scene, entity, velocity);
    }
}

} // namespace relay
