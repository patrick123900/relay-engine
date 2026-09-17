#include "relay/scene/scene_edit.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace relay {

std::optional<std::vector<Entity>> selection_roots(const Scene& scene,
                                                  const std::span<const Entity> selection) {
    if (selection.empty() || selection.size() > Scene::maximum_duplicate_entities) return {};
    std::set<Entity> selected(selection.begin(), selection.end());
    for (const auto entity : selected) if (!scene.contains(entity)) return {};
    std::vector<Entity> roots;
    for (const auto entity : selection) {
        if (!selected.erase(entity)) continue;
        auto parent = scene.get(entity)->parent;
        bool covered = false;
        while (scene.contains(parent)) {
            if (std::find(selection.begin(), selection.end(), parent) != selection.end()) {
                covered = true;
                break;
            }
            parent = scene.get(parent)->parent;
        }
        if (!covered) roots.push_back(entity);
    }
    return roots;
}

std::optional<SceneClipboard> copy_selection(const Scene& scene,
                                            const std::span<const Entity> selection) {
    auto roots = selection_roots(scene, selection);
    if (!roots) return {};
    SceneClipboard result;
    result.roots = *roots;
    std::map<Entity, std::vector<Entity>> children;
    for (const auto entity : scene.entities()) children[scene.get(entity)->parent].push_back(entity);
    auto queue = *roots;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        if (queue.size() > Scene::maximum_duplicate_entities) return {};
        const auto entity = queue[i];
        result.nodes.push_back({entity, *scene.get(entity)});
        for (const auto child : children[entity]) queue.push_back(child);
    }
    // Partial imported-node copies retain their authored components; never bind them to an unrelated model
    // after changing scenes (handles alone do not identify a scene).
    std::set<Entity> included(queue.begin(), queue.end());
    for (auto& node : result.nodes)
        if (node.record.model_node && !included.contains(node.record.model_node->root))
            node.record.model_node.reset();
    return result;
}

std::vector<Entity> paste_selection(Scene& scene, const SceneClipboard& clipboard,
                                    const Entity parent, const bool sibling) {
    if (clipboard.nodes.empty() || clipboard.nodes.size() > Scene::maximum_duplicate_entities ||
        (parent.valid() && !scene.contains(parent))) return {};
    const auto before = scene.capture_state();
    std::map<Entity, Entity> copies;
    for (const auto& node : clipboard.nodes) {
        const auto copy = scene.create(node.record.name);
        if (!copy.valid()) { scene.restore_state(before); return {}; }
        copies[node.original] = copy;
    }
    for (const auto& node : clipboard.nodes) {
        auto record = node.record;
        const auto found = copies.find(record.parent);
        record.parent = found != copies.end() ? found->second
                        : sibling && scene.contains(record.parent) ? record.parent : parent;
        if (record.camera) record.camera->active = false;
        if (record.model_node) record.model_node->root = copies.at(record.model_node->root);
        *scene.get(copies.at(node.original)) = std::move(record);
    }
    std::set<std::string> names;
    for (const auto entity : scene.entities()) names.insert(scene.get(entity)->name);
    std::vector<Entity> roots;
    for (const auto root : clipboard.roots) {
        const auto found = copies.find(root);
        if (found == copies.end()) { scene.restore_state(before); return {}; }
        auto* record = scene.get(found->second);
        auto base = record->name;
        if (const auto marker = base.rfind(" Copy"); marker != std::string::npos) {
            const auto suffix = base.substr(marker + 5);
            if (suffix.empty() || suffix.find_first_not_of(" 0123456789") == std::string::npos)
                base.resize(marker);
        }
        if (base.empty()) base = record->name;
        base.resize(std::min<std::size_t>(base.size(), 100));
        auto name = base + " Copy";
        for (std::size_t suffix = 2; names.contains(name); ++suffix)
            name = base + " Copy " + std::to_string(suffix);
        record->name = name;
        names.insert(name);
        roots.push_back(found->second);
    }
    return roots;
}

} // namespace relay
