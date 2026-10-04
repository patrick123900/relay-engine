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
// With `same_scene` false (the default) the clipboard may be pasted into another scene, so joints and
// script references to nodes outside the copied trees are cleared (handles do not identify a
// scene). With `same_scene` true, which duplicating and cloning in place use, they keep pointing at
// the nodes they named, as Scene::duplicate does.
[[nodiscard]] std::optional<SceneClipboard> copy_selection(
    const Scene& scene, std::span<const Entity> selection, bool same_scene = false);
[[nodiscard]] std::vector<Entity> paste_selection(
    Scene& scene, const SceneClipboard& clipboard, Entity parent = {}, bool sibling = false);

} // namespace relay
