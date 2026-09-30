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
    if (id == "ParticleEmitter") return record.particle_emitter.has_value();
    const auto& ui = record.ui;
    const auto container = [&](UiContainer::Layout layout) {
        return ui.container && ui.container->layout == layout;
    };
    if (id == "Canvas") return ui.canvas.has_value();
    if (id == "Control") return ui.control.has_value();
    if (id == "Button") return ui.button.has_value();
    if (id == "CheckBox") return ui.toggle.has_value();
    if (id == "Slider") return ui.slider.has_value();
    if (id == "ProgressBar") return ui.progress_bar.has_value();
    if (id == "Container") return ui.container.has_value();
    if (id == "VBoxContainer") return container(UiContainer::Layout::vertical);
    if (id == "HBoxContainer") return container(UiContainer::Layout::horizontal);
    if (id == "GridContainer") return container(UiContainer::Layout::grid);
    if (id == "Panel") return ui.panel.has_value();
    if (id == "Label") return ui.label.has_value();
    if (id == "Image") return ui.image.has_value();
    return false;
}

// Interface types: a size centered in the parent, and a look that reads on any game.
void contribute_ui(const std::string_view id, UiComponents& ui) {
    const auto centered = [&](double width, double height) {
        (void)apply_ui_anchor_preset(*ui.control, "center", Vec2{width, height});
    };
    const auto text = [&](std::string value, UiLabel::Align horizontal) {
        ui.label = UiLabel{};
        ui.label->text = std::move(value);
        ui.label->horizontal_align = horizontal;
    };
    if (id == "Canvas") {
        ui.canvas = UiCanvas{};
    } else if (id == "Control") {
        // A plain control usually groups others, so it fills its parent.
        ui.control = UiControl{};
        (void)apply_ui_anchor_preset(*ui.control, "full_rect");
    } else if (id == "Button") {
        centered(220.0, 56.0);
        ui.control->mouse_filter = UiControl::MouseFilter::stop;
        ui.panel = UiPanel{};
        ui.panel->color = {0.13, 0.14, 0.17, 0.95};
        ui.panel->corner_radius = 10.0;
        ui.button = UiButton{};
        text("Button", UiLabel::Align::center);
        ui.label->size = 22.0;
    } else if (id == "CheckBox") {
        centered(240.0, 40.0);
        ui.control->mouse_filter = UiControl::MouseFilter::stop;
        ui.toggle = UiToggle{};
        text("Check box", UiLabel::Align::start);
        ui.label->size = 22.0;
    } else if (id == "Slider") {
        centered(260.0, 32.0);
        ui.control->mouse_filter = UiControl::MouseFilter::stop;
        ui.slider = UiSlider{};
    } else if (id == "ProgressBar") {
        centered(280.0, 24.0);
        ui.panel = UiPanel{};
        ui.panel->color = {0.06, 0.07, 0.09, 0.8};
        ui.panel->corner_radius = 12.0;
        ui.progress_bar = UiProgressBar{};
    } else if (id == "VBoxContainer" || id == "HBoxContainer" || id == "GridContainer") {
        centered(320.0, 240.0);
        ui.container = UiContainer{};
        ui.container->layout = id == "VBoxContainer"   ? UiContainer::Layout::vertical
                               : id == "HBoxContainer" ? UiContainer::Layout::horizontal
                                                       : UiContainer::Layout::grid;
    } else if (id == "Panel") {
        centered(360.0, 240.0);
        ui.control->mouse_filter = UiControl::MouseFilter::stop;
        ui.panel = UiPanel{};
    } else if (id == "Label") {
        centered(240.0, 48.0);
        ui.control->mouse_filter = UiControl::MouseFilter::ignore;
        text("Label", UiLabel::Align::start);
    } else if (id == "Image") {
        centered(128.0, 128.0);
        ui.control->mouse_filter = UiControl::MouseFilter::ignore;
        ui.image = UiImage{};
    }
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
    } else if (id == "ParticleEmitter") {
        record.particle_emitter = ParticleEmitter{};
    } else {
        contribute_ui(id, record.ui);
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
        {"ParticleEmitter", "Particle Emitter", "Node",
         "Emits particles for sparks, smoke, fire, dust, rain and magic effects. Plays in the editor "
         "too, so you see the effect as you tune it.",
         {"particle_emitter"}, true},
        {"Canvas", "Canvas", "Node",
         "A layer for the game's interface, drawn over the game's view during Run Game only. It "
         "scales the controls below it to fit the screen.",
         {"ui_canvas"}, true},
        {"Control", "Control", "Node",
         "A rectangle of the game's interface, placed by anchors on its parent control or the "
         "screen. Groups other controls.",
         {"ui_control"}, true},
        // Interactive controls first: a button's panel and label must not make it a Panel.
        {"Button", "Button", "Control", "A clickable button with a label. Scripts hear clicks through on_ui.",
         {"ui_button", "ui_panel", "ui_label"}, true},
        {"CheckBox", "Check Box", "Control", "A check box or switch that flips on click, with a label.",
         {"ui_toggle", "ui_label"}, true},
        {"Slider", "Slider", "Control", "A value chosen by dragging a handle along a track.",
         {"ui_slider"}, true},
        {"ProgressBar", "Progress Bar", "Control", "A bar filled to a value, for health, loading or timers.",
         {"ui_progress_bar", "ui_panel"}, true},
        {"Container", "Container", "Control", "Arranges its child controls. Choose a column, row or grid.",
         {"ui_container"}, false},
        {"VBoxContainer", "Vertical Box", "Container", "Stacks its child controls in a column.", {}, true},
        {"HBoxContainer", "Horizontal Box", "Container", "Lines its child controls up in a row.", {}, true},
        {"GridContainer", "Grid", "Container", "Arranges its child controls in rows of equal columns.", {},
         true},
        {"Panel", "Panel", "Control", "A filled, rounded rectangle, such as a window or a HUD backdrop.",
         {"ui_panel"}, true},
        {"Label", "Label", "Control", "Text on the screen.", {"ui_label"}, true},
        {"Image", "Image", "Control", "A picture from a PNG or JPEG file.", {"ui_image"}, true},
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
        !scene.set_post_process(entity, record.post_process) ||
        !scene.set_particle_emitter(entity, record.particle_emitter) || !scene.set_ui(entity, record.ui)) {
        error = "could not give the node its components";
        return false;
    }
    // A new control goes in front of its siblings.
    if (record.ui.control) {
        const auto parent = scene.get(entity)->parent;
        std::int32_t order = 0;
        bool sibling = false;
        for (const auto other : scene.entities()) {
            const auto* candidate = scene.get(other);
            if (other == entity || candidate->parent != parent || !candidate->ui.control) continue;
            order = sibling ? std::max(order, candidate->ui.control->order) : candidate->ui.control->order;
            sibling = true;
        }
        if (sibling && order < 1000000) {
            auto ui = scene.get(entity)->ui;
            ui.control->order = order + 1;
            if (!scene.set_ui(entity, std::move(ui))) {
                error = "could not order the control";
                return false;
            }
        }
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
