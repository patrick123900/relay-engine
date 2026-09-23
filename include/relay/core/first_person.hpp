#pragma once

#include "relay/scene/scene.hpp"

#include <map>

namespace relay {

class InputState;
class LogBuffer;
class PhysicsWorld;

// Runs every FirstPersonController component during Run Game. It turns the controller's camera
// child for look and sets the node's physics velocity for walking and jumping, once per game step
// before scripts, so scripts see and can adjust the result. Stop Game restores the scene, so
// nothing here needs undoing.
class FirstPersonControllers {
public:
    // Finds each controller's camera and makes the first one the camera the game renders through.
    void start(Scene& scene, LogBuffer& logs);
    void update(Scene& scene, PhysicsWorld& physics, const InputState& input, double delta_seconds);
    void stop() { states_.clear(); }

private:
    struct State {
        Entity camera{};
        double yaw{};   // Camera heading in degrees, relative to the controller node.
        double pitch{};
    };
    std::map<Entity, State> states_;
};

} // namespace relay
