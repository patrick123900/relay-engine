#include "relay/scene/templates.hpp"

#include "relay/scene/scene_edit.hpp"
#include "relay/scene/scene_io.hpp"

#include <algorithm>
#include <array>
#include <system_error>

namespace relay {
namespace {

namespace fs = std::filesystem;

std::optional<fs::path> template_file(const fs::path& root, const std::string_view name) {
    if (!valid_template_name(name)) return std::nullopt;
    std::error_code error;
    const auto directory = root / template_directory;
    if (fs::is_symlink(directory, error)) return std::nullopt;
    const auto path = directory / (std::string{name} + std::string{template_suffix});
    if (fs::is_symlink(path, error)) return std::nullopt;
    return path;
}

} // namespace

bool valid_template_name(const std::string_view name) {
    if (name.empty() || name.size() > 64U || name.front() == ' ' || name.back() == ' ' ||
        name.front() == '-')
        return false;
    return std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == ' ' || c == '-' || c == '_';
    });
}

std::vector<NodeTemplate> list_templates(const std::optional<fs::path>& project_root) {
    std::vector<NodeTemplate> project;
    if (!project_root) return project;
    std::error_code error;
    const auto directory = *project_root / template_directory;
    if (fs::is_symlink(directory, error) || !fs::is_directory(directory, error)) return project;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const auto filename = it->path().filename().string();
        if (filename.starts_with('.') || it->is_symlink(error) || !it->is_regular_file(error) ||
            !filename.ends_with(template_suffix) || project.size() == 512U)
            continue;
        const auto name = filename.substr(0, filename.size() - template_suffix.size());
        if (!valid_template_name(name)) continue;
        const auto loaded = load_scene_file(it->path());
        std::string type = "Node";
        if (!loaded) continue;
        for (const auto& slot : loaded.state->slots)
            if (slot.alive && !slot.record.parent.valid()) type = node_type(slot.record);
        project.push_back({"project:" + name, name, type,
                           std::string{template_directory} + "/" + filename});
    }
    std::sort(project.begin(), project.end(),
              [](const NodeTemplate& a, const NodeTemplate& b) { return a.name < b.name; });
    return project;
}

std::optional<Entity> instantiate_template(Scene& scene, const std::optional<fs::path>& project_root,
                                           const std::string_view id, const Entity parent,
                                           const std::string_view name, std::string& error) {
    if (parent.valid() && !scene.contains(parent)) {
        error = "invalid or stale parent";
        return std::nullopt;
    }
    std::optional<Entity> root;
    if (id.starts_with("project:")) {
        if (!project_root) {
            error = "project templates need an open project";
            return std::nullopt;
        }
        const auto path = template_file(*project_root, id.substr(8));
        std::error_code code;
        if (!path || !fs::is_regular_file(*path, code)) {
            error = "project template not found";
            return std::nullopt;
        }
        auto loaded = load_scene_file(*path);
        if (!loaded) {
            error = "invalid template file: " + loaded.error;
            return std::nullopt;
        }
        Scene source;
        source.restore_state(std::move(*loaded.state));
        std::vector<Entity> roots;
        for (const auto entity : source.entities())
            if (!source.get(entity)->parent.valid()) roots.push_back(entity);
        if (roots.size() != 1U) {
            error = "a template file must hold exactly one root node";
            return std::nullopt;
        }
        const auto clipboard = copy_selection(source, roots);
        const auto pasted = clipboard ? paste_selection(scene, *clipboard, parent)
                                      : std::vector<Entity>{};
        if (pasted.size() != 1U) {
            error = "could not place the template in the scene";
            return std::nullopt;
        }
        root = pasted.front();
    } else {
        error = "template ids start with project:";
        return std::nullopt;
    }
    if (!name.empty() && !scene.set_name(*root, std::string{name})) {
        error = "invalid node name";
        return std::nullopt;
    }
    return root;
}

bool save_template(const Scene& scene, const Entity root, const fs::path& project_root,
                   const std::string_view name, const bool replace, std::string& error) {
    const auto path = template_file(project_root, name);
    if (!path) {
        error = "template names use letters, digits, spaces, '-' and '_', up to 64 characters";
        return false;
    }
    if (!scene.contains(root)) {
        error = "invalid or stale entity";
        return false;
    }
    std::error_code code;
    if (!replace && fs::exists(*path, code)) {
        error = "a template with that name already exists";
        return false;
    }
    const std::array selection{root};
    const auto clipboard = copy_selection(scene, selection);
    if (!clipboard) {
        error = "could not copy the node tree";
        return false;
    }
    // The copy becomes a root in its own scene, so the template never refers back to this one.
    Scene isolated;
    if (paste_selection(isolated, *clipboard).size() != 1U) {
        error = "could not copy the node tree";
        return false;
    }
    return save_scene_file_atomic(isolated, *path, error);
}

} // namespace relay
