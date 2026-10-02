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
        {"sky", "Sky", "Rendering", true, true, false,
         "The scene's sky: a color gradient or a sky material drawn behind everything, the "
         "ambient light it gives, and distance fog. The first node with a sky is used."},
        {"post_process", "Post process", "Rendering", true, true, false,
         "Full-screen effects from post_process shaders, such as color grading or outlines, "
         "applied in order to every view. The first node with post processing is used."},
        {"particle_emitter", "Particle emitter", "Effects", true, true, false,
         "Emits particles for sparks, smoke, fire, dust, rain and magic: a shape, a rate and "
         "bursts, forces, turbulence and collisions, and size, color and flipbook over each "
         "particle's life."},
        {"collider", "Collider", "Physics", true, true, false,
         "A box, sphere, capsule, convex hull or triangle mesh shape for collisions, overlaps "
         "and raycasts."},
        {"physics_body", "Physics body", "Physics", true, true, false,
         "Makes the node static or dynamic during Run Game: gravity, mass, bounce and friction."},
        {"joint", "Joint", "Physics", true, true, false,
         "Links this node's physics body to another body or to the world during Run Game: fixed, "
         "point (ball and socket), hinge, slider or distance (rope or spring)."},
        {"audio_source", "Audio source", "Audio", true, true, false,
         "Plays a sound file from the project during Run Game, positioned in the world or flat, "
         "through a mixer bus."},
        {"audio_listener", "Audio listener", "Audio", true, true, false,
         "Where the game hears from, usually on the player's camera. Without one, the active "
         "camera listens."},
        {"reverb_zone", "Reverb zone", "Audio", true, true, false,
         "A space with its own reverb, like a room, hall or cave, heard while the listener is "
         "inside it and fading out around it."},
        {"music_player", "Music player", "Audio", true, true, false,
         "Plays a playlist of music files, crossfading between tracks, with changes that can wait "
         "for the next beat or bar."},
        {"ui_canvas", "Canvas", "UI", true, true, false,
         "A layer for game interface controls, drawn over the game's view during Run Game only. "
         "It scales the controls below it to the screen."},
        {"ui_control", "Control", "UI", true, true, false,
         "Places a rectangle on the screen for interface widgets: anchors on the parent control "
         "or screen, offsets, pivot, rotation and scale."},
        {"ui_panel", "Panel", "UI", true, true, false,
         "A filled rectangle with rounded corners, a border and a shadow."},
        {"ui_label", "Label", "UI", true, true, false, "Text, aligned and optionally wrapped."},
        {"ui_image", "Image", "UI", true, true, false,
         "A PNG or JPEG image, stretched, fitted, tiled or nine-sliced."},
        {"ui_button", "Button", "UI", true, true, false,
         "Makes the control clickable; scripts hear clicks through on_ui."},
        {"ui_toggle", "Check box", "UI", true, true, false, "A check box or switch that flips on click."},
        {"ui_slider", "Slider", "UI", true, true, false, "A value chosen by dragging a handle."},
        {"ui_progress_bar", "Progress bar", "UI", true, true, false,
         "Fills part of the control from one edge, for health, loading or timers."},
        {"ui_container", "Container", "UI", true, true, false,
         "Arranges child controls in a column, a row or a grid."},
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
    if (id == "joint") return record.joint.has_value();
    if (id == "audio_source") return record.audio_source.has_value();
    if (id == "audio_listener") return record.audio_listener.has_value();
    if (id == "reverb_zone") return record.reverb_zone.has_value();
    if (id == "music_player") return record.music_player.has_value();
    if (id == "sky") return record.sky.has_value();
    if (id == "post_process") return record.post_process.has_value();
    if (id == "particle_emitter") return record.particle_emitter.has_value();
    if (id == "animator") return record.animator.has_value();
    if (id == "script") return !record.scripts.empty();
    if (const auto* ui = find_ui_component(id); ui && ui->id == id) return ui->present(record.ui);
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
    if (const auto* ui = find_ui_component(id); ui && ui->id == id) {
        // Widgets bring the Control that places them, like Unity's RectTransform.
        auto components = record->ui;
        if (id == "ui_canvas" && components.control) {
            error = "a Control cannot also be a Canvas; add the canvas to a parent node";
            return false;
        }
        if (id != "ui_canvas" && components.canvas) {
            error = "a Canvas cannot also be a Control; add controls as its children";
            return false;
        }
        if (id != "ui_canvas" && !components.control) components.control = UiControl{};
        ui->attach(components, true);
        std::string reason;
        if (!valid_ui(components, &reason)) {
            error = reason;
            return false;
        }
        added = scene.set_ui(entity, std::move(components));
    } else if (id == "mesh_renderer") {
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
    } else if (id == "joint") {
        added = scene.set_joint(entity, Joint{});
    } else if (id == "audio_source") {
        added = scene.set_audio_source(entity, AudioSource{});
    } else if (id == "audio_listener") {
        added = scene.set_audio_listener(entity, AudioListener{});
    } else if (id == "reverb_zone") {
        added = scene.set_reverb_zone(entity, ReverbZone{});
    } else if (id == "music_player") {
        added = scene.set_music_player(entity, MusicPlayer{});
    } else if (id == "sky") {
        added = scene.set_sky(entity, Sky{});
    } else if (id == "post_process") {
        added = scene.set_post_process(entity, PostProcess{});
    } else if (id == "particle_emitter") {
        added = scene.set_particle_emitter(entity, ParticleEmitter{});
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
    if (const auto* ui = find_ui_component(id); ui && ui->id == id) {
        auto components = record->ui;
        ui->attach(components, false);
        if (!valid_ui(components)) {
            error = "remove the node's UI widgets before its Control";
            return false;
        }
        return scene.set_ui(entity, std::move(components));
    }
    if (id == "mesh_renderer") return scene.set_mesh_renderer(entity, std::nullopt);
    if (id == "camera") return scene.set_camera(entity, std::nullopt);
    if (id == "light") return scene.set_light(entity, std::nullopt);
    if (id == "collider") return scene.set_collider(entity, std::nullopt);
    if (id == "physics_body") return scene.set_physics_body(entity, std::nullopt);
    if (id == "keyframes") return scene.set_transform_animation(entity, std::nullopt);
    if (id == "joint") return scene.set_joint(entity, std::nullopt);
    if (id == "audio_source") return scene.set_audio_source(entity, std::nullopt);
    if (id == "audio_listener") return scene.set_audio_listener(entity, std::nullopt);
    if (id == "reverb_zone") return scene.set_reverb_zone(entity, std::nullopt);
    if (id == "music_player") return scene.set_music_player(entity, std::nullopt);
    if (id == "sky") return scene.set_sky(entity, std::nullopt);
    if (id == "post_process") return scene.set_post_process(entity, std::nullopt);
    if (id == "particle_emitter") return scene.set_particle_emitter(entity, std::nullopt);
    auto scripts = record->scripts;
    scripts.erase(scripts.begin() + static_cast<std::ptrdiff_t>(index));
    return scene.set_scripts(entity, std::move(scripts));
}

std::optional<ComponentFlag> component_flag(const std::string_view id) {
    struct Entry {
        std::string_view id;
        ComponentFlag flag;
    };
    static constexpr Entry entries[]{
        {"camera", ComponentFlag::camera},
        {"mesh_renderer", ComponentFlag::mesh_renderer},
        {"light", ComponentFlag::light},
        {"sky", ComponentFlag::sky},
        {"post_process", ComponentFlag::post_process},
        {"particle_emitter", ComponentFlag::particle_emitter},
        {"physics_body", ComponentFlag::physics_body},
        {"audio_source", ComponentFlag::audio_source},
        {"audio_listener", ComponentFlag::audio_listener},
        {"reverb_zone", ComponentFlag::reverb_zone},
        {"music_player", ComponentFlag::music_player},
        {"keyframes", ComponentFlag::keyframes},
        {"ui_canvas", ComponentFlag::ui_canvas},
        {"ui_control", ComponentFlag::ui_control},
        {"ui_panel", ComponentFlag::ui_panel},
        {"ui_label", ComponentFlag::ui_label},
        {"ui_image", ComponentFlag::ui_image},
        {"ui_button", ComponentFlag::ui_button},
        {"ui_toggle", ComponentFlag::ui_toggle},
        {"ui_slider", ComponentFlag::ui_slider},
        {"ui_progress_bar", ComponentFlag::ui_progress_bar},
        {"ui_container", ComponentFlag::ui_container},
    };
    for (const auto& entry : entries)
        if (entry.id == id) return entry.flag;
    return std::nullopt;
}

bool can_disable_component(const std::string_view id) {
    return component_flag(id).has_value() || id == "collider" || id == "joint" || id == "script";
}

bool component_enabled(const EntityRecord& record, const std::string_view id, const std::size_t index) {
    if (id == "collider") return record.collider && record.collider->enabled;
    if (id == "joint") return record.joint && record.joint->enabled;
    if (id == "script") return index < record.scripts.size() && record.scripts[index].enabled;
    if (const auto flag = component_flag(id)) return has_component(record, id) && !component_disabled(record, *flag);
    return has_component(record, id);
}

bool set_component_enabled(Scene& scene, const Entity entity, const std::string_view id,
                           const std::size_t index, const bool enabled, std::string& error) {
    auto* record = scene.get(entity);
    if (!record) {
        error = "invalid or stale entity";
        return false;
    }
    if (!can_disable_component(id)) {
        error = id == "transform" ? "the Transform cannot be disabled"
                                  : "unknown or non-switchable component";
        return false;
    }
    if (!has_component(*record, id) || (id == "script" && index >= record->scripts.size())) {
        error = "the node has no such component";
        return false;
    }
    if (id == "collider") {
        auto collider = *record->collider;
        collider.enabled = enabled;
        return scene.set_collider(entity, std::move(collider));
    }
    if (id == "joint") {
        auto joint = *record->joint;
        joint.enabled = enabled;
        return scene.set_joint(entity, std::move(joint));
    }
    if (id == "script") {
        auto scripts = record->scripts;
        scripts[index].enabled = enabled;
        return scene.set_scripts(entity, std::move(scripts));
    }
    const auto bit = static_cast<std::uint32_t>(*component_flag(id));
    if (enabled) record->disabled_components &= ~bit;
    else record->disabled_components |= bit;
    return true;
}

} // namespace relay
