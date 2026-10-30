#pragma once

#include "relay/audio/audio_settings.hpp"
#include "relay/scene/build_settings.hpp"
#include "relay/core/input.hpp"
#include "relay/render/graphics_settings.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
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
    // settings.audio, the mixer's buses; none until someone changes them.
    std::optional<AudioSettings> audio;
    // settings.build, the options of Run > Build Project; none until someone changes them.
    std::optional<BuildSettings> build;
    [[nodiscard]] std::filesystem::path root() const;
    // The file contents; protocol replies leave the settings out.
    [[nodiscard]] std::string json(bool with_settings = true) const;
};

[[nodiscard]] std::optional<std::filesystem::path> workspace_file(
    std::string_view directory, std::string_view filename, std::string_view suffix);
// The project file's path. A relative `filename` must lie inside the workspace (workspace_file).
// With `allow_absolute`, a path the person chose in a system file dialog is also accepted: absolute,
// without . or .. parts, wherever on disk it is. Only human-driven requests pass it; agents stay
// inside the workspace.
[[nodiscard]] std::optional<std::filesystem::path> project_file_path(std::string_view filename,
                                                                     bool allow_absolute);
inline constexpr std::uint32_t project_file_version = 2;
// Reads versions 1 and 2. `warning` reports a legacy input map that could not be read.
[[nodiscard]] std::optional<Project> load_project(std::string_view filename, std::string& error,
                                                  bool allow_absolute = false);
[[nodiscard]] std::optional<Project> load_project(std::string_view filename, std::string& error,
                                                  std::string& warning, bool allow_absolute = false);
[[nodiscard]] bool save_project(const Project& project, std::string& error, bool create = false);
[[nodiscard]] std::vector<std::string> available_projects();
// One file of a project package or build: where it is on disk and its path inside the archive.
struct ProjectFile {
    std::filesystem::path source; // Empty for generated content, which is `text`.
    std::string name;
    std::uint64_t size{};
    std::string text;
};

inline constexpr std::size_t maximum_package_files = 65535U;
inline constexpr std::uint64_t maximum_package_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

// Lists what a portable copy of the project needs, sorted by name: every scene, template, script,
// asset and import record, plus a normalized project.relayproject (generated, so `source` is
// empty; its text is returned in `metadata`). Private state and anything that is output rather
// than input is left out: hidden entries (except .relay-imports.json), captures, traces, exports
// and each folder in `excluded` (the build folder). Symlinks and oversized files are refused.
[[nodiscard]] bool collect_project_files(const Project& project,
                                         const std::vector<std::string>& excluded,
                                         std::vector<ProjectFile>& files, std::string& metadata,
                                         std::string& error);
// Write a portable copy of the project to the project's exports folder: an uncompressed tar or a
// zip (stored, no compression), chosen by the filename's .tar or .zip extension. Extracting it
// anywhere gives a folder that opens as an identical project. Refuses to overwrite.
// `directory` is where to write it: empty for the project's exports folder, otherwise an absolute
// folder or one relative to the project, created if needed.
[[nodiscard]] bool package_project(const Project& project, std::string_view filename,
                                   std::string_view directory, std::string& error,
                                   std::uint64_t& bytes, std::filesystem::path* written = nullptr);

[[nodiscard]] inline bool package_project(const Project& project, std::string_view filename,
                                          std::string& error, std::uint64_t& bytes) {
    return package_project(project, filename, {}, error, bytes);
}

// How a build reports on itself and how it is told to stop. Both may be used from another thread.
struct BuildControl {
    // The stage now running ("Compiling scripts"), how far through the whole build it is (0 to 1),
    // and what it is on. Called from the thread that builds, and from script compiler threads.
    std::function<void(std::string_view stage, double fraction, std::string_view detail)> progress;
    const std::atomic<bool>* cancel{};
    [[nodiscard]] bool cancelled() const { return cancel != nullptr && cancel->load(); }
};

// What Run > Build Project produced.
struct BuildReport {
    std::filesystem::path directory; // <output>/<configuration>
    std::uint64_t files{};
    std::uint64_t bytes{};
    // The game's program inside `directory`.
    bool executable{};
    std::filesystem::path executable_path;
    // The build stopped because it was told to, not because something failed.
    bool cancelled{};
    // Project files the game does not refer to and so does not ship (the first few hundred, sorted),
    // how many there were, and anything the scan was unsure of.
    std::vector<std::string> left_out;
    std::size_t left_out_count{};
    std::vector<std::string> warnings;
    // The newest glibc, libstdc++ and C++ ABI the program and its libraries ask of the system, and
    // the file that asks the most of glibc. Empty for targets that are not Linux.
    std::string glibc, glibc_from, libstdcxx, cxxabi;
};

// The targets a build can be made for: those a player exists for, installed or built in.
struct BuildTarget {
    std::string platform;
    std::string architecture;
    bool available{};
    bool native{}; // This machine's own platform and CPU.
};
[[nodiscard]] std::vector<BuildTarget> available_build_targets();

// The glibc that Relay's own games are meant to run on: a Linux player that needs a newer one
// leaves older systems out, and games built with it, and their scripts, may not start there.
inline constexpr const char* portable_glibc = "2.31";

// What Relay has for one build target. Players are modular: each is a folder below the players folder
// (beside Relay, or RELAY_PLAYERS_DIR) with a player.json saying which platform and architecture it makes
// games for, so any target a player exists for can be built for, including community ones. This machine's
// own Linux target is also served by the player Relay was built with (`built_in`) unless one is installed.
struct PlayerInfo {
    std::string platform;
    std::string architecture;
    std::string name;        // From player.json; empty when it has none.
    std::string description;
    std::string version;
    bool native{};
    bool installed{};
    bool built_in{};
    std::filesystem::path folder;  // Where an installed player lives.
    std::filesystem::path program; // The program its games are made from.
    std::size_t files{};
    std::uintmax_t bytes{};
    std::string glibc;      // Linux: the newest glibc its program and libraries ask for.
    std::string glibc_from; // The file that sets it.
};
// The players that exist: installed ones, and this machine's built-in one.
[[nodiscard]] std::vector<PlayerInfo> list_players();
[[nodiscard]] std::filesystem::path players_folder();
// Copies the player in `source` (its program, a shaders/ folder of .spv files and whatever else it needs,
// described by a player.json) in as the player for its target, replacing one already there. A folder
// without a player.json needs the platform and architecture given. `platform` and `architecture` say
// which target it was installed for.
bool install_player(const std::filesystem::path& source, const std::string& platform_hint,
                    const std::string& architecture_hint, std::string& platform, std::string& architecture,
                    std::string& error);
// Deletes an installed player; the built-in one cannot be removed.
bool remove_player(const std::string& platform, const std::string& architecture, std::string& error);

// What a build of `profile` would pack and leave out, without building.
struct BuildPreview {
    std::size_t packed{};
    std::vector<std::string> left_out; // Sorted.
    std::vector<std::string> warnings;
};
[[nodiscard]] bool preview_build(const Project& project, const BuildSettings& settings,
                                 const BuildProfile& profile, BuildPreview& preview, std::string& error);
// Builds the game for `profile` (one of `settings.profiles`) into <output folder>/<configuration>/
// (both configuration folders are created; the folder is project-relative or absolute). The
// result runs on its own: the game's program named for the game, data/ (the project, without
// script sources), scripts/ (the behaviours compiled for the configuration), shaders/, lib/ (the
// libraries the program needs that a system may lack), build.json and icon.png. A previous build
// of the same configuration is replaced. Needs the player program (relay_player) from this Relay.
[[nodiscard]] bool build_project(const Project& project, const BuildSettings& settings,
                                 const BuildProfile& profile, std::string& error, BuildReport& report,
                                 BuildControl* control = nullptr);

} // namespace relay
