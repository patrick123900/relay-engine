#pragma once

#include "relay/scene/scene.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Engine components as people and agents see them: a node has a Transform and any mix of the
// others. Engine components keep typed storage in EntityRecord; scripts are the "script" kind,
// which a node may carry several of.
struct ComponentKind {
    std::string_view id;       // Wire identifier, as in component.add.
    std::string_view name;     // Display name.
    std::string_view category; // Browser grouping.
    bool addable{};            // Can be added from the component browser.
    bool removable{};
    bool multiple{};           // A node may carry more than one.
    std::string_view description;
};

[[nodiscard]] const std::vector<ComponentKind>& engine_components();
[[nodiscard]] const ComponentKind* find_component_kind(std::string_view id);
[[nodiscard]] bool has_component(const EntityRecord& record, std::string_view id);

// Adds a component with editor defaults: a camera becomes active only when the scene has none,
// keyframes start with one key holding the current transform. Scripts need `behaviour`.
[[nodiscard]] bool add_component(Scene& scene, Entity entity, std::string_view id,
                                 std::string_view behaviour, std::string& error);
// Removes one component; `index` selects among script components.
[[nodiscard]] bool remove_component(Scene& scene, Entity entity, std::string_view id,
                                    std::size_t index, std::string& error);

} // namespace relay
