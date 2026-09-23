#pragma once

#include "relay/scene/scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Node types form a tree rooted at Node. A type adds components to those of its parent, so
// creating a RigidBody applies Node, then PhysicsBody (a collider), then RigidBody (a dynamic
// body). A node's type is never stored: node_type() walks down the tree, at each level taking the
// first child whose own components the node has, and reports the deepest type reached.
struct NodeTypeInfo {
    std::string_view id;          // "RigidBody"
    std::string_view name;        // "Rigid Body"
    std::string_view parent;      // Empty for Node.
    std::string_view description;
    std::vector<std::string_view> adds; // Component ids this type adds to its parent's.
    bool creatable{};             // Categories and imported models are not created directly.
};

// Parents precede their children; siblings are in matching order.
[[nodiscard]] const std::vector<NodeTypeInfo>& node_types();
[[nodiscard]] const NodeTypeInfo* find_node_type(std::string_view id);
// Component ids of the type and all its ancestors, root first.
[[nodiscard]] std::vector<std::string_view> node_type_components(std::string_view id);
// Gives a fresh entity the components of a creatable type and its ancestors, with editor defaults.
[[nodiscard]] bool apply_node_type(Scene& scene, Entity entity, std::string_view id,
                                   std::string& error);

} // namespace relay
