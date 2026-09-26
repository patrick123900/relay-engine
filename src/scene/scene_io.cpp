#include "relay/scene/scene_io.hpp"

#include "relay/core/json.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>
#include <variant>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace relay {
namespace {

constexpr std::uintmax_t max_scene_bytes = 16U * 1024U * 1024U;
constexpr std::size_t max_entities = 100'000U;
constexpr std::uint32_t max_entity_index = 1'000'000U;
constexpr std::size_t max_name_bytes = 4096U;

bool read_integer(const JsonValue* value, std::uint32_t& output) {
    if (value == nullptr || value->number() == nullptr) return false;
    const double number = *value->number();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(number) != number) return false;
    output = static_cast<std::uint32_t>(number);
    return true;
}

bool read_vec3(const JsonValue* value, Vec3& output) {
    if (value == nullptr || value->object() == nullptr) return false;
    const auto& object = *value->object();
    const auto* x = field(object, "x");
    const auto* y = field(object, "y");
    const auto* z = field(object, "z");
    if (x == nullptr || y == nullptr || z == nullptr || x->number() == nullptr ||
        y->number() == nullptr || z->number() == nullptr) return false;
    output = {*x->number(), *y->number(), *z->number()};
    return std::isfinite(output.x) && std::isfinite(output.y) && std::isfinite(output.z);
}

bool validate_component_metadata(const JsonValue* value, std::string& error) {
    if (value == nullptr || value->array() == nullptr) {
        error = "versioned scene requires a components array";
        return false;
    }
    for (const auto& component : *value->array()) {
        const auto* object = component.object();
        std::uint32_t stable_id = 0;
        if (object == nullptr || field(*object, "name") == nullptr ||
            field(*object, "name")->string() == nullptr ||
            !read_integer(field(*object, "stable_id"), stable_id) || stable_id == 0U) {
            error = "component metadata has an invalid name or stable_id";
            return false;
        }
        const auto* fields = field(*object, "fields");
        if (fields == nullptr || fields->array() == nullptr) {
            error = "component metadata fields must be an array";
            return false;
        }
        for (const auto& reflected_field : *fields->array()) {
            const auto* reflected_object = reflected_field.object();
            if (reflected_object == nullptr || field(*reflected_object, "name") == nullptr ||
                field(*reflected_object, "name")->string() == nullptr ||
                field(*reflected_object, "type") == nullptr ||
                field(*reflected_object, "type")->string() == nullptr) {
                error = "component field metadata is invalid";
                return false;
            }
        }
    }
    return true;
}

bool hierarchy_is_valid(const SceneState& state, std::string& error) {
    std::vector<unsigned char> marks(state.slots.size(), 0U);
    for (std::size_t start = 0; start < state.slots.size(); ++start) {
        if (!state.slots[start].alive || marks[start] == 2U) continue;
        std::vector<std::size_t> chain;
        auto current = start;
        while (true) {
            if (marks[current] == 1U) {
                error = "entity hierarchy contains a cycle";
                return false;
            }
            if (marks[current] == 2U) break;
            marks[current] = 1U;
            chain.push_back(current);
            const auto parent = state.slots[current].record.parent;
            if (!parent.valid()) break;
            if (parent.index >= state.slots.size() || !state.slots[parent.index].alive ||
                state.slots[parent.index].generation != parent.generation) {
                error = "entity references a missing or stale parent " + parent.to_string();
                return false;
            }
            current = parent.index;
        }
        for (const auto index : chain) marks[index] = 2U;
    }
    return true;
}

std::filesystem::path temporary_path_for(const std::filesystem::path& destination) {
    static std::atomic<std::uint64_t> serial{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return destination.parent_path() /
           (destination.filename().string() + ".tmp." + std::to_string(ticks) + '.' +
            std::to_string(serial.fetch_add(1U, std::memory_order_relaxed)));
}

bool replace_atomically(const std::filesystem::path& temporary,
                        const std::filesystem::path& destination, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "atomic scene replacement failed with Windows error " + std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = "atomic scene replacement failed";
        return false;
    }
#endif
    return true;
}

} // namespace

SceneFileLoadResult load_scene_file(const std::filesystem::path& path) {
    SceneFileLoadResult result;
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        result.error = "could not inspect scene file: " + filesystem_error.message();
        return result;
    }
    if (size > max_scene_bytes) {
        result.error = "scene file exceeds the 16 MiB limit";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) && !text.empty()) {
        result.error = "could not read the complete scene file";
        return result;
    }

    JsonParser parser(text);
    auto document = parser.parse();
    if (!document) {
        result.error = parser.error();
        return result;
    }
    const auto* root = document->object();
    if (root == nullptr) {
        result.error = "scene document root must be an object";
        return result;
    }
    const auto* format = field(*root, "format");
    if (format == nullptr || format->string() == nullptr || *format->string() != "relay.scene") {
        result.error = "scene format must be relay.scene";
        return result;
    }
    if (!read_integer(field(*root, "version"), result.source_version) || result.source_version > scene_file_version) {
        result.error = "unsupported scene file version";
        return result;
    }
    result.migrated = result.source_version < scene_file_version;
    if (result.source_version >= 1U &&
        !validate_component_metadata(field(*root, "components"), result.error)) {
        return result;
    }

    const JsonValue* entities_value = nullptr;
    const JsonValue::Object* scene_object = nullptr;
    if (result.source_version == 0U) entities_value = field(*root, "entities");
    else {
        const auto* scene = field(*root, "scene");
        if (scene != nullptr) scene_object = scene->object();
        if (scene_object != nullptr) entities_value = field(*scene_object, "entities");
    }
    if (entities_value == nullptr || entities_value->array() == nullptr) {
        result.error = "scene entities must be an array";
        return result;
    }
    if (entities_value->array()->size() > max_entities) {
        result.error = "scene exceeds the 100000 entity limit";
        return result;
    }

    SceneState state;
    for (const auto& entity_value : *entities_value->array()) {
        const auto* entity_object = entity_value.object();
        if (entity_object == nullptr) {
            result.error = "every entity must be an object";
            return result;
        }
        const auto* encoded_entity = field(*entity_object, "entity");
        const auto parsed_entity = encoded_entity != nullptr && encoded_entity->string() != nullptr
                                       ? Entity::parse(*encoded_entity->string()) : std::nullopt;
        if (!parsed_entity || parsed_entity->index > max_entity_index) {
            result.error = "entity has an invalid or excessive handle";
            return result;
        }
        if (state.slots.size() <= parsed_entity->index) state.slots.resize(parsed_entity->index + 1U);
        auto& slot = state.slots[parsed_entity->index];
        if (slot.alive) {
            result.error = "scene contains a duplicate entity handle index";
            return result;
        }
        const auto* name = field(*entity_object, "name");
        if (name == nullptr || name->string() == nullptr || name->string()->size() > max_name_bytes) {
            result.error = "entity name is missing or exceeds 4096 bytes";
            return result;
        }
        Entity parent{};
        const auto* parent_value = field(*entity_object, "parent");
        if (parent_value == nullptr) {
            result.error = "entity parent is missing";
            return result;
        }
        if (!parent_value->is_null()) {
            if (parent_value->string() == nullptr) {
                result.error = "entity parent must be null or a handle string";
                return result;
            }
            const auto parsed_parent = Entity::parse(*parent_value->string());
            if (!parsed_parent) {
                result.error = "entity parent handle is invalid";
                return result;
            }
            parent = *parsed_parent;
        }
        const auto* transform_value = field(*entity_object, "transform");
        if (transform_value == nullptr || transform_value->object() == nullptr) {
            result.error = "entity transform must be an object";
            return result;
        }
        const auto& transform_object = *transform_value->object();
        Transform transform;
        const auto rotation_name = result.source_version == 0U ? "rotation" : "rotation_degrees";
        if (!read_vec3(field(transform_object, "position"), transform.position) ||
            !read_vec3(field(transform_object, rotation_name), transform.rotation_degrees) ||
            !read_vec3(field(transform_object, "scale"), transform.scale)) {
            result.error = "entity transform requires finite position, rotation and scale vectors";
            return result;
        }
        std::optional<Camera> camera;
        if (result.source_version >= 2U) {
            const auto* camera_value = field(*entity_object, "camera");
            if (camera_value == nullptr) {
                result.error = "version 2 entity camera field is missing";
                return result;
            }
            if (!camera_value->is_null()) {
                const auto* camera_object = camera_value->object();
                if (camera_object == nullptr) {
                    result.error = "entity camera must be null or an object";
                    return result;
                }
                const auto* fov = field(*camera_object, "field_of_view_y_degrees");
                const auto* near_plane = field(*camera_object, "near_plane");
                const auto* far_plane = field(*camera_object, "far_plane");
                const auto* active = field(*camera_object, "active");
                if (fov == nullptr || fov->number() == nullptr || near_plane == nullptr ||
                    near_plane->number() == nullptr || far_plane == nullptr ||
                    far_plane->number() == nullptr || active == nullptr || active->boolean() == nullptr) {
                    result.error = "camera requires numeric projection fields and an active flag";
                    return result;
                }
                camera = Camera{*fov->number(), *near_plane->number(), *far_plane->number(),
                                *active->boolean()};
                if (!std::isfinite(camera->field_of_view_y_degrees) ||
                    camera->field_of_view_y_degrees <= 1.0 ||
                    camera->field_of_view_y_degrees >= 179.0 || camera->near_plane <= 0.0 ||
                    camera->far_plane <= camera->near_plane) {
                    result.error = "camera projection values are outside their valid ranges";
                    return result;
                }
                if (result.source_version >= 4U) {
                    const auto *height = field(*camera_object, "orthographic_height");
                    if (!height || !height->number() || *height->number() < 0.0) {
                        result.error = "camera requires a nonnegative orthographic_height";
                        return result;
                    }
                    camera->orthographic_height = *height->number();
                }
                if (result.source_version >= 5U) {
                    const auto* exposure = field(*camera_object, "exposure_ev");
                    if (!exposure || !exposure->number() ||
                        !std::isfinite(*exposure->number()) ||
                        *exposure->number() < -16.0 || *exposure->number() > 16.0) {
                        result.error = "camera requires exposure_ev between -16 and 16";
                        return result;
                    }
                    camera->exposure_ev = *exposure->number();
                }
            }
        }
        std::optional<MeshRenderer> mesh_renderer;
        if (result.source_version >= 3U) {
            const auto* renderer_value = field(*entity_object, "mesh_renderer");
            if (renderer_value == nullptr) {
                result.error = "version 3 entity mesh_renderer field is missing";
                return result;
            }
            if (!renderer_value->is_null()) {
                const auto* renderer_object = renderer_value->object();
                const auto* mesh = renderer_object == nullptr ? nullptr : field(*renderer_object, "mesh");
                const auto* material = renderer_object == nullptr ? nullptr : field(*renderer_object, "material");
                if (mesh == nullptr || mesh->string() == nullptr || material == nullptr ||
                    material->string() == nullptr || mesh->string()->empty() || material->string()->empty() ||
                    mesh->string()->size() > 128U || material->string()->size() > 128U) {
                    result.error = "mesh renderer requires mesh and material asset identifiers";
                    return result;
                }
                mesh_renderer = MeshRenderer{*mesh->string(), *material->string()};
                if (result.source_version >= 4U) {
                    const auto *weights = field(*renderer_object, "morph_weights");
                    if (!weights || !weights->array() || weights->array()->size() > 64U) {
                        result.error = "renderer requires at most 64 morph weights";
                        return result;
                    }
                    for (const auto &weight : *weights->array()) {
                        if (!weight.number() || std::abs(*weight.number()) > 100.0) {
                            result.error = "invalid morph weight";
                            return result;
                        }
                        mesh_renderer->morph_weights.push_back(*weight.number());
                    }
                }
            }
        }
        slot.generation = parsed_entity->generation;
        slot.alive = true;
        slot.record = EntityRecord{*name->string(), transform, parent, camera, mesh_renderer};
        if (result.source_version >= 4U) {
            const auto *animator = field(*entity_object, "animator");
            const auto *binding = field(*entity_object, "model_node");
            const auto *light = field(*entity_object, "light");
            if (!animator || !binding || !light) {
                result.error = "version 4 entity requires animator, model_node and light fields";
                return result;
            }
            if (!animator->is_null()) {
                const auto *o = animator->object();
                std::uint32_t clip = 0;
                if (!o || !field(*o, "model") || !field(*o, "model")->string() ||
                    !read_integer(field(*o, "clip"), clip) || !field(*o, "time_seconds") ||
                    !field(*o, "time_seconds")->number() || !field(*o, "speed") ||
                    !field(*o, "speed")->number() || !field(*o, "playing") ||
                    !field(*o, "playing")->boolean() || !field(*o, "loop") ||
                    !field(*o, "loop")->boolean()) {
                    result.error = "invalid animator component";
                    return result;
                }
                slot.record.animator =
                    Animator{*field(*o, "model")->string(),        clip,
                             *field(*o, "time_seconds")->number(), *field(*o, "speed")->number(),
                             *field(*o, "playing")->boolean(),     *field(*o, "loop")->boolean()};
            }
            if (!binding->is_null()) {
                const auto *o = binding->object();
                std::uint32_t node = 0;
                if (!o || !field(*o, "root") || !field(*o, "root")->string() ||
                    !read_integer(field(*o, "node"), node)) {
                    result.error = "invalid model node component";
                    return result;
                }
                const auto root_entity = Entity::parse(*field(*o, "root")->string());
                if (!root_entity) {
                    result.error = "invalid model root handle";
                    return result;
                }
                slot.record.model_node = ModelNode{*root_entity, node};
            }
            if (!light->is_null()) {
                const auto *o = light->object();
                Light l;
                std::uint32_t type = 0;
                if (!o || !read_integer(field(*o, "type"), type) || type > 2U ||
                    !read_vec3(field(*o, "color"), l.color) ||
                    !read_vec3(field(*o, "attenuation"), l.attenuation) ||
                    !field(*o, "intensity") || !field(*o, "intensity")->number() ||
                    !field(*o, "inner_cone") || !field(*o, "inner_cone")->number() ||
                    !field(*o, "outer_cone") || !field(*o, "outer_cone")->number()) {
                    result.error = "invalid light component";
                    return result;
                }
                l.type = static_cast<Light::Type>(type);
                l.intensity = *field(*o, "intensity")->number();
                l.inner_cone = *field(*o, "inner_cone")->number();
                l.outer_cone = *field(*o, "outer_cone")->number();
                if (!field(*o, "range") || !field(*o, "range")->number()) {
                    result.error = "light requires range";
                    return result;
                }
                l.range = *field(*o, "range")->number();
                slot.record.light = l;
            }
            Scene validator;
            const auto handle = validator.create();
            if (!validator.set_animator(handle, slot.record.animator) ||
                !validator.set_light(handle, slot.record.light)) {
                result.error = "component values outside valid ranges";
                return result;
            }
        }
        if (result.source_version >= 6U) {
            const auto* animation_value = field(*entity_object, "transform_animation");
            if (!animation_value) {
                result.error = "version 6 entity requires transform_animation";
                return result;
            }
            if (!animation_value->is_null()) {
                const auto* o = animation_value->object();
                const auto* time = o ? field(*o, "time_seconds") : nullptr;
                const auto* duration = o ? field(*o, "duration_seconds") : nullptr;
                const auto* speed = o ? field(*o, "speed") : nullptr;
                const auto* playing = o ? field(*o, "playing") : nullptr;
                const auto* loop = o ? field(*o, "loop") : nullptr;
                const auto* keys = o ? field(*o, "keys") : nullptr;
                if (!time || !time->number() || !duration || !duration->number() ||
                    !speed || !speed->number() || !playing || !playing->boolean() ||
                    !loop || !loop->boolean() || !keys || !keys->array() ||
                    keys->array()->size() > 1024U) {
                    result.error = "invalid transform animation fields";
                    return result;
                }
                TransformAnimation animation;
                animation.time_seconds = *time->number();
                animation.duration_seconds = *duration->number();
                animation.speed = *speed->number();
                animation.playing = *playing->boolean();
                animation.loop = *loop->boolean();
                for (const auto& key_value : *keys->array()) {
                    const auto* key = key_value.object();
                    const auto* key_time = key ? field(*key, "time_seconds") : nullptr;
                    TransformKeyframe parsed;
                    if (!key_time || !key_time->number() ||
                        !read_vec3(field(*key, "position"), parsed.value.position) ||
                        !read_vec3(field(*key, "rotation_degrees"), parsed.value.rotation_degrees) ||
                        !read_vec3(field(*key, "scale"), parsed.value.scale)) {
                        result.error = "invalid transform keyframe";
                        return result;
                    }
                    parsed.time_seconds = *key_time->number();
                    animation.keys.push_back(parsed);
                }
                Scene validator;
                const auto handle = validator.create();
                if (!validator.set_transform_animation(handle, animation)) {
                    result.error = "transform animation values outside valid ranges";
                    return result;
                }
                slot.record.transform_animation = std::move(animation);
            }
        }
        if (result.source_version >= 7U) {
            const auto* collider_value = field(*entity_object, "collider");
            if (!collider_value) {
                result.error = "version 7 entity requires collider";
                return result;
            }
            if (!collider_value->is_null()) {
                const auto* collider_object = collider_value->object();
                BoxCollider collider;
                const auto* enabled = collider_object ? field(*collider_object, "enabled") : nullptr;
                if (!collider_object ||
                    !read_vec3(field(*collider_object, "center"), collider.center) ||
                    !read_vec3(field(*collider_object, "half_extents"), collider.half_extents) ||
                    !enabled || !enabled->boolean() ||
                    !read_integer(field(*collider_object, "layer"), collider.layer) ||
                    !read_integer(field(*collider_object, "mask"), collider.mask)) {
                    result.error = "invalid box collider";
                    return result;
                }
                if (result.source_version >= 10U) {
                    std::uint32_t type{};
                    const auto* radius = field(*collider_object, "radius");
                    const auto* half_height = field(*collider_object, "half_height");
                    if (!read_integer(field(*collider_object, "type"), type) ||
                        type > (result.source_version >= 11U ? 4U : 2U) ||
                        !radius || !radius->number() || !half_height || !half_height->number()) {
                        result.error = "invalid collider shape";
                        return result;
                    }
                    collider.type = static_cast<BoxCollider::Type>(type);
                    collider.radius = *radius->number();
                    collider.half_height = *half_height->number();
                }
                if (result.source_version >= 11U) {
                    const auto* mesh = field(*collider_object, "mesh");
                    if (!mesh || !mesh->string()) {
                        result.error = "invalid collider mesh";
                        return result;
                    }
                    collider.mesh = *mesh->string();
                }
                collider.enabled = *enabled->boolean();
                Scene validator;
                const auto handle = validator.create();
                if (!validator.set_collider(handle, collider)) {
                    result.error = "box collider values outside valid ranges";
                    return result;
                }
                slot.record.collider = collider;
            }
        }
        if (result.source_version >= 8U) {
            const auto* body_value = field(*entity_object, "physics_body");
            if (!body_value) {
                result.error = "version 8 entity requires physics_body";
                return result;
            }
            if (!body_value->is_null()) {
                const auto* body_object = body_value->object();
                std::uint32_t type{};
                PhysicsBody body;
                const auto* mass = body_object ? field(*body_object, "mass") : nullptr;
                const auto* gravity = body_object ? field(*body_object, "gravity_scale") : nullptr;
                const auto* restitution = body_object ? field(*body_object, "restitution") : nullptr;
                if (!body_object || !read_integer(field(*body_object, "type"), type) ||
                    !mass || !mass->number() || !gravity || !gravity->number() ||
                    !restitution || !restitution->number() || type > 1U) {
                    result.error = "invalid physics body";
                    return result;
                }
                body.type = static_cast<PhysicsBody::Type>(type);
                body.mass = *mass->number();
                body.gravity_scale = *gravity->number();
                body.restitution = *restitution->number();
                if (result.source_version >= 9U) {
                    const auto* friction = field(*body_object, "friction");
                    const auto* linear = field(*body_object, "linear_damping");
                    const auto* angular = field(*body_object, "angular_damping");
                    if (!friction || !friction->number() || !linear || !linear->number() ||
                        !angular || !angular->number()) {
                        result.error = "invalid physics body material or damping";
                        return result;
                    }
                    body.friction = *friction->number();
                    body.linear_damping = *linear->number();
                    body.angular_damping = *angular->number();
                }
                if (result.source_version >= 14U) {
                    const auto* lock = field(*body_object, "lock_rotation");
                    if (!lock || !lock->boolean()) {
                        result.error = "version 14 physics body requires lock_rotation";
                        return result;
                    }
                    body.lock_rotation = *lock->boolean();
                }
                Scene validator;
                const auto handle = validator.create();
                if (!validator.set_physics_body(handle, body)) {
                    result.error = "physics body values outside valid ranges";
                    return result;
                }
                slot.record.physics_body = body;
            }
        }
        if (result.source_version == 12U) {
            // Version 12 allowed one optional script; it becomes the first script component.
            const auto* script_value = field(*entity_object, "script");
            if (!script_value) {
                result.error = "version 12 entity requires script";
                return result;
            }
            if (!script_value->is_null()) {
                const auto* script_object = script_value->object();
                const auto* behaviour = script_object ? field(*script_object, "behaviour") : nullptr;
                const auto* enabled = script_object ? field(*script_object, "enabled") : nullptr;
                if (!behaviour || !behaviour->string() || !enabled || !enabled->boolean() ||
                    !valid_behaviour_name(*behaviour->string())) {
                    result.error = "invalid script component";
                    return result;
                }
                slot.record.scripts.push_back(Script{*behaviour->string(), *enabled->boolean(), {}});
            }
        }
        if (result.source_version >= 13U) {
            const auto* scripts_value = field(*entity_object, "scripts");
            if (!scripts_value || !scripts_value->array() ||
                scripts_value->array()->size() > maximum_scripts_per_entity) {
                result.error = "version 13 entity requires a bounded scripts array";
                return result;
            }
            for (const auto& script_value : *scripts_value->array()) {
                const auto* script_object = script_value.object();
                const auto* behaviour = script_object ? field(*script_object, "behaviour") : nullptr;
                const auto* enabled = script_object ? field(*script_object, "enabled") : nullptr;
                const auto* properties = script_object ? field(*script_object, "properties") : nullptr;
                if (!behaviour || !behaviour->string() || !enabled || !enabled->boolean() ||
                    !properties || !properties->array()) {
                    result.error = "invalid script component";
                    return result;
                }
                Script script{*behaviour->string(), *enabled->boolean(), {}};
                for (const auto& property_value : *properties->array()) {
                    const auto* property_object = property_value.object();
                    const auto* property_name = property_object ? field(*property_object, "name") : nullptr;
                    const auto* type_name = property_object ? field(*property_object, "type") : nullptr;
                    const auto* value = property_object ? field(*property_object, "value") : nullptr;
                    const auto type = type_name && type_name->string()
                        ? script_property_type_from_name(*type_name->string()) : std::nullopt;
                    if (!property_name || !property_name->string() || !type || !value) {
                        result.error = "invalid script property";
                        return result;
                    }
                    ScriptProperty property;
                    property.name = *property_name->string();
                    property.type = *type;
                    bool valid = false;
                    switch (*type) {
                    case ScriptProperty::Type::boolean:
                        valid = value->boolean() != nullptr;
                        if (valid) property.boolean = *value->boolean();
                        break;
                    case ScriptProperty::Type::number:
                        valid = value->number() != nullptr;
                        if (valid) property.number = *value->number();
                        break;
                    case ScriptProperty::Type::vector:
                        valid = read_vec3(value, property.vector);
                        break;
                    case ScriptProperty::Type::text:
                        valid = value->string() != nullptr;
                        if (valid) property.text = *value->string();
                        break;
                    }
                    if (!valid) {
                        result.error = "script property value does not match its type";
                        return result;
                    }
                    script.properties.push_back(std::move(property));
                }
                if (!valid_script(script)) {
                    result.error = "script component values outside valid ranges";
                    return result;
                }
                slot.record.scripts.push_back(std::move(script));
            }
        }
        if (result.source_version == 15U) {
            // Version 15 had a native first person controller. It becomes a FirstPersonController
            // script component with the same settings, which projects provide as a script.
            const auto* value = field(*entity_object, "first_person_controller");
            if (!value) {
                result.error = "version 15 entity requires first_person_controller";
                return result;
            }
            if (!value->is_null()) {
                const auto* object = value->object();
                Script script{"FirstPersonController", true, {}};
                for (const auto* key : {"walk_speed", "sprint_speed", "jump_speed", "mouse_sensitivity",
                                        "stick_look_speed", "ground_distance"}) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->number()) {
                        result.error = "invalid first person controller";
                        return result;
                    }
                    ScriptProperty property;
                    property.name = key;
                    property.number = *item->number();
                    script.properties.push_back(std::move(property));
                }
                const auto* invert = object ? field(*object, "invert_y") : nullptr;
                const auto* camera_name = object ? field(*object, "camera") : nullptr;
                if (!invert || !invert->boolean() || !camera_name || !camera_name->string()) {
                    result.error = "invalid first person controller";
                    return result;
                }
                ScriptProperty invert_property;
                invert_property.name = "invert_y";
                invert_property.type = ScriptProperty::Type::boolean;
                invert_property.boolean = *invert->boolean();
                script.properties.push_back(std::move(invert_property));
                ScriptProperty camera_property;
                camera_property.name = "camera_name";
                camera_property.type = ScriptProperty::Type::text;
                camera_property.text = *camera_name->string();
                script.properties.push_back(std::move(camera_property));
                if (slot.record.scripts.size() >= maximum_scripts_per_entity || !valid_script(script)) {
                    result.error = "first person controller values outside valid ranges";
                    return result;
                }
                slot.record.scripts.push_back(std::move(script));
            }
        }
        if (result.source_version >= 17U) {
            const auto* value = field(*entity_object, "joint");
            if (!value) {
                result.error = "version 17 entity requires joint";
                return result;
            }
            if (!value->is_null()) {
                const auto* object = value->object();
                Joint joint;
                const auto number = [&](const char* key, double& target) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->number()) return false;
                    target = *item->number();
                    return true;
                };
                const auto flag = [&](const char* key, bool& target) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->boolean()) return false;
                    target = *item->boolean();
                    return true;
                };
                const auto* type = object ? field(*object, "type") : nullptr;
                const auto* connected = object ? field(*object, "connected") : nullptr;
                const auto parsed_type = type && type->string()
                    ? joint_type_from_name(*type->string()) : std::nullopt;
                std::optional<Entity> partner;
                if (connected && connected->string()) partner = Entity::parse(*connected->string());
                if (!parsed_type || !connected || (!connected->is_null() && !partner) ||
                    !read_vec3(field(*object, "anchor"), joint.anchor) ||
                    !read_vec3(field(*object, "axis"), joint.axis) ||
                    !read_vec3(field(*object, "connected_anchor"), joint.connected_anchor) ||
                    !flag("limits", joint.limits) || !number("limit_min", joint.limit_min) ||
                    !number("limit_max", joint.limit_max) || !flag("motor", joint.motor) ||
                    !number("motor_speed", joint.motor_speed) ||
                    !number("motor_force", joint.motor_force) ||
                    !number("spring_frequency", joint.spring_frequency) ||
                    !number("spring_damping", joint.spring_damping) ||
                    !flag("collide_connected", joint.collide_connected) ||
                    !flag("enabled", joint.enabled)) {
                    result.error = "invalid joint";
                    return result;
                }
                joint.type = *parsed_type;
                // Values are checked here; the connected node once every entity is loaded.
                Scene validator;
                const auto handle = validator.create();
                if (!validator.set_joint(handle, joint)) {
                    result.error = "joint values outside valid ranges";
                    return result;
                }
                if (partner) joint.connected = *partner;
                slot.record.joint = joint;
            }
        }
        if (result.source_version >= 18U) {
            const auto* source_value = field(*entity_object, "audio_source");
            const auto* listener_value = field(*entity_object, "audio_listener");
            if (!source_value || !listener_value ||
                (!listener_value->is_null() && !listener_value->object())) {
                result.error = "version 18 entity requires audio_source and audio_listener";
                return result;
            }
            if (!source_value->is_null()) {
                const auto* object = source_value->object();
                AudioSource source;
                const auto number = [&](const char* key, double& target) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->number()) return false;
                    target = *item->number();
                    return true;
                };
                const auto flag = [&](const char* key, bool& target) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->boolean()) return false;
                    target = *item->boolean();
                    return true;
                };
                const auto string_value = [&](const char* key, std::string& target) {
                    const auto* item = object ? field(*object, key) : nullptr;
                    if (!item || !item->string()) return false;
                    target = *item->string();
                    return true;
                };
                std::string rolloff;
                if (!string_value("clip", source.clip) || !string_value("bus", source.bus) ||
                    !number("volume_db", source.volume_db) || !number("pitch", source.pitch) ||
                    !number("pan", source.pan) || !flag("loop", source.loop) ||
                    !flag("play_on_start", source.play_on_start) ||
                    !flag("spatial", source.spatial) ||
                    !number("min_distance", source.min_distance) ||
                    !number("max_distance", source.max_distance) || !string_value("rolloff", rolloff) ||
                    !number("doppler", source.doppler)) {
                    result.error = "invalid audio source";
                    return result;
                }
                const auto parsed_rolloff = audio_rolloff_from_name(rolloff);
                if (!parsed_rolloff) {
                    result.error = "invalid audio source rolloff";
                    return result;
                }
                source.rolloff = *parsed_rolloff;
                if (result.source_version >= 19U &&
                    (!flag("occlusion", source.occlusion) ||
                     !number("reverb_send", source.reverb_send))) {
                    result.error = "invalid audio source occlusion or reverb send";
                    return result;
                }
                if (!valid_audio_source(source)) {
                    result.error = "audio source values outside valid ranges";
                    return result;
                }
                slot.record.audio_source = std::move(source);
            }
            if (!listener_value->is_null()) slot.record.audio_listener = AudioListener{};
        }
        if (result.source_version >= 19U) {
            const auto* value = field(*entity_object, "reverb_zone");
            if (!value || (!value->is_null() && !value->object())) {
                result.error = "version 19 entity requires reverb_zone";
                return result;
            }
            if (!value->is_null()) {
                const auto& object = *value->object();
                ReverbZone zone;
                const auto number = [&](const char* key, double& target) {
                    const auto* item = field(object, key);
                    if (!item || !item->number()) return false;
                    target = *item->number();
                    return true;
                };
                const auto* shape = field(object, "shape");
                const auto* preset = field(object, "preset");
                const auto parsed_shape = shape && shape->string()
                                              ? reverb_shape_from_name(*shape->string())
                                              : std::nullopt;
                if (!parsed_shape || !preset || !preset->string() ||
                    !number("radius", zone.radius) ||
                    !read_vec3(field(object, "half_extents"), zone.half_extents) ||
                    !number("fade", zone.fade) || !number("room_size", zone.room_size) ||
                    !number("damping", zone.damping) || !number("wet_db", zone.wet_db) ||
                    !number("pre_delay_ms", zone.pre_delay_ms)) {
                    result.error = "invalid reverb zone";
                    return result;
                }
                zone.shape = *parsed_shape;
                zone.preset = *preset->string();
                if (!valid_reverb_zone(zone)) {
                    result.error = "reverb zone values outside valid ranges";
                    return result;
                }
                slot.record.reverb_zone = std::move(zone);
            }
        }
        if (result.source_version >= 20U) {
            const auto* value = field(*entity_object, "music_player");
            if (!value || (!value->is_null() && !value->object())) {
                result.error = "version 20 entity requires music_player";
                return result;
            }
            if (!value->is_null()) {
                const auto& object = *value->object();
                MusicPlayer player;
                const auto number = [&](const char* key, double& target) {
                    const auto* item = field(object, key);
                    if (!item || !item->number()) return false;
                    target = *item->number();
                    return true;
                };
                const auto flag = [&](const char* key, bool& target) {
                    const auto* item = field(object, key);
                    if (!item || !item->boolean()) return false;
                    target = *item->boolean();
                    return true;
                };
                const auto* tracks = field(object, "tracks");
                const auto* bus = field(object, "bus");
                const auto* sync = field(object, "sync");
                const auto parsed_sync = sync && sync->string() ? music_sync_from_name(*sync->string())
                                                                : std::nullopt;
                double beats = 0.0;
                bool ok = tracks && tracks->array() && bus && bus->string() && parsed_sync &&
                          number("volume_db", player.volume_db) &&
                          number("crossfade_seconds", player.crossfade_seconds) &&
                          flag("shuffle", player.shuffle) && flag("loop_playlist", player.loop_playlist) &&
                          flag("play_on_start", player.play_on_start) && number("bpm", player.bpm) &&
                          number("beats_per_bar", beats) &&
                          number("first_beat_seconds", player.first_beat_seconds) && beats >= 1.0 &&
                          beats <= 16.0 && std::floor(beats) == beats;
                if (ok)
                    for (const auto& track : *tracks->array()) {
                        if (!track.string()) ok = false;
                        else player.tracks.push_back(*track.string());
                    }
                if (!ok) {
                    result.error = "invalid music player";
                    return result;
                }
                player.bus = *bus->string();
                player.sync = *parsed_sync;
                player.beats_per_bar = static_cast<std::uint32_t>(beats);
                if (!valid_music_player(player)) {
                    result.error = "music player values outside valid ranges";
                    return result;
                }
                slot.record.music_player = std::move(player);
            }
        }
        ++result.entity_count;
    }

    if (result.source_version >= 1U) {
        const auto* allocator_value = scene_object == nullptr ? nullptr : field(*scene_object, "allocator");
        const auto* allocator = allocator_value == nullptr ? nullptr : allocator_value->object();
        const auto* generations_value = allocator == nullptr ? nullptr : field(*allocator, "slot_generations");
        const auto* free_value = allocator == nullptr ? nullptr : field(*allocator, "free_indices");
        if (generations_value == nullptr || generations_value->array() == nullptr ||
            free_value == nullptr || free_value->array() == nullptr ||
            generations_value->array()->size() > static_cast<std::size_t>(max_entity_index) + 1U ||
            generations_value->array()->size() < state.slots.size()) {
            result.error = "scene allocator metadata is missing or invalid";
            return result;
        }
        state.slots.resize(generations_value->array()->size());
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            std::uint32_t generation = 0;
            if (!read_integer(&(*generations_value->array())[index], generation) || generation == 0U ||
                (state.slots[index].alive && state.slots[index].generation != generation)) {
                result.error = "scene allocator contains an invalid slot generation";
                return result;
            }
            state.slots[index].generation = generation;
        }
        std::vector<bool> free_seen(state.slots.size(), false);
        for (const auto& free_index_value : *free_value->array()) {
            std::uint32_t free_index = 0;
            if (!read_integer(&free_index_value, free_index) || free_index >= state.slots.size() ||
                state.slots[free_index].alive || free_seen[free_index]) {
                result.error = "scene allocator contains an invalid free index";
                return result;
            }
            free_seen[free_index] = true;
            state.free_indices.push_back(free_index);
        }
        for (std::size_t index = 0; index < state.slots.size(); ++index) {
            if (!state.slots[index].alive && !free_seen[index]) {
                result.error = "scene allocator omits a free slot";
                return result;
            }
        }
    } else {
        for (std::uint32_t index = 0; index < state.slots.size(); ++index) {
            if (!state.slots[index].alive) state.free_indices.push_back(index);
        }
    }
    if (!hierarchy_is_valid(state, result.error)) return result;
    for (std::size_t index = 0; index < state.slots.size(); ++index) {
        const auto& slot = state.slots[index];
        if (!slot.alive || !slot.record.joint || !slot.record.joint->connected.valid()) continue;
        const auto partner = slot.record.joint->connected;
        if (partner.index >= state.slots.size() || partner.index == index ||
            !state.slots[partner.index].alive ||
            state.slots[partner.index].generation != partner.generation) {
            result.error = "joint connects to a missing, stale or identical node";
            return result;
        }
    }
    for (const auto &slot : state.slots)
        if (slot.alive && slot.record.model_node) {
            const auto model_root = slot.record.model_node->root;
            if (model_root.index >= state.slots.size() || !state.slots[model_root.index].alive ||
                state.slots[model_root.index].generation != model_root.generation ||
                !state.slots[model_root.index].record.animator) {
                result.error = "model node references a missing, stale or non-model root";
                return result;
            }
        }
    std::optional<Entity> active_camera;
    for (std::uint32_t index = 0; index < state.slots.size(); ++index) {
        const auto& slot = state.slots[index];
        if (!slot.alive || !slot.record.camera.has_value() || !slot.record.camera->active) continue;
        if (active_camera.has_value()) {
            result.error = "scene contains more than one active camera";
            return result;
        }
        active_camera = Entity{index, slot.generation};
    }
    result.state = std::move(state);
    return result;
}

bool save_scene_file_atomic(const Scene& scene, const std::filesystem::path& path, std::string& error) {
    error.clear();
    if (path.empty() || path.filename().empty()) {
        error = "scene path must name a file";
        return false;
    }
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            error = "could not create scene directory: " + filesystem_error.message();
            return false;
        }
    }
    const auto temporary = temporary_path_for(path);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto document = scene.serialize_json();
        output.write(document.data(), static_cast<std::streamsize>(document.size()));
        output.put('\n');
        output.flush();
        if (!output) {
            error = "could not write the complete temporary scene file";
            output.close();
            std::filesystem::remove(temporary, filesystem_error);
            return false;
        }
    }
    if (!replace_atomically(temporary, path, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

} // namespace relay
