#pragma once

#include "relay/scene/scene.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Templates are node trees (prefabs) saved in the project's templates/ folder. Instantiating
// copies them: later edits to a template do not reach earlier copies, and a node's displayed type
// stays derived from its components (see node_types.hpp). Built-in node types are created with
// apply_node_type instead.
inline constexpr std::string_view template_directory = "templates";
inline constexpr std::string_view template_suffix = ".relay-template.json";

struct NodeTemplate {
    std::string id;       // "project:<name>"
    std::string name;     // The file name without suffix.
    std::string type;     // Node type of the saved root.
    std::string path;     // Project-relative template file.
};


// The project's templates sorted by name. Unreadable files are skipped.
[[nodiscard]] std::vector<NodeTemplate> list_templates(
    const std::optional<std::filesystem::path>& project_root);
// Template names are file-safe: letters, digits, spaces, '-' and '_', at most 64 characters.
[[nodiscard]] bool valid_template_name(std::string_view name);

// Creates the template's node tree under `parent` and returns its root. `name`, when not empty,
// renames the root.
[[nodiscard]] std::optional<Entity> instantiate_template(
    Scene& scene, const std::optional<std::filesystem::path>& project_root,
    std::string_view id, Entity parent, std::string_view name, std::string& error);
// Saves `root` and its descendants as templates/<name>.relay-template.json.
[[nodiscard]] bool save_template(const Scene& scene, Entity root,
                                 const std::filesystem::path& project_root, std::string_view name,
                                 bool replace, std::string& error);

} // namespace relay
