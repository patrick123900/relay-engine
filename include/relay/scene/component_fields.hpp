#pragma once

// Named fields of engine components, for gameplay scripts: "collider.radius",
// "physics_body.mass", "light.color". A name is the component id (as in component.add) and the
// field, as the component stores it. Interface controls and particle emitters have their own,
// richer field tables (ui.hpp and particles.hpp), so they are not listed here.

#include "relay/scene/scene.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace relay {

enum class ComponentFieldType : std::uint8_t { boolean, number, integer, vec3, choice, text };

struct ComponentFieldInfo {
    std::string_view name; // "<component>.<field>".
    ComponentFieldType type{};
    std::vector<std::string_view> choices; // Choice fields, in the stored enum's order.
};

// Booleans, numbers (integers included), vectors, and text (choices by name).
using ComponentFieldValue = std::variant<bool, double, Vec3, std::string>;

[[nodiscard]] const std::vector<ComponentFieldInfo>& component_fields();
[[nodiscard]] const ComponentFieldInfo* find_component_field(std::string_view name);
// The component id a field name starts with: "collider" for "collider.radius".
[[nodiscard]] std::string_view component_field_component(std::string_view name);
// Nothing when the name is unknown or the entity lacks the component.
[[nodiscard]] std::optional<ComponentFieldValue> component_field_value(const EntityRecord& record,
                                                                        std::string_view name);
// Sets one field through the scene's validating setter. A value of the wrong kind, an unknown
// choice or a value the component refuses leaves it unchanged and explains why in `error`.
// Asset names (meshes, materials, sounds) are not checked here.
[[nodiscard]] bool set_component_field(Scene& scene, Entity entity, std::string_view name,
                                       const ComponentFieldValue& value, std::string& error);

} // namespace relay
