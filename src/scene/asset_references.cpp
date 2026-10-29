#include "relay/scene/asset_references.hpp"

#include "relay/render/asset_manifest.hpp"
#include "relay/scene/project_files.hpp"

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>
#include <unordered_set>

namespace relay {
namespace {

namespace fs = std::filesystem;

constexpr std::uintmax_t maximum_scanned_bytes = 16ULL * 1024ULL * 1024ULL;

std::string read_whole(const fs::path& path) {
    std::error_code ec;
    if (fs::file_size(path, ec) > maximum_scanned_bytes || ec) return {};
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// The text between double quotes, without the escapes JSON and C++ share.
std::vector<std::string> quoted_strings(const std::string_view text) {
    std::vector<std::string> strings;
    std::string current;
    bool inside = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!inside) {
            if (c == '"') { inside = true; current.clear(); }
            continue;
        }
        if (c == '\\' && i + 1U < text.size()) {
            const char next = text[++i];
            current += next == 'n' || next == 't' || next == 'r' ? ' ' : next;
        } else if (c == '"') {
            if (!current.empty() && current.size() <= 512U) strings.push_back(current);
            inside = false;
        } else if (c == '\n') {
            inside = false; // An unterminated string ends with its line.
        } else if (current.size() <= 512U) {
            current += c;
        }
    }
    return strings;
}

bool text_like(const std::string& name) {
    const auto kind = asset_kind_of(name, false);
    return kind == AssetKind::scene || kind == AssetKind::node_template || kind == AssetKind::material ||
           (kind == AssetKind::shader && name.ends_with(".relay-shader")) || kind == AssetKind::text;
}

std::string filename_of(const std::string& name) { return fs::path{name}.filename().string(); }

std::string stem_of(const std::string& name) {
    const auto file = filename_of(name);
    return file.substr(0, file.find('.'));
}

} // namespace

bool find_referenced_assets(const Project& project, const std::vector<ProjectFile>& files,
                            const std::string& project_metadata,
                            const std::vector<std::string>& always_include, AssetReferences& references,
                            std::string& error) {
    references = {};
    std::map<std::string, const ProjectFile*> candidates;
    for (const auto& file : files)
        if (!file.source.empty()) candidates.emplace(file.name, &file);
    const auto root = project.root();

    // The record of imported models. Scenes name imported meshes and materials by identities
    // ("asset.<content id>.mesh.0"), which lead back to the model file they came from.
    ImportManifest manifest;
    std::string manifest_error;
    if (!manifest.load(root, manifest_error)) {
        error = "the import record is unreadable: " + manifest_error;
        return false;
    }
    std::map<std::string, std::string> model_of_content;
    for (const auto& entry : manifest.entries()) model_of_content.emplace(entry.content_id, entry.source);

    std::set<std::string>& kept = references.kept;
    std::deque<std::string> queue;
    bool adding_dependency = false;
    const auto keep = [&](const std::string& name) {
        if (!candidates.contains(name)) return;
        if (kept.insert(name).second) {
            queue.push_back(name);
            if (adding_dependency) references.import_only.insert(name);
        } else if (!adding_dependency) {
            references.import_only.erase(name);
        }
    };

    // A string from a project file: an exact name, or a name with ".mesh.0"-style suffixes.
    const auto strict = [&](const std::string& token) {
        if (token.rfind("asset.", 0) == 0) {
            const auto end = token.find('.', 6U);
            const auto found = model_of_content.find(token.substr(6U, end == std::string::npos ? end : end - 6U));
            if (found != model_of_content.end()) keep(found->second);
        }
        auto candidate = token;
        while (!candidate.empty()) {
            keep(candidate);
            const auto slash = candidate.rfind('/');
            const auto cut = candidate.find_last_of(".#");
            if (cut == std::string::npos || (slash != std::string::npos && cut < slash)) break;
            candidate.resize(cut);
        }
    };
    // The folders at the top of the project, to tell a script's path from any other text with a slash.
    std::set<std::string> top_folders;
    for (const auto& [name, file] : candidates) {
        (void)file;
        const auto slash = name.find('/');
        if (slash != std::string::npos) top_folders.insert(name.substr(0, slash));
    }
    std::set<std::string> warned;
    // A string from a script: strict matches plus whatever the code could be building.
    const auto generous = [&](const std::string& token) {
        strict(token);
        if (token.size() < 3U) return;
        const bool path = token.find('/') != std::string::npos;
        bool matched = candidates.contains(token);
        for (const auto& [name, file] : candidates) {
            (void)file;
            if (path) {
                if (name.rfind(token, 0) == 0) { keep(name); matched = true; }
                if (name.size() > token.size() && name.ends_with(token) && name[name.size() - token.size() - 1U] == '/') {
                    keep(name);
                    matched = true;
                }
            } else {
                const auto file_name = filename_of(name);
                if (file_name == token || (token.size() >= 4U && file_name.rfind(token, 0) == 0)) { keep(name); matched = true; }
                if (stem_of(name) == token) { keep(name); matched = true; }
            }
        }
        // "sounds/boom_" in a script, with a sounds folder but nothing starting that way, is probably a typo
        // or a path made up in code that the scan cannot follow.
        if (path && !matched && token.find(' ') == std::string::npos && token.size() >= 4U &&
            top_folders.contains(token.substr(0, token.find('/'))) && warned.insert(token).second)
            references.warnings.push_back("a script mentions \"" + token + "\", which matches no file in the project");
    };

    for (const auto& token : quoted_strings(project_metadata)) strict(token);
    for (const auto& scene : project.scenes) keep("scenes/" + scene);
    // Files and folders the profile always keeps, with everything they refer to.
    for (const auto& wanted : always_include)
        for (const auto& [name, file] : candidates) {
            (void)file;
            if (name == wanted || name.rfind(wanted + "/", 0) == 0) keep(name);
        }

    // Scripts are not shipped as sources, so they are read from the project.
    std::error_code ec;
    const auto scripts = root / "scripts";
    if (fs::is_directory(scripts, ec))
        for (fs::recursive_directory_iterator it(scripts, ec), end; !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec) || it->path().filename().string().starts_with('.')) continue;
            const auto name = it->path().generic_string();
            if (asset_kind_of(name, false) != AssetKind::script) continue;
            for (const auto& token : quoted_strings(read_whole(it->path()))) generous(token);
        }

    while (!queue.empty()) {
        const auto name = queue.front();
        queue.pop_front();
        if (text_like(name))
            for (const auto& token : quoted_strings(read_whole(candidates.at(name)->source))) strict(token);
        for (const auto& entry : manifest.entries())
            if (entry.source == name) {
                adding_dependency = true;
                for (const auto& dependency : entry.dependencies) {
                    if (dependency == name) continue;
                    keep(dependency);
                    keep((fs::path{name}.parent_path() / dependency).generic_string());
                }
                adding_dependency = false;
            }
    }
    // The record itself is not an asset, and what it describes ships already imported.
    for (const auto& [name, file] : candidates) {
        (void)file;
        if (!kept.contains(name) && name != ".relay-imports.json") references.dropped.push_back(name);
    }
    error.clear();
    return true;
}

} // namespace relay
