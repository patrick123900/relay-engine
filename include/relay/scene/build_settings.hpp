#pragma once

#include "relay/core/json.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// How to build the game for one target. A project keeps several (a template per platform, say) and
// builds one at a time.
struct BuildProfile {
    // Unique within the project; shown in the Build Project window's list.
    std::string name{"Linux"};
    std::string target_platform{"linux"};
    std::string architecture{"x86_64"};
    // "release" or "debug". Each has its own folder below the build directory.
    std::string configuration{"release"};
    // The folder that holds one subfolder per configuration: project-relative, or an absolute path
    // for a folder elsewhere.
    std::string output_directory{"builds"};
    // How the game's window first opens: "windowed", "borderless" (fullscreen at the desktop's
    // size) or "fullscreen". The size is the window's client area when windowed.
    std::string window_mode{"windowed"};
    std::uint32_t window_width{1280};
    std::uint32_t window_height{720};
    // A build ships only the files the game refers to (see asset_references.hpp). Turn this on to
    // ship every file in the project, for a game that loads files by names its code builds up.
    bool include_unused_assets{false};
    // Cook the models' textures into BC1/BC3 blocks with mip chains (a fourth to an eighth of the
    // memory, nothing to generate at start), and wav sounds into lossless FLAC. Both are off for
    // a build that should keep its files exactly as authored.
    bool compress_textures{true};
    bool compress_audio{true};
    // Linux: the oldest glibc the game must run on ("2.31"), or empty to not check. A build whose
    // program or libraries ask for more fails, naming the file, so a game is not shipped that only
    // starts on the machine that built it.
    std::string minimum_glibc;
    // Project files and folders (project-relative, with forward slashes) that always ship, whatever
    // the scan finds, and whatever they refer to ships with them.
    std::vector<std::string> always_include;

    friend bool operator==(const BuildProfile&, const BuildProfile&) = default;
};

// Saved in the project file under settings.build. The game's identity is shared by every profile.
struct BuildSettings {
    // Empty means the project's name.
    std::string game_name;
    std::string description;
    std::string author;
    // YYYY-MM-DD, or empty for none.
    std::string release_date;
    // A project-relative PNG used as the executable's icon, or empty for Relay's own.
    std::string icon;
    // Dotted numbers with an optional -suffix, such as 1.0.0 or 0.3.1-beta.
    std::string version{"1.0.0"};
    // At least one profile; names are unique.
    std::vector<BuildProfile> profiles{BuildProfile{}};
    // The profile the window shows and Build uses.
    std::string active_profile{"Linux"};

    friend bool operator==(const BuildSettings&, const BuildSettings&) = default;
    [[nodiscard]] const BuildProfile* find_profile(std::string_view name) const;
    [[nodiscard]] BuildProfile* find_profile(std::string_view name);
    [[nodiscard]] const BuildProfile& active() const;
};

inline constexpr std::size_t maximum_game_name_length = 64;
inline constexpr std::size_t maximum_game_description_length = 1024;
inline constexpr std::size_t maximum_game_author_length = 128;
inline constexpr std::size_t maximum_game_version_length = 32;
inline constexpr std::size_t maximum_profile_name_length = 32;
inline constexpr std::size_t maximum_build_profiles = 16;
inline constexpr std::size_t maximum_output_directory_length = 512;
inline constexpr std::size_t maximum_always_include = 64;
inline constexpr std::uint32_t minimum_window_size = 320;
inline constexpr std::uint32_t maximum_window_size = 16384;

// A platform or architecture name: a lower-case word of letters, digits and underscores.
[[nodiscard]] bool valid_build_target_word(const std::string& word);
[[nodiscard]] const std::vector<std::string_view>& build_configurations();
[[nodiscard]] const std::vector<std::string_view>& build_window_modes();

// Checks every field; `error` names the first problem.
[[nodiscard]] bool validate_build_profile(const BuildProfile& profile, std::string& error);
[[nodiscard]] bool validate_build_settings(const BuildSettings& settings, std::string& error);
[[nodiscard]] std::string build_profile_json(const BuildProfile& profile);
[[nodiscard]] std::string build_settings_json(const BuildSettings& settings);
// Reads settings.build; unknown fields are rejected so typos do not silently do nothing.
[[nodiscard]] std::optional<BuildSettings> parse_build_settings(const JsonValue& value,
                                                                std::string& error);
// The name the game is shown and built under: the setting, else the project's name.
[[nodiscard]] std::string effective_game_name(const BuildSettings& settings,
                                              std::string_view project_name);
// The game name reduced to characters that are safe in a file name.
[[nodiscard]] std::string game_file_stem(std::string_view game_name);
// Where a profile's builds go: its output folder, below `project_root` when relative.
[[nodiscard]] std::filesystem::path build_output_root(const std::filesystem::path& project_root,
                                                      const BuildProfile& profile);
// `output_directory` as a path relative to the project when the folder lies inside it, else empty.
[[nodiscard]] std::string output_inside_project(const std::filesystem::path& project_root,
                                                const BuildProfile& profile);

} // namespace relay
