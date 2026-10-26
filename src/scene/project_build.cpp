#include "relay/scene/project.hpp"

#include "relay/core/pak.hpp"
#include "relay/core/process.hpp"
#include "relay/render/cooked_assets.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/scene/asset_references.hpp"
#include "relay/script/script_system.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

#ifndef RELAY_PLAYER_DEFAULT_PATH
#define RELAY_PLAYER_DEFAULT_PATH ""
#endif

namespace relay {
namespace {

constexpr std::string_view game_archive_name = "game.pak";

bool copy_regular_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) return false;
    return std::filesystem::copy_file(from, to, std::filesystem::copy_options::none, ec) && !ec;
}

bool write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    stream.flush();
    return static_cast<bool>(stream);
}

// The program a built game runs: RELAY_PLAYER_BINARY, else relay_player beside the running
// program, else where this build of Relay put it.
std::filesystem::path find_player_binary() {
    std::error_code ec;
    if (const char* chosen = std::getenv("RELAY_PLAYER_BINARY"); chosen && *chosen) return chosen;
#ifndef _WIN32
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        const auto beside = self.parent_path() / "relay_player";
        if (std::filesystem::is_regular_file(beside, ec)) return beside;
    }
#endif
    return RELAY_PLAYER_DEFAULT_PATH;
}

// Libraries every Linux system provides, or that must come from the machine's own drivers and
// desktop; everything else the player needs ships beside it.
bool system_library(const std::string& name) {
    static const char* const prefixes[] = {
        "ld-linux", "libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so", "libutil.so",
        "libresolv.so", "libnsl.so", "libanl.so", "libvulkan.so", "libGL", "libEGL", "libX", "libxcb",
        "libwayland", "libdrm", "libgbm", "libasound", "libpulse", "libpipewire", "libudev", "libdbus",
        "libsystemd", "libxkbcommon", "libdecor", "libgio", "libglib", "libgobject", "libffi", "libselinux",
        "libpcre", "libmount", "libblkid", "libcap", "libbsd", "libmd.so"};
    return std::any_of(std::begin(prefixes), std::end(prefixes),
                       [&](const char* prefix) { return name.rfind(prefix, 0) == 0; });
}

// The shared libraries `program` loads, as soname -> real file, from ldd.
bool program_libraries(const std::filesystem::path& program,
                       std::vector<std::pair<std::string, std::filesystem::path>>& libraries,
                       std::string& error) {
    const auto result = run_process({"ldd", program.string()}, std::chrono::seconds{30}, 256U * 1024U);
    if (!result.started || result.exit_code != 0) {
        error = "cannot list the libraries of " + program.string() + " with ldd";
        return false;
    }
    std::istringstream lines{result.output};
    for (std::string line; std::getline(lines, line);) {
        const auto arrow = line.find(" => ");
        if (arrow == std::string::npos) continue;
        auto name = line.substr(0, arrow);
        name.erase(0, name.find_first_not_of(" \t"));
        auto rest = line.substr(arrow + 4U);
        if (rest.rfind("not found", 0) == 0) {
            error = "the player needs " + name + ", which is not installed";
            return false;
        }
        const auto paren = rest.rfind(" (");
        if (paren != std::string::npos) rest.erase(paren);
        name = std::filesystem::path{name}.filename().string();
        if (rest.empty() || system_library(name)) continue;
        libraries.emplace_back(std::move(name), std::filesystem::path{rest});
    }
    return true;
}

// No component of `path` below `root` may be a link, so a build never writes outside the project.
bool free_of_links(const std::filesystem::path& root, const std::filesystem::path& relative) {
    std::error_code ec;
    auto current = root;
    if (std::filesystem::is_symlink(current, ec) || ec) return false;
    for (const auto& part : relative) {
        current /= part;
        if (std::filesystem::is_symlink(current, ec)) return false;
    }
    return true;
}

} // namespace

bool build_project(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                   std::string& error, BuildReport& report) {
    report = {};
    if (!validate_build_settings(settings, error) || !validate_build_profile(profile, error)) return false;
    const auto root = project.root();
    std::error_code ec;
    // A folder inside the project must not pass through a link; one elsewhere is the user's choice.
    const bool external = std::filesystem::path{profile.output_directory}.is_absolute();
    if (!external && !free_of_links(root, profile.output_directory)) {
        error = "the build folder passes through a symlink";
        return false;
    }
    std::uint64_t icon_size{};
    const auto icon_source = root / settings.icon;
    if (!settings.icon.empty()) {
        if (!free_of_links(root, settings.icon) || !std::filesystem::is_regular_file(icon_source, ec) ||
            ec || (icon_size = std::filesystem::file_size(icon_source, ec), ec) ||
            icon_size > 16ULL * 1024ULL * 1024ULL) {
            error = "the icon " + settings.icon + " is missing or too large";
            return false;
        }
    }

    // The game's program, from this installation of Relay.
    if (profile.target_platform != "linux") {
        error = "only Linux games can be built for now";
        return false;
    }
#if !defined(__x86_64__)
    error = "this Relay produces players for its own CPU, which is not x86_64";
    return false;
#endif
    const auto player = find_player_binary();
    if (player.empty() || !std::filesystem::is_regular_file(player, ec)) {
        error = "the game player was not found" + (player.empty() ? std::string{} : " at " + player.generic_string()) +
                "; build the relay_player target or set RELAY_PLAYER_BINARY";
        return false;
    }
    const auto shader_source = player.parent_path() / "shaders";
    std::vector<std::filesystem::path> shader_files;
    for (std::filesystem::directory_iterator it(shader_source, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file() && it->path().extension() == ".spv") shader_files.push_back(it->path());
    if (shader_files.empty()) {
        error = "the player's compiled shaders were not found in " + shader_source.generic_string();
        return false;
    }
    std::vector<std::pair<std::string, std::filesystem::path>> libraries;
    if (!program_libraries(player, libraries, error)) return false;

    std::vector<ProjectFile> files;
    std::string metadata;
    // The build folder is output, so neither it nor the default builds folder belongs in the game.
    // The game ships its scripts compiled, so their sources stay behind.
    std::vector<std::string> excluded{"builds", std::string{script_source_directory}};
    for (const auto& other : settings.profiles)
        if (const auto inside = output_inside_project(root, other); !inside.empty())
            excluded.push_back(inside);
    if (!collect_project_files(project, excluded, files, metadata, error)) return false;

    // Only what the game refers to ships, unless the profile says everything should.
    std::set<std::string> kept;
    std::set<std::string> import_only;
    std::size_t dropped = 0;
    if (!profile.include_unused_assets) {
        AssetReferences references;
        if (!find_referenced_assets(project, files, metadata, references, error)) return false;
        dropped = references.dropped.size();
        kept = std::move(references.kept);
        import_only = std::move(references.import_only);
    } else {
        for (const auto& file : files)
            if (!file.source.empty()) kept.insert(file.name);
    }

    // Models and shaders ship cooked: imported and compiled now, so the game neither carries the
    // importer and the shader compiler nor spends its start-up running them.
    std::vector<PakInput> inputs;
    std::set<std::string> cooked_sources;
    std::uint32_t cooked_models = 0;
    std::uint32_t cooked_shaders = 0;
    {
        ImportManifest imports;
        if (!imports.load(root, error)) return false;
        std::set<std::string> models;
        for (const auto& entry : imports.entries())
            if (kept.contains(entry.source)) models.insert(entry.source);
        if (!models.empty()) {
            std::string bytes;
            if (!cook_models(root, models, bytes, error)) {
                error = "cannot cook the models: " + error;
                return false;
            }
            inputs.push_back({std::string{cooked_models_name}, {}, std::move(bytes)});
            cooked_models = static_cast<std::uint32_t>(models.size());
            cooked_sources.insert(models.begin(), models.end());
        }
        for (const auto& name : kept)
            if (name.ends_with(".relay-shader")) {
                std::string bytes;
                if (!cook_shader(root, name, bytes, error)) {
                    error = "cannot compile the shader " + error;
                    return false;
                }
                inputs.push_back({std::string{cooked_shader_directory} + name + ".bin", {}, std::move(bytes)});
                ++cooked_shaders;
                cooked_sources.insert(name);
            }
    }
    for (const auto& file : files) {
        if (file.source.empty()) {
            inputs.push_back({file.name, {}, file.text});
        } else if (file.name != ".relay-imports.json" && kept.contains(file.name) &&
                   !cooked_sources.contains(file.name) && !import_only.contains(file.name)) {
            inputs.push_back({file.name, file.source, {}});
        }
    }

    const auto output_root = build_output_root(root, profile);
    if (std::filesystem::is_symlink(output_root, ec)) {
        error = "the build folder is a symlink";
        return false;
    }
    for (const auto configuration : build_configurations()) {
        std::filesystem::create_directories(output_root / std::string{configuration}, ec);
        if (ec) {
            error = "cannot create the build folder " + output_root.generic_string();
            return false;
        }
    }
    const auto target = output_root / profile.configuration;
    // An earlier build is ours to replace; anything else in the folder is not.
    bool previous_build = false;
    bool target_empty = true;
    for (std::filesystem::directory_iterator it(target, ec), end; !ec && it != end; it.increment(ec)) {
        target_empty = false;
        previous_build = previous_build || it->path().filename() == "build.json";
    }
    if (ec || (!target_empty && !previous_build)) {
        error = "the build folder " + target.generic_string() +
                " holds files that are not a previous build; empty it or choose another folder";
        return false;
    }

    static std::atomic<std::uint64_t> serial{};
    const auto stage = output_root / (".staging-" + profile.configuration + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(++serial));
    const auto fail = [&](std::string message) {
        std::error_code cleanup;
        std::filesystem::remove_all(stage, cleanup);
        error = std::move(message);
        return false;
    };
    std::filesystem::create_directories(stage, ec);
    if (ec) return fail("cannot create the build folder");
    std::string pak_error;
    if (!write_pak(stage / game_archive_name, inputs, pak_error)) return fail("cannot write the game archive: " + pak_error);
    std::uint64_t bytes = 0;
    for (const auto& input : inputs) {
        std::error_code size_error;
        bytes += input.source.empty() ? input.text.size() : std::filesystem::file_size(input.source, size_error);
    }
    if (!settings.icon.empty() && !copy_regular_file(icon_source, stage / "icon.png"))
        return fail("cannot copy the icon");

    // The scripts, compiled for this build's configuration.
    std::string scripts_entry = "null";
    {
        const auto scripts = compile_scripts_for_export(root, profile.configuration == "debug");
        if (!scripts.succeeded) {
            std::string message = "the scripts did not compile: " + scripts.error;
            for (const auto& diagnostic : scripts.diagnostics)
                if (diagnostic.severity == "error") {
                    message += "\n" + diagnostic.file + ":" + std::to_string(diagnostic.line) + ": " + diagnostic.message;
                    break;
                }
            return fail(message);
        }
        if (!scripts.library.empty()) {
            if (!project_scripts_trusted(root))
                return fail("this project's scripts have not been trusted to build native code; trust the project in the editor first");
            if (!copy_regular_file(scripts.library, stage / "scripts" / "relay-scripts.so"))
                return fail("cannot copy the compiled scripts");
            scripts_entry = "\"scripts/relay-scripts.so\"";
        }
    }

    // The player, renamed for the game, with the shaders and libraries it loads from beside itself.
    const auto name = effective_game_name(settings, project.name);
    const auto stem = game_file_stem(name);
    if (!copy_regular_file(player, stage / stem)) return fail("cannot copy the game player");
    std::filesystem::permissions(stage / stem,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    for (const auto& shader : shader_files)
        if (!copy_regular_file(shader, stage / "shaders" / shader.filename())) return fail("cannot copy a shader");
    for (const auto& [soname, path] : libraries) {
        std::error_code resolve;
        const auto real = std::filesystem::canonical(path, resolve);
        if (resolve || !copy_regular_file(real, stage / "lib" / soname)) return fail("cannot copy the library " + soname);
    }

    const std::string manifest =
        "{\n  \"format\": \"relay.build\",\n  \"version\": 1,\n  \"game\": {\"name\": \"" +
        json_escape(name) + "\", \"file_name\": \"" + json_escape(game_file_stem(name)) +
        "\", \"version\": \"" + json_escape(settings.version) + "\", \"description\": \"" +
        json_escape(settings.description) + "\", \"author\": \"" +
        json_escape(settings.author) + "\", \"release_date\": \"" +
        json_escape(settings.release_date) + "\", \"icon\": " +
        (settings.icon.empty() ? "null" : "\"icon.png\"") + "},\n  \"profile\": \"" +
        json_escape(profile.name) + "\",\n  \"window\": {\"mode\": \"" +
        json_escape(profile.window_mode) + "\", \"width\": " + std::to_string(profile.window_width) +
        ", \"height\": " + std::to_string(profile.window_height) +
        "},\n  \"target\": {\"platform\": \"" + json_escape(profile.target_platform) +
        "\", \"architecture\": \"" + json_escape(profile.architecture) + "\", \"configuration\": \"" +
        json_escape(profile.configuration) + "\"},\n  \"content\": {\"archive\": \"" +
        std::string{game_archive_name} + "\", \"root\": \"data\", "
        "\"project\": \"project.relayproject\", \"unused_files_left_out\": " + std::to_string(dropped) +
        ", \"cooked_models\": " + std::to_string(cooked_models) +
        ", \"cooked_shaders\": " + std::to_string(cooked_shaders) +
        ", \"startup_scene\": \"" +
        json_escape(project.startup_scene) + "\", \"files\": " + std::to_string(inputs.size()) +
        ", \"bytes\": " + std::to_string(bytes) + "},\n  \"scripts\": " + scripts_entry +
        ",\n  \"executable\": \"" + json_escape(stem) + "\"\n}\n";
    if (!write_text_file(stage / "build.json", manifest)) return fail("cannot write build.json");

    std::filesystem::remove_all(target, ec);
    if (ec) return fail("cannot replace the previous build");
    std::filesystem::rename(stage, target, ec);
    if (ec) return fail("cannot finish the build folder");
    report.directory = target;
    report.files = files.size();
    report.bytes = bytes;
    report.executable = true;
    report.executable_path = target / stem;
    error.clear();
    return true;
}

} // namespace relay
