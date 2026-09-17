#include "relay/scene/scene.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace relay {
namespace {

std::string escape_json(const std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (byte < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += hex[byte >> 4U];
                escaped += hex[byte & 0x0FU];
            } else escaped += character;
            break;
        }
    }
    return escaped;
}

void append_vec3(std::ostringstream& output, const Vec3& value) {
    output << "{\"x\":" << value.x << ",\"y\":" << value.y << ",\"z\":" << value.z << '}';
}

void append_entity(std::ostringstream& output, const Entity entity, const EntityRecord& record) {
    output << "{\"entity\":\"" << entity.to_string() << "\",\"name\":\""
           << escape_json(record.name) << "\",\"parent\":";
    if (record.parent.valid()) output << '"' << record.parent.to_string() << '"';
    else output << "null";
    output << ",\"transform\":{\"position\":";
    append_vec3(output, record.transform.position);
    output << ",\"rotation_degrees\":";
    append_vec3(output, record.transform.rotation_degrees);
    output << ",\"scale\":";
    append_vec3(output, record.transform.scale);
    output << "},\"camera\":";
    if (record.camera.has_value()) {
        output << "{\"field_of_view_y_degrees\":" << record.camera->field_of_view_y_degrees
               << ",\"near_plane\":" << record.camera->near_plane
               << ",\"far_plane\":" << record.camera->far_plane
               << ",\"active\":" << (record.camera->active ? "true" : "false")
               << ",\"orthographic_height\":" << record.camera->orthographic_height << '}';
    } else {
        output << "null";
    }
    output << ",\"mesh_renderer\":";
    if (record.mesh_renderer.has_value()) {
        output << "{\"mesh\":\"" << escape_json(record.mesh_renderer->mesh) << "\",\"material\":\""
               << escape_json(record.mesh_renderer->material) << "\",\"morph_weights\":[";
        for (std::size_t i = 0; i < record.mesh_renderer->morph_weights.size(); ++i) {
            if (i)
                output << ',';
            output << record.mesh_renderer->morph_weights[i];
        }
        output << "]}";
    } else {
        output << "null";
    }
    output << ",\"animator\":";
    if (record.animator) {
        const auto &a = *record.animator;
        output << "{\"model\":\"" << escape_json(a.model) << "\",\"clip\":" << a.clip
               << ",\"time_seconds\":" << a.time_seconds << ",\"speed\":" << a.speed
               << ",\"playing\":" << (a.playing ? "true" : "false")
               << ",\"loop\":" << (a.loop ? "true" : "false") << '}';
    } else
        output << "null";
    output << ",\"model_node\":";
    if (record.model_node) {
        output << "{\"root\":\"" << record.model_node->root.to_string()
               << "\",\"node\":" << record.model_node->node << '}';
    } else
        output << "null";
    output << ",\"light\":";
    if (record.light) {
        const auto &l = *record.light;
        output << "{\"type\":" << static_cast<unsigned>(l.type) << ",\"color\":";
        append_vec3(output, l.color);
        output << ",\"intensity\":" << l.intensity << ",\"attenuation\":";
        append_vec3(output, l.attenuation);
        output << ",\"inner_cone\":" << l.inner_cone << ",\"outer_cone\":" << l.outer_cone
               << ",\"range\":" << l.range << '}';
    } else
        output << "null";
    output << '}';
}

std::string_view field_type_name(const ReflectedFieldType type) {
    switch (type) {
    case ReflectedFieldType::string: return "string";
    case ReflectedFieldType::entity: return "entity";
    case ReflectedFieldType::vec3: return "vec3";
    case ReflectedFieldType::number: return "number";
    case ReflectedFieldType::boolean: return "boolean";
    case ReflectedFieldType::number_array:
        return "number_array";
    }
    return "unknown";
}

} // namespace

std::uint64_t Entity::packed() const {
    return (static_cast<std::uint64_t>(generation) << 32U) | index;
}

std::string Entity::to_string() const {
    if (!valid()) return "null";
    return std::to_string(index) + ':' + std::to_string(generation);
}

std::optional<Entity> Entity::parse(const std::string_view value) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) return std::nullopt;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    const auto index_result = std::from_chars(value.data(), value.data() + separator, index);
    const auto generation_result = std::from_chars(
        value.data() + separator + 1U, value.data() + value.size(), generation);
    if (index_result.ec != std::errc{} || index_result.ptr != value.data() + separator ||
        generation_result.ec != std::errc{} || generation_result.ptr != value.data() + value.size() ||
        generation == 0) {
        return std::nullopt;
    }
    return Entity{index, generation};
}

Entity Scene::create(std::string name, const Entity parent) {
    std::uint32_t index = 0;
    if (!free_indices_.empty()) {
        index = free_indices_.back();
        free_indices_.pop_back();
    } else {
        if (slots_.size() >= std::numeric_limits<std::uint32_t>::max()) return {};
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(SceneSlotState{});
    }
    auto& slot = slots_[index];
    slot.alive = true;
    slot.record = EntityRecord{std::move(name), {}, {}, std::nullopt, std::nullopt};
    const Entity created{index, slot.generation};
    if (parent.valid() && !set_parent(created, parent)) {
        slot.record.parent = {};
    }
    return created;
}

bool Scene::destroy(const Entity entity) {
    if (!contains(entity)) return false;
    destroy_recursive(entity);
    return true;
}

void Scene::destroy_recursive(const Entity entity) {
    const auto current_entities = entities();
    for (const auto candidate : current_entities) {
        const auto* record = get(candidate);
        if (record != nullptr && record->parent == entity) destroy_recursive(candidate);
    }
    auto& slot = slots_[entity.index];
    slot.alive = false;
    slot.record = {};
    ++slot.generation;
    if (slot.generation == 0) slot.generation = 1;
    free_indices_.push_back(entity.index);
}

bool Scene::contains(const Entity entity) const {
    return entity.valid() && entity.index < slots_.size() && slots_[entity.index].alive &&
           slots_[entity.index].generation == entity.generation;
}

EntityRecord* Scene::get(const Entity entity) {
    return contains(entity) ? &slots_[entity.index].record : nullptr;
}

const EntityRecord* Scene::get(const Entity entity) const {
    return contains(entity) ? &slots_[entity.index].record : nullptr;
}

std::vector<Entity> Scene::entities() const {
    std::vector<Entity> result;
    result.reserve(slots_.size() - free_indices_.size());
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].alive) result.push_back(Entity{index, slots_[index].generation});
    }
    return result;
}

bool Scene::set_name(const Entity entity, std::string name) {
    auto* record = get(entity);
    if (record == nullptr || name.empty() || name.size() > 128U) return false;
    record->name = std::move(name);
    return true;
}

bool Scene::set_transform(const Entity entity, const Transform& transform) {
    auto* record = get(entity);
    if (record == nullptr) return false;
    record->transform = transform;
    return true;
}

bool Scene::set_parent(const Entity entity, const Entity parent) {
    auto* record = get(entity);
    if (record == nullptr || (parent.valid() && !contains(parent)) || would_create_cycle(entity, parent)) {
        return false;
    }
    record->parent = parent;
    return true;
}

bool Scene::set_camera(const Entity entity, std::optional<Camera> camera) {
    auto* record = get(entity);
    if (record == nullptr) return false;
    if (camera.has_value()) {
        if (!std::isfinite(camera->field_of_view_y_degrees) || !std::isfinite(camera->near_plane) ||
            !std::isfinite(camera->far_plane) || camera->field_of_view_y_degrees <= 1.0 ||
            camera->field_of_view_y_degrees >= 179.0 || camera->near_plane <= 0.0 ||
            camera->far_plane <= camera->near_plane ||
            !std::isfinite(camera->orthographic_height) || camera->orthographic_height < 0.0) {
            return false;
        }
        if (camera->active) {
            for (auto& slot : slots_) {
                if (slot.alive && slot.record.camera.has_value()) slot.record.camera->active = false;
            }
        }
    }
    record->camera = camera;
    return true;
}

bool Scene::set_mesh_renderer(const Entity entity, std::optional<MeshRenderer> renderer) {
    auto* record = get(entity);
    if (record == nullptr) return false;
    if (renderer.has_value() &&
        (renderer->mesh.empty() || renderer->material.empty() || renderer->mesh.size() > 128U ||
         renderer->material.size() > 128U || renderer->morph_weights.size() > 64U)) {
        return false;
    }
    if (renderer)
        for (const auto w : renderer->morph_weights) {
            if (!std::isfinite(w) || std::abs(w) > 100.0)
                return false;
        }
    record->mesh_renderer = std::move(renderer);
    return true;
}

bool Scene::set_animator(const Entity entity, std::optional<Animator> animator) {
    auto *record = get(entity);
    if (!record)
        return false;
    if (animator && (animator->model.empty() || animator->model.size() > 128U ||
                     !std::isfinite(animator->time_seconds) || animator->time_seconds < 0.0 ||
                     !std::isfinite(animator->speed) || std::abs(animator->speed) > 100.0))
        return false;
    record->animator = std::move(animator);
    return true;
}

bool Scene::set_light(const Entity entity, std::optional<Light> light) {
    auto *record = get(entity);
    if (!record)
        return false;
    if (light) {
        const auto valid = [](const Vec3 &v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.x >= 0.0 &&
                   v.y >= 0.0 && v.z >= 0.0;
        };
        if (static_cast<unsigned>(light->type) > 2U || !valid(light->color) ||
            !valid(light->attenuation) || !std::isfinite(light->intensity) ||
            light->intensity < 0.0 || !std::isfinite(light->inner_cone) ||
            !std::isfinite(light->outer_cone) || light->inner_cone < 0.0 ||
            light->outer_cone <= 0.0 || light->outer_cone > 1.5707963267948966 ||
            light->inner_cone > light->outer_cone || !std::isfinite(light->range) ||
            light->range < 0.0)
            return false;
    }
    record->light = std::move(light);
    return true;
}

std::optional<Entity> Scene::active_camera() const {
    for (std::uint32_t index = 0; index < slots_.size(); ++index) {
        const auto& slot = slots_[index];
        if (slot.alive && slot.record.camera.has_value() && slot.record.camera->active) {
            return Entity{index, slot.generation};
        }
    }
    return std::nullopt;
}

bool Scene::would_create_cycle(const Entity entity, Entity parent) const {
    while (parent.valid()) {
        if (parent == entity) return true;
        const auto* record = get(parent);
        if (record == nullptr) return false;
        parent = record->parent;
    }
    return false;
}

SceneState Scene::capture_state() const {
    return SceneState{slots_, free_indices_};
}

void Scene::restore_state(SceneState state) {
    slots_ = std::move(state.slots);
    free_indices_ = std::move(state.free_indices);
}

std::string Scene::entity_json(const Entity entity) const {
    const auto* record = get(entity);
    if (record == nullptr) return "null";
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    append_entity(output, entity, *record);
    return output.str();
}

std::string Scene::list_json() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "{\"entities\":[";
    const auto current_entities = entities();
    for (std::size_t index = 0; index < current_entities.size(); ++index) {
        if (index != 0) output << ',';
        const auto entity = current_entities[index];
        append_entity(output, entity, *get(entity));
    }
    output << "]}";
    return output.str();
}

std::string Scene::serialize_json() const {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output << "{\"format\":\"relay.scene\",\"version\":4,\"components\":[";
    const auto& descriptors = component_descriptors();
    for (std::size_t descriptor_index = 0; descriptor_index < descriptors.size(); ++descriptor_index) {
        if (descriptor_index != 0) output << ',';
        const auto& descriptor = descriptors[descriptor_index];
        output << "{\"name\":\"" << descriptor.name << "\",\"stable_id\":" << descriptor.stable_id
               << ",\"fields\":[";
        for (std::size_t field_index = 0; field_index < descriptor.fields.size(); ++field_index) {
            if (field_index != 0) output << ',';
            const auto& field = descriptor.fields[field_index];
            output << "{\"name\":\"" << field.name << "\",\"type\":\""
                   << field_type_name(field.type) << "\"}";
        }
        output << "]}";
    }
    const auto entity_list = list_json();
    output << "],\"scene\":{" << entity_list.substr(1U, entity_list.size() - 2U)
           << ",\"allocator\":{\"slot_generations\":[";
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (index != 0) output << ',';
        output << slots_[index].generation;
    }
    output << "],\"free_indices\":[";
    for (std::size_t index = 0; index < free_indices_.size(); ++index) {
        if (index != 0) output << ',';
        output << free_indices_[index];
    }
    output << "]}}}";
    return output.str();
}

const std::vector<ComponentDescriptor>& Scene::component_descriptors() {
    static const std::vector<ComponentDescriptor> descriptors{
        {"Name", 0x01U, {{"value", ReflectedFieldType::string}}},
        {"Transform",
         0x02U,
         {{"position", ReflectedFieldType::vec3},
          {"rotation_degrees", ReflectedFieldType::vec3},
          {"scale", ReflectedFieldType::vec3}}},
        {"Hierarchy", 0x03U, {{"parent", ReflectedFieldType::entity}}},
        {"Camera",
         0x04U,
         {{"field_of_view_y_degrees", ReflectedFieldType::number},
          {"near_plane", ReflectedFieldType::number},
          {"far_plane", ReflectedFieldType::number},
          {"active", ReflectedFieldType::boolean},
          {"orthographic_height", ReflectedFieldType::number}}},
        {"MeshRenderer",
         0x05U,
         {{"mesh", ReflectedFieldType::string},
          {"material", ReflectedFieldType::string},
          {"morph_weights", ReflectedFieldType::number_array}}},
        {"Animator",
         0x06U,
         {{"model", ReflectedFieldType::string},
          {"clip", ReflectedFieldType::number},
          {"time_seconds", ReflectedFieldType::number},
          {"speed", ReflectedFieldType::number},
          {"playing", ReflectedFieldType::boolean},
          {"loop", ReflectedFieldType::boolean}}},
        {"ModelNode",
         0x07U,
         {{"root", ReflectedFieldType::entity}, {"node", ReflectedFieldType::number}}},
        {"Light",
         0x08U,
         {{"type", ReflectedFieldType::number},
          {"color", ReflectedFieldType::vec3},
          {"intensity", ReflectedFieldType::number},
          {"attenuation", ReflectedFieldType::vec3},
          {"inner_cone", ReflectedFieldType::number},
          {"outer_cone", ReflectedFieldType::number},
          {"range", ReflectedFieldType::number}}},
    };
    return descriptors;
}

} // namespace relay
