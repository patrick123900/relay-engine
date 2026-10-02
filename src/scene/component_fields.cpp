#include "relay/scene/component_fields.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <type_traits>

namespace relay {
namespace {

// Where each component lives in EntityRecord and the scene setter that validates it.
template <class C> struct Slot;
template <> struct Slot<Camera> {
    static constexpr auto member = &EntityRecord::camera;
    static bool set(Scene& scene, Entity entity, Camera value) { return scene.set_camera(entity, std::move(value)); }
};
template <> struct Slot<MeshRenderer> {
    static constexpr auto member = &EntityRecord::mesh_renderer;
    static bool set(Scene& scene, Entity entity, MeshRenderer value) {
        return scene.set_mesh_renderer(entity, std::move(value));
    }
};
template <> struct Slot<Light> {
    static constexpr auto member = &EntityRecord::light;
    static bool set(Scene& scene, Entity entity, Light value) { return scene.set_light(entity, std::move(value)); }
};
template <> struct Slot<BoxCollider> {
    static constexpr auto member = &EntityRecord::collider;
    static bool set(Scene& scene, Entity entity, BoxCollider value) {
        return scene.set_collider(entity, std::move(value));
    }
};
template <> struct Slot<PhysicsBody> {
    static constexpr auto member = &EntityRecord::physics_body;
    static bool set(Scene& scene, Entity entity, PhysicsBody value) {
        return scene.set_physics_body(entity, std::move(value));
    }
};
template <> struct Slot<Joint> {
    static constexpr auto member = &EntityRecord::joint;
    static bool set(Scene& scene, Entity entity, Joint value) { return scene.set_joint(entity, std::move(value)); }
};
template <> struct Slot<AudioSource> {
    static constexpr auto member = &EntityRecord::audio_source;
    static bool set(Scene& scene, Entity entity, AudioSource value) {
        return scene.set_audio_source(entity, std::move(value));
    }
};
template <> struct Slot<TransformAnimation> {
    static constexpr auto member = &EntityRecord::transform_animation;
    static bool set(Scene& scene, Entity entity, TransformAnimation value) {
        return scene.set_transform_animation(entity, std::move(value));
    }
};

using Getter = std::function<std::optional<ComponentFieldValue>(const EntityRecord&)>;
using Setter = std::function<bool(Scene&, Entity, const EntityRecord&, const ComponentFieldValue&, std::string&)>;

struct Entry {
    ComponentFieldInfo info;
    Getter get;
    Setter set;
};

template <class T> constexpr ComponentFieldType type_of() {
    if constexpr (std::is_same_v<T, bool>) return ComponentFieldType::boolean;
    else if constexpr (std::is_same_v<T, double>) return ComponentFieldType::number;
    else if constexpr (std::is_same_v<T, std::uint32_t>) return ComponentFieldType::integer;
    else if constexpr (std::is_same_v<T, Vec3>) return ComponentFieldType::vec3;
    else if constexpr (std::is_same_v<T, std::string>) return ComponentFieldType::text;
    else {
        static_assert(std::is_enum_v<T>);
        return ComponentFieldType::choice;
    }
}

std::string join(const std::vector<std::string_view>& choices) {
    std::string text;
    for (const auto choice : choices) text += (text.empty() ? "" : ", ") + std::string{choice};
    return text;
}

// Assigns `value` to `target` when it is the field's kind; otherwise explains what it takes.
template <class T>
bool assign(T& target, const ComponentFieldValue& value, const std::vector<std::string_view>& choices,
            std::string& error) {
    if constexpr (std::is_same_v<T, bool>) {
        if (const auto* flag = std::get_if<bool>(&value)) target = *flag;
        else if (const auto* number = std::get_if<double>(&value)) target = *number != 0.0;
        else { error = "takes true or false"; return false; }
    } else if constexpr (std::is_same_v<T, double>) {
        const auto* number = std::get_if<double>(&value);
        if (!number || !std::isfinite(*number)) { error = "takes a finite number"; return false; }
        target = *number;
    } else if constexpr (std::is_same_v<T, std::uint32_t>) {
        const auto* number = std::get_if<double>(&value);
        if (!number || !std::isfinite(*number) || *number < 0.0 || *number > 4294967295.0 ||
            std::floor(*number) != *number) {
            error = "takes a whole number from 0 to 4294967295";
            return false;
        }
        target = static_cast<std::uint32_t>(*number);
    } else if constexpr (std::is_same_v<T, Vec3>) {
        const auto* vector = std::get_if<Vec3>(&value);
        if (!vector || !std::isfinite(vector->x) || !std::isfinite(vector->y) || !std::isfinite(vector->z)) {
            error = "takes three finite numbers";
            return false;
        }
        target = *vector;
    } else if constexpr (std::is_same_v<T, std::string>) {
        const auto* text = std::get_if<std::string>(&value);
        if (!text) { error = "takes text"; return false; }
        target = *text;
    } else {
        const auto* text = std::get_if<std::string>(&value);
        const auto found = text ? std::find(choices.begin(), choices.end(), *text) : choices.end();
        if (found == choices.end()) {
            error = "is one of " + join(choices);
            return false;
        }
        target = static_cast<T>(found - choices.begin());
    }
    return true;
}

template <class C, class T>
Entry field(std::string_view name, T C::*member, std::vector<std::string_view> choices = {}) {
    Entry entry{{name, type_of<T>(), choices}, {}, {}};
    entry.get = [member, choices](const EntityRecord& record) -> std::optional<ComponentFieldValue> {
        const auto& component = record.*Slot<C>::member;
        if (!component) return std::nullopt;
        const auto& value = (*component).*member;
        if constexpr (std::is_same_v<T, std::uint32_t>) return ComponentFieldValue{static_cast<double>(value)};
        else if constexpr (std::is_enum_v<T>) {
            const auto index = static_cast<std::size_t>(value);
            if (index >= choices.size()) return std::nullopt;
            return ComponentFieldValue{std::string{choices[index]}};
        } else return ComponentFieldValue{value};
    };
    entry.set = [member, choices, name](Scene& scene, Entity entity, const EntityRecord& record,
                                        const ComponentFieldValue& value, std::string& error) {
        const auto& current = record.*Slot<C>::member;
        if (!current) {
            error = "the entity has no " + std::string{component_field_component(name)} + " component";
            return false;
        }
        auto component = *current;
        if (!assign(component.*member, value, choices, error)) {
            error = std::string{name} + " " + error;
            return false;
        }
        // Morph weights belong to a mesh and per-object parameters to a material, as in the Inspector.
        if constexpr (std::is_same_v<C, MeshRenderer>) {
            if (component.mesh != current->mesh) component.morph_weights.clear();
            if (component.material != current->material) component.parameters.clear();
        }
        if (Slot<C>::set(scene, entity, std::move(component))) return true;
        error = "the value is out of range for " + std::string{name};
        return false;
    };
    return entry;
}

const std::vector<Entry>& entries() {
    static const std::vector<Entry> list{
        field("camera.active", &Camera::active),
        field("camera.field_of_view_y_degrees", &Camera::field_of_view_y_degrees),
        field("camera.near_plane", &Camera::near_plane),
        field("camera.far_plane", &Camera::far_plane),
        field("camera.orthographic_height", &Camera::orthographic_height),
        field("camera.exposure_ev", &Camera::exposure_ev),
        field("mesh_renderer.mesh", &MeshRenderer::mesh),
        field("mesh_renderer.material", &MeshRenderer::material),
        field("light.type", &Light::type, {"directional", "point", "spot"}),
        field("light.color", &Light::color),
        field("light.intensity", &Light::intensity),
        field("light.attenuation", &Light::attenuation),
        field("light.inner_cone", &Light::inner_cone),
        field("light.outer_cone", &Light::outer_cone),
        field("light.range", &Light::range),
        field("collider.type", &BoxCollider::type, {"box", "sphere", "capsule", "convex", "mesh"}),
        field("collider.enabled", &BoxCollider::enabled),
        field("collider.center", &BoxCollider::center),
        field("collider.half_extents", &BoxCollider::half_extents),
        field("collider.radius", &BoxCollider::radius),
        field("collider.half_height", &BoxCollider::half_height),
        field("collider.mesh", &BoxCollider::mesh),
        field("collider.layer", &BoxCollider::layer),
        field("collider.mask", &BoxCollider::mask),
        field("physics_body.type", &PhysicsBody::type, {"static", "dynamic"}),
        field("physics_body.mass", &PhysicsBody::mass),
        field("physics_body.gravity_scale", &PhysicsBody::gravity_scale),
        field("physics_body.restitution", &PhysicsBody::restitution),
        field("physics_body.friction", &PhysicsBody::friction),
        field("physics_body.linear_damping", &PhysicsBody::linear_damping),
        field("physics_body.angular_damping", &PhysicsBody::angular_damping),
        field("physics_body.lock_rotation", &PhysicsBody::lock_rotation),
        field("joint.type", &Joint::type, {"fixed", "point", "hinge", "slider", "distance"}),
        field("joint.enabled", &Joint::enabled),
        field("joint.anchor", &Joint::anchor),
        field("joint.axis", &Joint::axis),
        field("joint.connected_anchor", &Joint::connected_anchor),
        field("joint.limits", &Joint::limits),
        field("joint.limit_min", &Joint::limit_min),
        field("joint.limit_max", &Joint::limit_max),
        field("joint.motor", &Joint::motor),
        field("joint.motor_speed", &Joint::motor_speed),
        field("joint.motor_force", &Joint::motor_force),
        field("joint.spring_frequency", &Joint::spring_frequency),
        field("joint.spring_damping", &Joint::spring_damping),
        field("joint.collide_connected", &Joint::collide_connected),
        field("audio_source.clip", &AudioSource::clip),
        field("audio_source.bus", &AudioSource::bus),
        field("audio_source.volume_db", &AudioSource::volume_db),
        field("audio_source.pitch", &AudioSource::pitch),
        field("audio_source.pan", &AudioSource::pan),
        field("audio_source.loop", &AudioSource::loop),
        field("audio_source.play_on_start", &AudioSource::play_on_start),
        field("audio_source.spatial", &AudioSource::spatial),
        field("audio_source.min_distance", &AudioSource::min_distance),
        field("audio_source.max_distance", &AudioSource::max_distance),
        field("audio_source.rolloff", &AudioSource::rolloff, {"inverse", "linear", "inverse_square"}),
        field("audio_source.doppler", &AudioSource::doppler),
        field("audio_source.occlusion", &AudioSource::occlusion),
        field("audio_source.reverb_send", &AudioSource::reverb_send),
        field("keyframes.playing", &TransformAnimation::playing),
        field("keyframes.loop", &TransformAnimation::loop),
        field("keyframes.speed", &TransformAnimation::speed),
        field("keyframes.time_seconds", &TransformAnimation::time_seconds),
        field("keyframes.duration_seconds", &TransformAnimation::duration_seconds),
    };
    return list;
}

const Entry* find_entry(const std::string_view name) {
    const auto& list = entries();
    const auto found = std::find_if(list.begin(), list.end(), [name](const Entry& entry) { return entry.info.name == name; });
    return found == list.end() ? nullptr : &*found;
}

} // namespace

const std::vector<ComponentFieldInfo>& component_fields() {
    static const std::vector<ComponentFieldInfo> list = [] {
        std::vector<ComponentFieldInfo> result;
        for (const auto& entry : entries()) result.push_back(entry.info);
        return result;
    }();
    return list;
}

const ComponentFieldInfo* find_component_field(const std::string_view name) {
    const auto* entry = find_entry(name);
    return entry ? &entry->info : nullptr;
}

std::string_view component_field_component(const std::string_view name) {
    return name.substr(0, name.find('.'));
}

std::optional<ComponentFieldValue> component_field_value(const EntityRecord& record, const std::string_view name) {
    const auto* entry = find_entry(name);
    return entry ? entry->get(record) : std::nullopt;
}

bool set_component_field(Scene& scene, const Entity entity, const std::string_view name,
                         const ComponentFieldValue& value, std::string& error) {
    const auto* record = scene.get(entity);
    const auto* entry = find_entry(name);
    if (!record) {
        error = "the entity is gone";
        return false;
    }
    if (!entry) {
        error = "no component field is called " + std::string{name};
        return false;
    }
    return entry->set(scene, entity, *record, value, error);
}

} // namespace relay
