#include "relay/scene/node_types.hpp"

#include <algorithm>

namespace relay {
namespace {

// Whether a node has what this type adds on top of its parent. Only called once the parent matched.
bool matches(const std::string_view id, const EntityRecord& record) {
    const auto light = [&](Light::Type type) {
        return record.light && record.light->type == type;
    };
    const bool dynamic = record.physics_body &&
                         record.physics_body->type == PhysicsBody::Type::dynamic;
    if (id == "Node") return true;
    if (id == "Model") return record.animator.has_value();
    if (id == "Camera") return record.camera.has_value();
    if (id == "Light") return record.light.has_value();
    if (id == "DirectionalLight") return light(Light::Type::directional);
    if (id == "PointLight") return light(Light::Type::point);
    if (id == "SpotLight") return light(Light::Type::spot);
    if (id == "PhysicsBody") return record.collider.has_value() || record.physics_body.has_value();
    if (id == "FirstPersonController") return record.first_person_controller.has_value();
    if (id == "RigidBody") return dynamic;
    if (id == "StaticBody") return !dynamic;
    if (id == "StaticMesh") return record.mesh_renderer.has_value();
    return false;
}

// Adds the components one type contributes, with the defaults a new node of that type gets.
void contribute(const std::string_view id, EntityRecord& record, const bool camera_free) {
    const auto light = [&](Light::Type type) {
        if (!record.light) record.light = Light{};
        record.light->type = type;
    };
    if (id == "Camera") {
        record.camera = Camera{};
        record.camera->active = camera_free;
    } else if (id == "Light") {
        record.light = Light{};
    } else if (id == "DirectionalLight") {
        light(Light::Type::directional);
    } else if (id == "PointLight") {
        light(Light::Type::point);
    } else if (id == "SpotLight") {
        light(Light::Type::spot);
    } else if (id == "PhysicsBody") {
        record.collider = BoxCollider{};
    } else if (id == "FirstPersonController") {
        // A 1.8 m capsule that stays upright; walking sets velocity, so friction would only
        // catch on walls.
        record.collider = BoxCollider{};
        record.collider->type = BoxCollider::Type::capsule;
        record.collider->radius = 0.35;
        record.collider->half_height = 0.55;
        record.physics_body = PhysicsBody{};
        record.physics_body->mass = 70.0;
        record.physics_body->friction = 0.0;
        record.physics_body->linear_damping = 0.0;
        record.physics_body->angular_damping = 0.0;
        record.physics_body->lock_rotation = true;
        record.first_person_controller = FirstPersonController{};
    } else if (id == "RigidBody") {
        record.physics_body = PhysicsBody{};
    } else if (id == "StaticBody") {
        record.physics_body = PhysicsBody{PhysicsBody::Type::static_body};
    } else if (id == "StaticMesh") {
        record.mesh_renderer = MeshRenderer{"builtin.quad", "builtin.azure", {}};
    }
}

} // namespace

const std::vector<NodeTypeInfo>& node_types() {
    static const std::vector<NodeTypeInfo> types{
        {"Node", "Node", "", "A position, rotation and scale in the scene. Every node is one.", {},
         true},
        {"Model", "Model", "Node",
         "The root of an imported model, driving its animation. Created by importing a model.",
         {"animator"}, false},
        {"PhysicsBody", "Physics Body", "Node",
         "Takes part in collisions. Choose a body that moves or one that stays still.",
         {"collider"}, false},
        {"FirstPersonController", "First Person Controller", "PhysicsBody",
         "A player to walk around with: an upright capsule body, a camera at eye height, and "
         "mouse, keyboard and gamepad look, walk, sprint and jump from the input map.",
         {"physics_body", "first_person_controller"}, true},
        {"RigidBody", "Rigid Body", "PhysicsBody",
         "Falls, collides and responds to impulses during Run Game. Add a Mesh renderer to see it.",
         {"physics_body"}, true},
        {"StaticBody", "Static Body", "PhysicsBody",
         "Collides but never moves, such as floors and walls. Add a Mesh renderer to see it.",
         {"physics_body"}, true},
        {"Camera", "Camera", "Node", "A viewpoint. The active camera is what Run Game shows.",
         {"camera"}, true},
        {"Light", "Light", "Node", "Lights the scene. Choose a directional, point or spot light.",
         {"light"}, false},
        {"DirectionalLight", "Directional Light", "Light",
         "Parallel light from far away, like the sun. Casts cascaded shadows.", {}, true},
        {"PointLight", "Point Light", "Light", "Light shining in every direction from one point.",
         {}, true},
        {"SpotLight", "Spot Light", "Light", "A cone of light, like a torch or stage light.", {},
         true},
        {"StaticMesh", "Static Mesh", "Node", "Draws a mesh with a material.", {"mesh_renderer"},
         true},
    };
    return types;
}

const NodeTypeInfo* find_node_type(const std::string_view id) {
    const auto& types = node_types();
    const auto found = std::find_if(types.begin(), types.end(),
                                    [id](const NodeTypeInfo& type) { return type.id == id; });
    return found == types.end() ? nullptr : &*found;
}

std::vector<std::string_view> node_type_components(const std::string_view id) {
    std::vector<const NodeTypeInfo*> chain;
    for (const auto* type = find_node_type(id); type; type = find_node_type(type->parent))
        chain.push_back(type);
    std::vector<std::string_view> components{"transform"};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (const auto component : (*it)->adds)
            if (std::find(components.begin(), components.end(), component) == components.end())
                components.push_back(component);
    return components;
}

std::string_view node_type(const EntityRecord& record) {
    std::string_view current = "Node";
    for (bool descended = true; descended;) {
        descended = false;
        for (const auto& type : node_types())
            if (type.parent == current && matches(type.id, record)) {
                current = type.id;
                descended = true;
                break;
            }
    }
    return current;
}

bool apply_node_type(Scene& scene, const Entity entity, const std::string_view id,
                     std::string& error) {
    const auto* type = find_node_type(id);
    if (!type || !type->creatable) {
        error = "unknown or non-creatable node type";
        return false;
    }
    std::vector<std::string_view> chain;
    for (const auto* current = type; current; current = find_node_type(current->parent))
        chain.push_back(current->id);
    EntityRecord record;
    const bool camera_free = !scene.active_camera().has_value();
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) contribute(*it, record, camera_free);
    if (!scene.set_camera(entity, record.camera) || !scene.set_light(entity, record.light) ||
        !scene.set_collider(entity, record.collider) ||
        !scene.set_physics_body(entity, record.physics_body) ||
        !scene.set_mesh_renderer(entity, record.mesh_renderer) ||
        !scene.set_first_person_controller(entity, record.first_person_controller)) {
        error = "could not give the node its components";
        return false;
    }
    if (id != "FirstPersonController") return true;
    // It stands on the ground plane, looking through a camera at eye height (1.6 m).
    auto transform = scene.get(entity)->transform;
    transform.position.y = 1.0;
    const auto eye = scene.create("Camera", entity);
    Transform eye_transform;
    eye_transform.position = {0.0, 0.7, 0.0};
    Camera camera;
    camera.field_of_view_y_degrees = 75.0;
    camera.near_plane = 0.05;
    camera.active = false; // The controller makes it active when the game starts.
    if (!scene.set_transform(entity, transform) || !eye.valid() ||
        !scene.set_transform(eye, eye_transform) || !scene.set_camera(eye, camera)) {
        error = "could not give the controller its camera";
        return false;
    }
    return true;
}

} // namespace relay
