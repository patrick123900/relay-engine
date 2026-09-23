#include "relay/scene/components.hpp"

#include <algorithm>

namespace relay {

const std::vector<ComponentKind>& engine_components() {
    // Model animation drives imported node hierarchies that depend on it, so it stays attached.
    static const std::vector<ComponentKind> kinds{
        {"transform", "Transform", "Core", false, false, false,
         "Position, rotation and scale. Every node has exactly one."},
        {"mesh_renderer", "Mesh renderer", "Rendering", true, true, false,
         "Draws a mesh with a material."},
        {"camera", "Camera", "Rendering", true, true, false,
         "A perspective or orthographic viewpoint with exposure. The active camera is what Run "
         "Game shows."},
        {"light", "Light", "Rendering", true, true, false,
         "A directional, point or spot light, with shadows."},
        {"collider", "Collider", "Physics", true, true, false,
         "A box, sphere, capsule, convex hull or triangle mesh shape for collisions, overlaps "
         "and raycasts."},
        {"physics_body", "Physics body", "Physics", true, true, false,
         "Makes the node static or dynamic during Run Game: gravity, mass, bounce and friction."},
        {"first_person_controller", "First person controller", "Physics", true, true, false,
         "Walks, sprints, jumps and looks around with the input map during Run Game. Needs a "
         "dynamic physics body with rotation locked, a collider and a child camera node."},
        {"keyframes", "Transform keyframes", "Animation", true, true, false,
         "Animates position, rotation and scale between keys you set on a timeline."},
        {"animator", "Model animation", "Animation", false, false, false,
         "Plays an imported model's animation clips."},
        {"script", "Script", "Scripts", true, true, true,
         "Runs a C++ gameplay behaviour from the project's scripts folder during Run Game."},
    };
    return kinds;
}

const ComponentKind* find_component_kind(const std::string_view id) {
    const auto& kinds = engine_components();
    const auto found = std::find_if(kinds.begin(), kinds.end(),
                                    [id](const ComponentKind& kind) { return kind.id == id; });
    return found == kinds.end() ? nullptr : &*found;
}

bool has_component(const EntityRecord& record, const std::string_view id) {
    if (id == "transform") return true;
    if (id == "mesh_renderer") return record.mesh_renderer.has_value();
    if (id == "camera") return record.camera.has_value();
    if (id == "light") return record.light.has_value();
    if (id == "collider") return record.collider.has_value();
    if (id == "physics_body") return record.physics_body.has_value();
    if (id == "keyframes") return record.transform_animation.has_value();
    if (id == "first_person_controller") return record.first_person_controller.has_value();
    if (id == "animator") return record.animator.has_value();
    if (id == "script") return !record.scripts.empty();
    return false;
}

bool add_component(Scene& scene, const Entity entity, const std::string_view id,
                   const std::string_view behaviour, std::string& error) {
    const auto* record = scene.get(entity);
    const auto* kind = find_component_kind(id);
    if (!record) {
        error = "invalid or stale entity";
        return false;
    }
    if (!kind || !kind->addable) {
        error = "unknown or non-addable component";
        return false;
    }
    if (!kind->multiple && has_component(*record, id)) {
        error = "the node already has a " + std::string{kind->name} + " component";
        return false;
    }
    bool added = false;
    if (id == "mesh_renderer") {
        added = scene.set_mesh_renderer(entity, MeshRenderer{"builtin.quad", "builtin.azure", {}});
    } else if (id == "camera") {
        Camera camera;
        camera.active = !scene.active_camera().has_value();
        added = scene.set_camera(entity, camera);
    } else if (id == "light") {
        added = scene.set_light(entity, Light{});
    } else if (id == "collider") {
        added = scene.set_collider(entity, BoxCollider{});
    } else if (id == "physics_body") {
        added = scene.set_physics_body(entity, PhysicsBody{});
    } else if (id == "first_person_controller") {
        added = scene.set_first_person_controller(entity, FirstPersonController{});
    } else if (id == "keyframes") {
        TransformAnimation animation;
        animation.keys.push_back({0.0, record->transform});
        added = scene.set_transform_animation(entity, animation);
    } else if (id == "script") {
        if (!valid_behaviour_name(behaviour)) {
            error = "a script component needs a behaviour class name";
            return false;
        }
        auto scripts = record->scripts;
        scripts.push_back(Script{std::string{behaviour}, true, {}});
        if (scripts.size() > maximum_scripts_per_entity) {
            error = "a node carries at most 32 scripts";
            return false;
        }
        added = scene.set_scripts(entity, std::move(scripts));
    }
    if (!added) error = "could not add the component";
    return added;
}

bool remove_component(Scene& scene, const Entity entity, const std::string_view id,
                      const std::size_t index, std::string& error) {
    const auto* record = scene.get(entity);
    const auto* kind = find_component_kind(id);
    if (!record) {
        error = "invalid or stale entity";
        return false;
    }
    if (!kind || !kind->removable) {
        error = id == "transform" ? "the Transform cannot be removed"
                                  : "unknown or non-removable component";
        return false;
    }
    if (!has_component(*record, id) || (id == "script" && index >= record->scripts.size())) {
        error = "the node has no such component";
        return false;
    }
    if (id == "mesh_renderer") return scene.set_mesh_renderer(entity, std::nullopt);
    if (id == "camera") return scene.set_camera(entity, std::nullopt);
    if (id == "light") return scene.set_light(entity, std::nullopt);
    if (id == "collider") return scene.set_collider(entity, std::nullopt);
    if (id == "physics_body") return scene.set_physics_body(entity, std::nullopt);
    if (id == "keyframes") return scene.set_transform_animation(entity, std::nullopt);
    if (id == "first_person_controller")
        return scene.set_first_person_controller(entity, std::nullopt);
    auto scripts = record->scripts;
    scripts.erase(scripts.begin() + static_cast<std::ptrdiff_t>(index));
    return scene.set_scripts(entity, std::move(scripts));
}

} // namespace relay
