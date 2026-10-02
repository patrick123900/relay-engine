#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// External editors a script can be opened in. `command` is the launch template: arguments split on
// spaces (double quotes group), with {file}, {line} and {project} replaced; there is no shell.
struct IdePreset {
    std::string_view id;
    std::string_view name;
    std::string_view command; // Empty for the system default and for a custom command.
};

[[nodiscard]] const std::vector<IdePreset>& ide_presets();
[[nodiscard]] const IdePreset* find_ide_preset(std::string_view id);

struct IdeLaunch {
    std::string preset{"system"}; // An `ide_presets()` id, or "custom".
    std::string custom_command;   // The template used when the preset is "custom".
    std::filesystem::path file;
    int line{1};
    std::filesystem::path project; // The project folder, so editors open it as the workspace.
};

// Splits a launch template into arguments, substituting its placeholders. Empty when the template
// is empty or has an unclosed quote.
[[nodiscard]] std::vector<std::string> ide_command_line(std::string_view command_template,
                                                        const std::filesystem::path& file, int line,
                                                        const std::filesystem::path& project);
// Whether a program named by the first word of a launch can be found on the PATH (or exists as a
// path).
[[nodiscard]] bool ide_program_found(const std::string& program);

// Starts the user's editor on a file without blocking and returns an error message, or empty on
// success. A human-only editor action, like show_in_file_browser; it is not part of the agent-facing
// control protocol.
[[nodiscard]] std::string open_in_ide(const IdeLaunch& launch);

} // namespace relay
