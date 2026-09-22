#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

struct Entity {
    std::uint32_t index{0};
    std::uint32_t generation{0};

    [[nodiscard]] constexpr bool valid() const { return generation != 0; }
    [[nodiscard]] std::uint64_t packed() const;
    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] static std::optional<Entity> parse(std::string_view value);

    auto operator<=>(const Entity&) const = default;
};

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    auto operator<=>(const Vec3&) const = default;
};

struct Transform {
    Vec3 position{};
    Vec3 rotation_degrees{};
    Vec3 scale{1.0, 1.0, 1.0};

    auto operator<=>(const Transform&) const = default;
};

struct Camera {
    double field_of_view_y_degrees{60.0};
    double near_plane{0.1};
    double far_plane{1000.0};
    bool active{true};
    // Zero selects perspective; otherwise the full orthographic viewport height.
    double orthographic_height{};
    // Exposure compensation in stops. Zero preserves the renderer's reference exposure.
    double exposure_ev{};

    auto operator<=>(const Camera&) const = default;
};

struct MeshRenderer {
    std::string mesh{"builtin.triangle"};
    std::string material{"builtin.orange"};
    std::vector<double> morph_weights{};

    auto operator<=>(const MeshRenderer&) const = default;
};

struct Animator {
    std::string model;
    std::uint32_t clip{};
    double time_seconds{};
    double speed{1.0};
    bool playing{false};
    bool loop{true};
    auto operator<=>(const Animator &) const = default;
};

struct TransformKeyframe {
    double time_seconds{};
    Transform value{};
    auto operator<=>(const TransformKeyframe&) const = default;
};

struct TransformAnimation {
    double time_seconds{};
    double duration_seconds{1.0};
    double speed{1.0};
    bool playing{false};
    bool loop{true};
    std::vector<TransformKeyframe> keys;
    auto operator<=>(const TransformAnimation&) const = default;
};

struct ModelNode {
    Entity root;
    std::uint32_t node{};
    auto operator<=>(const ModelNode &) const = default;
};

struct Light {
    enum class Type : std::uint8_t { directional, point, spot } type{Type::point};
    Vec3 color{1.0, 1.0, 1.0};
    double intensity{1.0};
    Vec3 attenuation{0.0, 0.0, 1.0};
    double inner_cone{0.0};
    double outer_cone{0.7853981633974483};
    double range{}; // Zero means infinite.
    auto operator<=>(const Light &) const = default;
};

enum class ReflectedFieldType { string, entity, vec3, number, boolean, number_array, object_array };

struct ReflectedField {
    std::string_view name;
    ReflectedFieldType type;
};

struct ComponentDescriptor {
    std::string_view name;
    std::uint32_t stable_id;
    std::vector<ReflectedField> fields;
};

struct EntityRecord {
    std::string name;
    Transform transform;
    Entity parent{};
    std::optional<Camera> camera;
    std::optional<MeshRenderer> mesh_renderer;
    std::optional<Animator> animator{};
    std::optional<TransformAnimation> transform_animation{};
    std::optional<ModelNode> model_node{};
    std::optional<Light> light{};
};

struct SceneSlotState {
    std::uint32_t generation{1};
    bool alive{false};
    EntityRecord record;
};

struct SceneState {
    std::vector<SceneSlotState> slots;
    std::vector<std::uint32_t> free_indices;
};

class Scene {
public:
    // A duplicated subtree is bounded so one call cannot grow the scene without limit.
    static constexpr std::size_t maximum_duplicate_entities = 4096;

    [[nodiscard]] Entity create(std::string name = "Entity", Entity parent = {});
    [[nodiscard]] bool destroy(Entity entity);
    // Copies `source` and everything beneath it, placing the copy beside the original. Components
    // are preserved, except that a copied camera is never the active one. Returns an invalid
    // entity if the source is gone or the subtree exceeds `maximum_duplicate_entities`.
    [[nodiscard]] Entity duplicate(Entity source);
    // Destroys every entity, leaving handles taken beforehand stale rather than reusable.
    void clear();
    [[nodiscard]] bool contains(Entity entity) const;
    [[nodiscard]] EntityRecord* get(Entity entity);
    [[nodiscard]] const EntityRecord* get(Entity entity) const;
    [[nodiscard]] std::vector<Entity> entities() const;

    [[nodiscard]] bool set_name(Entity entity, std::string name);
    [[nodiscard]] bool set_transform(Entity entity, const Transform& transform);
    [[nodiscard]] bool set_parent(Entity entity, Entity parent);
    [[nodiscard]] bool set_camera(Entity entity, std::optional<Camera> camera);
    [[nodiscard]] bool set_mesh_renderer(Entity entity, std::optional<MeshRenderer> renderer);
    [[nodiscard]] bool set_animator(Entity entity, std::optional<Animator> animator);
    [[nodiscard]] bool set_transform_animation(Entity entity,
                                               std::optional<TransformAnimation> animation);
    [[nodiscard]] bool set_light(Entity entity, std::optional<Light> light);
    [[nodiscard]] std::optional<Entity> active_camera() const;

    [[nodiscard]] SceneState capture_state() const;
    void restore_state(SceneState state);

    [[nodiscard]] std::string entity_json(Entity entity) const;
    [[nodiscard]] std::string list_json() const;
    [[nodiscard]] std::string serialize_json() const;
    [[nodiscard]] static const std::vector<ComponentDescriptor>& component_descriptors();

private:
    void destroy_recursive(Entity entity);
    [[nodiscard]] std::string unique_copy_name(std::string_view name) const;
    [[nodiscard]] bool would_create_cycle(Entity entity, Entity parent) const;

    std::vector<SceneSlotState> slots_;
    std::vector<std::uint32_t> free_indices_;
};

[[nodiscard]] Transform sample_transform_animation(const TransformAnimation& animation,
                                                   const Transform& fallback);

} // namespace relay
