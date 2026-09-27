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
    if (id == "Sky") return record.sky.has_value();
    if (id == "Light") return record.light.has_value();
    if (id == "DirectionalLight") return light(Light::Type::directional);
    if (id == "PointLight") return light(Light::Type::point);
    if (id == "SpotLight") return light(Light::Type::spot);
    if (id == "PhysicsBody") return record.collider.has_value() || record.physics_body.has_value();
    if (id == "RigidBody") return dynamic;
    if (id == "StaticBody") return !dynamic;
    if (id == "StaticMesh") return record.mesh_renderer.has_value();
    if (id == "AudioSource") return record.audio_source.has_value();
    if (id == "ReverbZone") return record.reverb_zone.has_value();
    if (id == "MusicPlayer") return record.music_player.has_value();
    if (id == "PostProcess") return record.post_process.has_value();
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
    } else if (id == "Sky") {
        record.sky = Sky{};
        // The sun: warm white, bright enough to lead the sky's ambient light.
        record.light = Light{};
        record.light->type = Light::Type::directional;
        record.light->color = default_sun_color;
        record.light->intensity = default_sun_intensity;
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
    } else if (id == "RigidBody") {
        record.physics_body = PhysicsBody{};
    } else if (id == "StaticBody") {
        record.physics_body = PhysicsBody{PhysicsBody::Type::static_body};
    } else if (id == "StaticMesh") {
        record.mesh_renderer = MeshRenderer{"builtin.quad", "builtin.azure", {}};
    } else if (id == "AudioSource") {
        record.audio_source = AudioSource{};
    } else if (id == "ReverbZone") {
        record.reverb_zone = ReverbZone{};
    } else if (id == "MusicPlayer") {
        record.music_player = MusicPlayer{};
    } else if (id == "PostProcess") {
        record.post_process = PostProcess{};
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
        {"RigidBody", "Rigid Body", "PhysicsBody",
         "Falls, collides and responds to impulses during Run Game. Add a Mesh renderer to see it.",
         {"physics_body"}, true},
        {"StaticBody", "Static Body", "PhysicsBody",
         "Collides but never moves, such as floors and walls. Add a Mesh renderer to see it.",
         {"physics_body"}, true},
        {"Camera", "Camera", "Node", "A viewpoint. The active camera is what Run Game shows.",
         {"camera"}, true},
        // Before Light, so a node with a sky and its sun is a Sky rather than a Directional Light.
        {"Sky", "Sky", "Node",
         "The sky, sun and fog: a color gradient or sky material behind everything, a directional "
         "light for the sun (turn the node to aim it), and distance fog.",
         {"sky", "light"}, true},
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
        {"AudioSource", "Audio Source", "Node",
         "Plays a sound during Run Game, from its position in the world or flat.",
         {"audio_source"}, true},
        {"ReverbZone", "Reverb Zone", "Node",
         "A room, hall or cave with its own reverb, heard while the listener is inside it.",
         {"reverb_zone"}, true},
        {"MusicPlayer", "Music Player", "Node",
         "Plays a playlist of music, crossfading between tracks on the beat.", {"music_player"}, true},
        {"PostProcess", "Post Process", "Node",
         "Full-screen effects on the lit scene, such as color grading, vignettes or outlines, from "
         "post_process shaders.",
         {"post_process"}, true},
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
        !scene.set_audio_source(entity, record.audio_source) ||
        !scene.set_reverb_zone(entity, record.reverb_zone) ||
        !scene.set_music_player(entity, record.music_player) ||
        !scene.set_sky(entity, record.sky) ||
        !scene.set_post_process(entity, record.post_process)) {
        error = "could not give the node its components";
        return false;
    }
    // A new sky's sun comes from above and to one side rather than along the horizon.
    if (record.sky) {
        auto transform = scene.get(entity)->transform;
        if (transform.rotation_degrees == Vec3{}) {
            transform.rotation_degrees = {-50.0, 30.0, 0.0};
            if (!scene.set_transform(entity, transform)) {
                error = "could not aim the sun";
                return false;
            }
        }
    }
    return true;
}

} // namespace relay
