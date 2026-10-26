#pragma once

#include "relay/scene/project.hpp"

#include <set>
#include <string>
#include <vector>

namespace relay {

// What a game build needs of a project's files. Starting from the project file, its scenes and
// its scripts, every quoted string in a scene, template, material or shader that names a project
// file pulls that file in, and so on through the files it names. Strings in script sources are
// matched generously, because scripts build paths and names in code: a string that starts a file's
// path or name, ends it, or is its file name or the name before its first dot keeps that file.
// Models keep what their import records list as dependencies. Nothing else ships.
struct AssetReferences {
    std::set<std::string> kept;     // Project-relative names that are needed.
    std::vector<std::string> dropped;
    // Kept only because a model's import reads them (its textures and buffers). A build that ships
    // the model already imported leaves them out, unless something else refers to them too.
    std::set<std::string> import_only;
};

// `files` is what collect_project_files listed (generated entries have an empty source).
[[nodiscard]] bool find_referenced_assets(const Project& project,
                                          const std::vector<ProjectFile>& files,
                                          const std::string& project_metadata,
                                          AssetReferences& references, std::string& error);

} // namespace relay
