#pragma once

#include "relay/scene/scene.hpp"
#include <span>

namespace relay {

// A self-contained forest; parent and model-instance references within it are remapped on paste.
struct SceneClipboard {
    struct Node { Entity original; EntityRecord record; };
    std::vector<Node> nodes;
    std::vector<Entity> roots;
};

[[nodiscard]] std::optional<std::vector<Entity>> selection_roots(
    const Scene& scene, std::span<const Entity> selection);
[[nodiscard]] std::optional<SceneClipboard> copy_selection(
    const Scene& scene, std::span<const Entity> selection);
[[nodiscard]] std::vector<Entity> paste_selection(
    Scene& scene, const SceneClipboard& clipboard, Entity parent = {}, bool sibling = false);

} // namespace relay
