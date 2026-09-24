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

struct BoxCollider {
    enum class Type : std::uint8_t { box, sphere, capsule, convex, mesh } type{Type::box};
    Vec3 center{};
    Vec3 half_extents{0.5, 0.5, 0.5};
    double radius{0.5};
    double half_height{0.5}; // Capsule cylinder, excluding the round ends.
    // Convex and mesh shapes use this registered mesh, or the entity's renderer mesh when empty.
    std::string mesh{};
    bool enabled{true};
    std::uint32_t layer{1};
    std::uint32_t mask{0xffffffffU};
    auto operator<=>(const BoxCollider&) const = default;
};

struct PhysicsBody {
    enum class Type : std::uint8_t { static_body, dynamic } type{Type::dynamic};
    double mass{1.0};
    double gravity_scale{1.0};
    double restitution{0.0};
    double friction{0.2};
    double linear_damping{0.05};
    double angular_damping{0.05};
    // Keeps a dynamic body upright: collisions move it but never turn it, as characters need.
    bool lock_rotation{};
    auto operator<=>(const PhysicsBody&) const = default;
};

// Links this node's physics body to another node's body, or to a fixed point in the world, during
// Run Game. Anchor and axis are in this node's local space, captured when the game starts. At least
// one of the two bodies must be dynamic for the joint to do anything.
struct Joint {
    enum class Type : std::uint8_t { fixed, point, hinge, slider, distance } type{Type::hinge};
    Entity connected{};      // Invalid means the world.
    Vec3 anchor{};           // Pivot, in this node's local space.
    Vec3 axis{0.0, 1.0, 0.0}; // Hinge or slider axis, in this node's local space.
    // Distance joints: the far end, in the connected node's local space (world space for the world).
    Vec3 connected_anchor{};
    // Hinge angle in degrees (min in [-180, 0], max in [0, 180]); slider travel in metres from the
    // start (min <= 0 <= max); distance length in metres (0 <= min <= max). Without limits a
    // distance joint keeps its starting length.
    bool limits{};
    double limit_min{-45.0};
    double limit_max{45.0};
    // Hinge and slider: drive at motor_speed (degrees or metres per second) with at most
    // motor_force (newton metres or newtons).
    bool motor{};
    double motor_speed{90.0};
    double motor_force{1000.0};
    // Distance: a spring of this frequency (Hz) and damping ratio holds the limits; 0 is rigid.
    double spring_frequency{};
    double spring_damping{0.5};
    bool collide_connected{}; // Joined bodies pass through each other unless this is set.
    bool enabled{true};
    auto operator<=>(const Joint&) const = default;
};

// Default limits for a joint type: +-45 degrees for hinges, +-1 m for sliders, 0 to 2 m for
// distance joints.
void set_default_joint_limits(Joint& joint);
[[nodiscard]] std::string_view joint_type_name(Joint::Type type);
[[nodiscard]] std::optional<Joint::Type> joint_type_from_name(std::string_view name);

// An authored value for one of a behaviour's declared properties. Properties a node does not
// override keep the default written in the script's code.
struct ScriptProperty {
    enum class Type : std::uint8_t { boolean, number, vector, text } type{Type::number};
    std::string name;
    bool boolean{};
    double number{};
    Vec3 vector{};
    std::string text;
    auto operator<=>(const ScriptProperty&) const = default;
};

// A script component: one native gameplay behaviour, by its registered class name, run on the
// entity during Run Game. An entity may carry several, in order.
struct Script {
    std::string behaviour;
    bool enabled{true};
    std::vector<ScriptProperty> properties;
    auto operator<=>(const Script&) const = default;
};

inline constexpr std::size_t maximum_scripts_per_entity = 32U;
inline constexpr std::size_t maximum_script_properties = 64U;
inline constexpr std::size_t maximum_script_text_bytes = 1024U;

// Behaviour and property names are C++ identifiers of at most 128 characters.
[[nodiscard]] bool valid_behaviour_name(std::string_view name);
[[nodiscard]] bool valid_script(const Script& script);
[[nodiscard]] std::string_view script_property_type_name(ScriptProperty::Type type);
[[nodiscard]] std::optional<ScriptProperty::Type> script_property_type_from_name(std::string_view name);

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
    std::optional<BoxCollider> collider{};
    std::optional<PhysicsBody> physics_body{};
    std::vector<Script> scripts{};
    std::optional<Joint> joint{};
};

// The node type shown to people and agents, derived from the components an entity has now by
// walking the node type tree (see node_types.hpp). Scripts do not affect it.
[[nodiscard]] std::string_view node_type(const EntityRecord& record);

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
    // Joints connected to a destroyed node are disabled and fall back to the world connection.
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
    [[nodiscard]] bool set_collider(Entity entity, std::optional<BoxCollider> collider);
    [[nodiscard]] bool set_physics_body(Entity entity, std::optional<PhysicsBody> body);
    [[nodiscard]] bool set_scripts(Entity entity, std::vector<Script> scripts);
    // The connected node must exist and differ from `entity`.
    [[nodiscard]] bool set_joint(Entity entity, std::optional<Joint> joint);
    [[nodiscard]] std::optional<Entity> active_camera() const;

    [[nodiscard]] SceneState capture_state() const;
    void restore_state(SceneState state);

    [[nodiscard]] std::string entity_json(Entity entity) const;
    // Includes each entity's derived node type, which scene files do not store.
    [[nodiscard]] std::string list_json() const;
    [[nodiscard]] std::string serialize_json() const;
    [[nodiscard]] static const std::vector<ComponentDescriptor>& component_descriptors();

private:
    [[nodiscard]] std::string list_json(bool derived) const;
    void destroy_recursive(Entity entity);
    [[nodiscard]] std::string unique_copy_name(std::string_view name) const;
    [[nodiscard]] bool would_create_cycle(Entity entity, Entity parent) const;

    std::vector<SceneSlotState> slots_;
    std::vector<std::uint32_t> free_indices_;
};

[[nodiscard]] Transform sample_transform_animation(const TransformAnimation& animation,
                                                   const Transform& fallback);

} // namespace relay
