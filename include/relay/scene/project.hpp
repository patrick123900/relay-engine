#pragma once

#include "relay/core/input.hpp"
#include "relay/render/graphics_settings.hpp"

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// A .relayproject file owns its containing folder. Paths are workspace-relative; opening
// a project never changes the process working directory. Version 2 adds game settings.
struct Project {
    std::string filename;
    std::string name;
    std::vector<std::string> scenes;
    std::string startup_scene;
    // settings.input; none until someone saves a map, and the engine defaults apply meanwhile.
    std::optional<InputMap> input;
    // `input` came from a version 1 project's input.relay-input.json, which the next save removes.
    bool legacy_input_file{};
    // settings.graphics; none until someone saves settings, and the defaults apply meanwhile.
    std::optional<GraphicsSettings> graphics;
    [[nodiscard]] std::filesystem::path root() const;
    // The file contents; protocol replies leave the settings out.
    [[nodiscard]] std::string json(bool with_settings = true) const;
};

[[nodiscard]] std::optional<std::filesystem::path> workspace_file(
    std::string_view directory, std::string_view filename, std::string_view suffix);
inline constexpr std::uint32_t project_file_version = 2;
// Reads versions 1 and 2. `warning` reports a legacy input map that could not be read.
[[nodiscard]] std::optional<Project> load_project(std::string_view filename, std::string& error);
[[nodiscard]] std::optional<Project> load_project(std::string_view filename, std::string& error,
                                                  std::string& warning);
[[nodiscard]] bool save_project(const Project& project, std::string& error, bool create = false);
[[nodiscard]] std::vector<std::string> available_projects();
// Write a portable, uncompressed tar of on-disk project metadata, member scenes,
// and non-hidden assets. The archive is created under the project's exports folder.
[[nodiscard]] bool package_project(const Project& project, std::string_view filename,
                                   std::string& error, std::uint64_t& bytes);

} // namespace relay
