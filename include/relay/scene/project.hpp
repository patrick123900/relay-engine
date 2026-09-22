#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// A .relayproject file owns its containing folder. Paths are workspace-relative; opening
// a project never changes the process working directory.
struct Project {
    std::string filename;
    std::string name;
    std::vector<std::string> scenes;
    std::string startup_scene;
    [[nodiscard]] std::filesystem::path root() const;
    [[nodiscard]] std::string json() const;
};

[[nodiscard]] std::optional<std::filesystem::path> workspace_file(
    std::string_view directory, std::string_view filename, std::string_view suffix);
[[nodiscard]] std::optional<Project> load_project(std::string_view filename, std::string& error);
[[nodiscard]] bool save_project(const Project& project, std::string& error, bool create = false);
[[nodiscard]] std::vector<std::string> available_projects();
// Write a portable, uncompressed tar of on-disk project metadata, member scenes,
// and non-hidden assets. The archive is created under the project's exports folder.
[[nodiscard]] bool package_project(const Project& project, std::string_view filename,
                                   std::string& error, std::uint64_t& bytes);

} // namespace relay
