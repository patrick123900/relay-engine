#include "relay/scene/project.hpp"

#include "relay/audio/audio_clip.hpp"
#include "relay/audio/flac_encoder.hpp"
#include "relay/core/elf_requirements.hpp"
#include "relay/core/json.hpp"
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

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#ifndef RELAY_PLAYER_DEFAULT_PATH
#define RELAY_PLAYER_DEFAULT_PATH ""
#endif
#ifndef RELAY_PLAYERS_DEFAULT_DIR
#define RELAY_PLAYERS_DEFAULT_DIR ""
#endif

namespace relay {
namespace {

constexpr std::string_view game_archive_name = "game.pak";
constexpr std::size_t maximum_reported_left_out = 500;

// What a game is made of before anything is cooked or copied.
struct BuildPlan {
    std::vector<ProjectFile> files;
    std::string metadata;
    std::set<std::string> kept, import_only;
    std::vector<std::string> left_out, warnings;
};

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

bool has_wav_extension(const std::string& name) {
    if (name.size() < 4U) return false;
    std::string tail = name.substr(name.size() - 4U);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ".wav";
}

// A WAV file as lossless FLAC, kept only when it is smaller and decodes to exactly the same
// samples. The game reads it under the same name: the audio decoders go by what is inside.
std::optional<std::string> cook_wav(const std::filesystem::path& source) {
    std::ifstream stream(source, std::ios::binary);
    const std::vector<std::uint8_t> wav{std::istreambuf_iterator<char>(stream), {}};
    std::string error;
    auto flac = encode_wav_as_flac(wav, error);
    if (!flac || flac->size() + flac->size() / 20U >= wav.size()) return std::nullopt;
    std::string decode_error;
    const auto before = decode_audio(wav, decode_error);
    const std::vector<std::uint8_t> packed(flac->begin(), flac->end());
    const auto after = decode_audio(packed, decode_error);
    if (!before || !after || before->samples != after->samples || before->sample_rate != after->sample_rate ||
        before->channels != after->channels)
        return std::nullopt;
    return flac;
}

const char* host_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// Linux and Windows builds of Relay produce a player of their own; other hosts rely on installed ones.
bool builtin_player_platform() {
    const std::string platform{host_platform()};
    return platform == "linux" || platform == "windows";
}

const char* host_architecture() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#else
    return "x86_64";
#endif
}

// The program of a game and the files that go beside it: the shaders it loads and the libraries it
// needs. `files` maps a path inside the game's folder to the file to copy there.
struct PlayerKit {
    std::filesystem::path program;
    std::string program_extension; // ".exe" for Windows.
    std::vector<std::pair<std::string, std::filesystem::path>> files;
};

// Players are modular. Each lives in a folder of its own below `players/` beside Relay (or in
// RELAY_PLAYERS_DIR), described by a player.json: which platform and architecture its games are for, and
// which file is the program. A Windows player is relay_player.exe with its shaders/ and libraries; a
// community player for any other target (an Android one, say) is installed the same way. A folder without
// a player.json is named <platform>-<architecture> and holds relay_player or relay_player.exe.
struct PlayerEntry {
    std::string platform, architecture, name, description, version;
    std::filesystem::path folder;
    std::string program; // The program's file name inside the folder.
};

std::filesystem::path players_directory() {
    if (const char* chosen = std::getenv("RELAY_PLAYERS_DIR"); chosen && *chosen) return chosen;
    std::error_code ec;
#ifndef _WIN32
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && std::filesystem::is_directory(self.parent_path() / "players", ec)) return self.parent_path() / "players";
#endif
    return RELAY_PLAYERS_DEFAULT_DIR;
}

std::string player_target_name(const BuildProfile& profile) { return profile.target_platform + "-" + profile.architecture; }

// A platform or architecture is a short lower-case word.
bool valid_target_word(const std::string& word) {
    if (word.empty() || word.size() > 24U || word.front() < 'a' || word.front() > 'z') return false;
    return std::all_of(word.begin(), word.end(),
                       [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool read_player_manifest(const std::filesystem::path& folder, PlayerEntry& entry) {
    std::ifstream stream(folder / "player.json", std::ios::binary);
    if (!stream) return false;
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    JsonParser parser(text);
    const auto value = parser.parse();
    const auto* object = value ? value->object() : nullptr;
    if (!object) return false;
    const auto text_of = [&](const char* name) {
        const auto* entry_value = field(*object, name);
        return entry_value && entry_value->string() ? *entry_value->string() : std::string{};
    };
    if (text_of("format") != "relay.player") return false;
    entry.platform = text_of("platform");
    entry.architecture = text_of("architecture");
    entry.name = text_of("name");
    entry.description = text_of("description");
    entry.version = text_of("version");
    entry.program = text_of("program");
    return true;
}

std::vector<PlayerEntry> installed_players() {
    std::vector<PlayerEntry> players;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(players_directory(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec) || it->is_symlink(ec)) continue;
        const auto folder_name = it->path().filename().string();
        if (folder_name.ends_with(".installing")) continue;
        PlayerEntry entry;
        entry.folder = it->path();
        if (!read_player_manifest(it->path(), entry)) {
            const auto dash = folder_name.find('-');
            if (dash == std::string::npos) continue;
            entry.platform = folder_name.substr(0, dash);
            entry.architecture = folder_name.substr(dash + 1U);
            entry.program.clear();
        }
        if (entry.program.empty())
            for (const char* name : {"relay_player", "relay_player.exe"})
                if (std::filesystem::is_regular_file(it->path() / name, ec)) entry.program = name;
        if (!valid_target_word(entry.platform) || !valid_target_word(entry.architecture) || entry.program.empty() ||
            entry.program.find('/') != std::string::npos || entry.program.find('\\') != std::string::npos ||
            !std::filesystem::is_regular_file(it->path() / entry.program, ec))
            continue;
        players.push_back(std::move(entry));
    }
    std::sort(players.begin(), players.end(), [](const PlayerEntry& a, const PlayerEntry& b) {
        return std::tie(a.platform, a.architecture) < std::tie(b.platform, b.architecture);
    });
    return players;
}

const PlayerEntry* find_installed_player(const std::vector<PlayerEntry>& players, const std::string& platform,
                                         const std::string& architecture) {
    for (const auto& entry : players)
        if (entry.platform == platform && entry.architecture == architecture) return &entry;
    return nullptr;
}

bool player_installed(const BuildProfile& profile) {
    return find_installed_player(installed_players(), profile.target_platform, profile.architecture) != nullptr;
}

// The program a built game runs: RELAY_PLAYER_BINARY, else relay_player beside the running
// program, else where this build of Relay put it.
std::filesystem::path find_player_binary() {
    std::error_code ec;
    if (const char* chosen = std::getenv("RELAY_PLAYER_BINARY"); chosen && *chosen) return chosen;
#if defined(_WIN32)
    std::wstring buffer(32768U, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0U && length < buffer.size()) {
        const auto beside = std::filesystem::path(buffer.substr(0U, length)).parent_path() / "relay_player.exe";
        if (std::filesystem::is_regular_file(beside, ec)) return beside;
    }
#else
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
[[maybe_unused]] bool system_library(const std::string& name) {
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
#if defined(_WIN32)
    // The Windows player links its libraries statically; the system supplies the rest.
    (void)program;
    (void)libraries;
    (void)error;
    return true;
#else
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
#endif
}

// Finds the player for the profile's target and the files it needs. This machine's own target uses
// the relay_player it was built with; any other (and any target on a machine that is not Linux) uses
// an installed player folder, whose files are copied as they are.
bool find_player_kit(const BuildProfile& profile, PlayerKit& kit, std::string& error) {
    std::error_code ec;
    const bool own = profile.target_platform == host_platform() && profile.architecture == host_architecture();
    // The player this Relay was built with serves its own Linux target, unless one was installed for it.
    if (own && builtin_player_platform() && !player_installed(profile)) {
        const auto player = find_player_binary();
        if (player.empty() || !std::filesystem::is_regular_file(player, ec)) {
            error = "the game player was not found" + (player.empty() ? std::string{} : " at " + player.generic_string()) +
                    "; build the relay_player target or set RELAY_PLAYER_BINARY";
            return false;
        }
        kit.program = player;
        kit.program_extension = player.extension().string();
        const auto shader_source = player.parent_path() / "shaders";
        std::size_t shaders = 0;
        for (std::filesystem::directory_iterator it(shader_source, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file() && it->path().extension() == ".spv") {
                kit.files.emplace_back("shaders/" + it->path().filename().string(), it->path());
                ++shaders;
            }
        if (shaders == 0U) {
            error = "the player's compiled shaders were not found in " + shader_source.generic_string();
            return false;
        }
        std::vector<std::pair<std::string, std::filesystem::path>> libraries;
        if (!program_libraries(player, libraries, error)) return false;
        for (const auto& [soname, path] : libraries) {
            std::error_code resolve;
            kit.files.emplace_back("lib/" + soname, std::filesystem::canonical(path, resolve));
        }
        return true;
    }
    const auto players = installed_players();
    const auto* entry = find_installed_player(players, profile.target_platform, profile.architecture);
    if (entry == nullptr) {
        error = "there is no " + std::string{profile.target_platform} + " " + profile.architecture + " player installed. " +
                "Add one under Run > Manage players, or put its folder in " + players_directory().generic_string();
        return false;
    }
    const auto folder = entry->folder;
    kit.program = folder / entry->program;
    kit.program_extension = std::filesystem::path{entry->program}.extension().string();
    std::size_t shaders = 0;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path() == kit.program) continue;
        const auto relative = it->path().lexically_relative(folder).generic_string();
        if (kit.files.size() >= 4096U) {
            error = "the player folder " + folder.generic_string() + " holds too many files";
            return false;
        }
        if (relative.rfind("shaders/", 0) == 0 && it->path().extension() == ".spv") ++shaders;
        kit.files.emplace_back(relative, it->path());
    }
    if (shaders == 0U) {
        error = "the " + player_target_name(profile) + " player has no compiled shaders in " + (folder / "shaders").generic_string();
        return false;
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

static bool plan_game_files(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                     BuildPlan& plan, std::string& error) {
    plan = {};
    const auto root = project.root();
    // The build folder is output, so neither it nor the default builds folder belongs in the game.
    // The game ships its scripts compiled, so their sources stay behind.
    std::vector<std::string> excluded{"builds", std::string{script_source_directory}};
    for (const auto& other : settings.profiles)
        if (const auto inside = output_inside_project(root, other); !inside.empty())
            excluded.push_back(inside);
    if (!collect_project_files(project, excluded, plan.files, plan.metadata, error)) return false;
    // Only what the game refers to ships, unless the profile says everything should.
    if (!profile.include_unused_assets) {
        AssetReferences references;
        if (!find_referenced_assets(project, plan.files, plan.metadata, profile.always_include, references, error)) return false;
        plan.kept = std::move(references.kept);
        plan.import_only = std::move(references.import_only);
        plan.warnings = std::move(references.warnings);
        plan.left_out = std::move(references.dropped);
        std::sort(plan.left_out.begin(), plan.left_out.end());
    } else {
        for (const auto& file : plan.files)
            if (!file.source.empty()) plan.kept.insert(file.name);
    }
    return true;
}

std::filesystem::path players_folder() { return players_directory(); }

std::vector<PlayerInfo> list_players() {
    std::vector<PlayerInfo> players;
    std::error_code ec;
    const auto measure = [&](PlayerInfo& info, const std::vector<std::filesystem::path>& files) {
        ElfRequirements newest;
        for (const auto& file : files) {
            ElfRequirements found;
            std::string read_error;
            if (!read_elf_requirements(file, found, read_error)) continue;
            if (!found.glibc.empty() && compare_dotted_versions(found.glibc, newest.glibc) > 0) info.glibc_from = file.filename().string();
            merge_requirements(newest, found);
        }
        info.glibc = newest.glibc;
    };
    const auto installed = installed_players();
    for (const auto& entry : installed) {
        PlayerInfo info;
        info.platform = entry.platform;
        info.architecture = entry.architecture;
        info.name = entry.name;
        info.description = entry.description;
        info.version = entry.version;
        info.native = entry.platform == host_platform() && entry.architecture == host_architecture();
        info.installed = true;
        info.folder = entry.folder;
        info.program = entry.folder / entry.program;
        std::vector<std::filesystem::path> measured;
        for (std::filesystem::recursive_directory_iterator it(entry.folder, ec), end; !ec && it != end && info.files < 100000U;
             it.increment(ec))
            if (it->is_regular_file(ec)) {
                ++info.files;
                info.bytes += it->file_size(ec);
                if (entry.platform == "linux" &&
                    (it->path() == info.program || it->path().filename().string().find(".so") != std::string::npos))
                    measured.push_back(it->path());
            }
        measure(info, measured);
        players.push_back(std::move(info));
    }
    // This machine's own Linux player, which the editor was built with, serves its target unless one is installed.
    if (builtin_player_platform() && !find_installed_player(installed, host_platform(), host_architecture())) {
        const auto player = find_player_binary();
        if (!player.empty() && std::filesystem::is_regular_file(player, ec)) {
            PlayerInfo info;
            info.platform = host_platform();
            info.architecture = host_architecture();
            info.native = true;
            info.built_in = true;
            info.folder = players_directory() / player_target_name([&] {
                BuildProfile profile;
                profile.target_platform = info.platform;
                profile.architecture = info.architecture;
                return profile;
            }());
            info.program = player;
            info.files = 1U;
            info.bytes = std::filesystem::file_size(player, ec);
            std::vector<std::filesystem::path> measured{player};
            std::vector<std::pair<std::string, std::filesystem::path>> libraries;
            std::string ignored;
            if (program_libraries(player, libraries, ignored))
                for (const auto& library : libraries) measured.push_back(library.second);
            measure(info, measured);
            players.insert(players.begin(), std::move(info));
        }
    }
    return players;
}

std::vector<BuildTarget> available_build_targets() {
    std::vector<BuildTarget> targets;
    for (const auto& player : list_players()) targets.push_back({player.platform, player.architecture, true, player.native});
    return targets;
}

bool install_player(const std::filesystem::path& source, const std::string& platform_hint, const std::string& architecture_hint,
                    std::string& platform, std::string& architecture, std::string& error) {
    std::error_code ec;
    if (!source.is_absolute() || !std::filesystem::is_directory(source, ec)) {
        error = "choose the folder that holds the player, with its program and shaders";
        return false;
    }
    // player.json says what the player is for; a folder without one is told by the person.
    PlayerEntry described;
    const bool has_manifest = read_player_manifest(source, described);
    platform = has_manifest && !described.platform.empty() ? described.platform : platform_hint;
    architecture = has_manifest && !described.architecture.empty() ? described.architecture : architecture_hint;
    if (platform.empty() || architecture.empty()) {
        error = "this folder has no player.json saying which platform and architecture the player is for; say which";
        return false;
    }
    if (!valid_target_word(platform) || !valid_target_word(architecture)) {
        error = "a platform and an architecture are short lower-case words, such as windows and x86_64";
        return false;
    }
    std::string program = described.program;
    if (program.empty())
        for (const char* name : {"relay_player", "relay_player.exe"})
            if (std::filesystem::is_regular_file(source / name, ec)) program = name;
    if (program.empty() || program.find('/') != std::string::npos || program.find('\\') != std::string::npos ||
        !std::filesystem::is_regular_file(source / program, ec)) {
        error = source.generic_string() + " has no player program (relay_player, or the program named in player.json)";
        return false;
    }
    std::size_t shaders = 0;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_symlink(ec) || !it->is_regular_file(ec)) continue;
        if (files.size() >= 4096U) {
            error = "the player folder holds more than 4096 files";
            return false;
        }
        const auto relative = it->path().lexically_relative(source).generic_string();
        if (relative.rfind("shaders/", 0) == 0 && it->path().extension() == ".spv") ++shaders;
        files.push_back(it->path());
    }
    if (shaders == 0U) {
        error = "the player has no compiled shaders: " + (source / "shaders").generic_string() + " holds no .spv files";
        return false;
    }
    const auto root = players_directory();
    const auto name = platform + "-" + architecture;
    const auto target = root / name;
    const auto staging = root / (name + ".installing");
    std::filesystem::remove_all(staging, ec);
    std::filesystem::create_directories(staging, ec);
    if (ec) {
        error = "cannot create " + staging.generic_string() + ": " + ec.message();
        return false;
    }
    for (const auto& file : files)
        if (!copy_regular_file(file, staging / file.lexically_relative(source))) {
            std::filesystem::remove_all(staging, ec);
            error = "cannot copy " + file.generic_string();
            return false;
        }
    if (!has_manifest) {
        const std::string manifest = "{\"format\": \"relay.player\", \"version\": 1, \"platform\": \"" + platform +
                                     "\", \"architecture\": \"" + architecture + "\", \"program\": \"" + json_escape(program) + "\"}\n";
        if (!write_text_file(staging / "player.json", manifest)) {
            std::filesystem::remove_all(staging, ec);
            error = "cannot write the player's description";
            return false;
        }
    }
    std::filesystem::permissions(staging / program,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    // A player already installed for the target, whatever its folder is called, is replaced.
    for (const auto& entry : installed_players())
        if (entry.platform == platform && entry.architecture == architecture) std::filesystem::remove_all(entry.folder, ec);
    std::filesystem::remove_all(target, ec);
    std::filesystem::rename(staging, target, ec);
    if (ec) {
        error = "cannot install the player in " + target.generic_string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool remove_player(const std::string& platform, const std::string& architecture, std::string& error) {
    std::error_code ec;
    const auto installed = installed_players();
    const auto* entry = find_installed_player(installed, platform, architecture);
    if (entry == nullptr) {
        error = "no player is installed for " + platform + " " + architecture;
        return false;
    }
    std::filesystem::remove_all(entry->folder, ec);
    if (ec) {
        error = "cannot remove " + entry->folder.generic_string() + ": " + ec.message();
        return false;
    }
    return true;
}

bool preview_build(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                   BuildPreview& preview, std::string& error) {
    preview = {};
    if (!validate_build_settings(settings, error) || !validate_build_profile(profile, error)) return false;
    BuildPlan plan;
    if (!plan_game_files(project, settings, profile, plan, error)) return false;
    preview.packed = plan.kept.size();
    preview.left_out = std::move(plan.left_out);
    preview.warnings = std::move(plan.warnings);
    return true;
}

bool build_project(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                   std::string& error, BuildReport& report, BuildControl* control) {
    report = {};
    const auto step = [&](const std::string_view stage, const double fraction, const std::string_view detail = {}) {
        if (control && control->progress) control->progress(stage, fraction, detail);
    };
    // True (with the report and error set) when the build was told to stop.
    const auto stopped = [&] {
        if (!control || !control->cancelled()) return false;
        report.cancelled = true;
        error = "the build was cancelled";
        return true;
    };
    step("Checking the project", 0.0);
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

    // The game's program, from this installation of Relay (or its players folder, for other targets).
    PlayerKit kit;
    if (!find_player_kit(profile, kit, error)) return false;
    const bool native_target = profile.target_platform == host_platform() && profile.architecture == host_architecture();
    if (!native_target && project_has_script_sources(root)) {
        error = "this project has scripts, which are native code built with this machine's compiler, so a " +
                player_target_name(profile) + " game cannot be built here yet; build it on that platform";
        return false;
    }
    if (project_has_script_sources(root) && !project_scripts_trusted(root)) {
        error = "this project's scripts have not been trusted to build native code; trust the project in the editor first";
        return false;
    }
    if (stopped()) return false;

    step("Collecting files", 0.03);
    BuildPlan plan;
    if (!plan_game_files(project, settings, profile, plan, error)) return false;
    auto& files = plan.files;
    auto& metadata = plan.metadata;
    auto& kept = plan.kept;
    auto& import_only = plan.import_only;
    const std::size_t dropped = plan.left_out.size();
    step("Finding what the game uses", 0.07, std::to_string(files.size()) + " files");
    if (stopped()) return false;

    // Models and shaders ship cooked: imported and compiled now, so the game neither carries the
    // importer and the shader compiler nor spends its start-up running them.
    std::vector<PakInput> inputs;
    std::set<std::string> cooked_sources;
    CookStats cook_stats;
    std::uint32_t cooked_models = 0;
    std::uint32_t cooked_shaders = 0;
    {
        ImportManifest imports;
        if (!imports.load(root, error)) return false;
        std::set<std::string> models;
        for (const auto& entry : imports.entries())
            if (kept.contains(entry.source)) models.insert(entry.source);
        if (!models.empty()) {
            step("Cooking models", 0.12, std::to_string(models.size()) + (models.size() == 1U ? " model" : " models"));
            std::string bytes;
            if (!cook_models(root, models, bytes, error, profile.compress_textures, &cook_stats)) {
                if (stopped()) return false;
                error = "cannot cook the models: " + error;
                return false;
            }
            inputs.push_back({std::string{cooked_models_name}, {}, std::move(bytes)});
            cooked_models = static_cast<std::uint32_t>(models.size());
            cooked_sources.insert(models.begin(), models.end());
        }
        if (stopped()) return false;
        std::size_t shaders_done = 0;
        std::size_t shaders_total = 0;
        for (const auto& name : kept)
            if (name.ends_with(".relay-shader")) ++shaders_total;
        for (const auto& name : kept)
            if (name.ends_with(".relay-shader")) {
                if (stopped()) return false;
                step("Compiling shaders", 0.25 + 0.10 * static_cast<double>(shaders_done) / static_cast<double>(std::max<std::size_t>(shaders_total, 1U)), name);
                ++shaders_done;
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
    std::uint32_t sounds_compressed = 0;
    std::uint64_t sound_bytes_before = 0, sound_bytes_after = 0;
    for (const auto& file : files) {
        if (file.source.empty()) {
            inputs.push_back({file.name, {}, file.text});
        } else if (file.name != ".relay-imports.json" && kept.contains(file.name) &&
                   !cooked_sources.contains(file.name) && !import_only.contains(file.name)) {
            if (stopped()) return false;
            if (profile.compress_audio && has_wav_extension(file.name)) {
                step("Compressing sounds", 0.30, file.name);
                if (auto flac = cook_wav(file.source)) {
                    ++sounds_compressed;
                    sound_bytes_before += file.size;
                    sound_bytes_after += flac->size();
                    inputs.push_back({file.name, {}, std::move(*flac)});
                    continue;
                }
            }
            inputs.push_back({file.name, file.source, {}});
        }
    }

    if (stopped()) return false;
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
        if (control && control->cancelled()) report.cancelled = true;
        return false;
    };
    std::filesystem::create_directories(stage, ec);
    if (ec) return fail("cannot create the build folder");
    step("Writing the game's files", 0.36, std::to_string(inputs.size()) + " files");
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
    if (stopped()) return fail("the build was cancelled");
    {
        step("Compiling scripts", 0.42);
        const auto scripts = compile_scripts_for_export(
            root, profile.configuration == "debug", control ? control->cancel : nullptr,
            [&](const std::uint32_t done, const std::uint32_t total) {
                step("Compiling scripts", 0.42 + 0.43 * static_cast<double>(done) / static_cast<double>(std::max<std::uint32_t>(total, 1U)),
                     std::to_string(done) + " of " + std::to_string(total) + " files");
            });
        if (scripts.cancelled) return fail("the build was cancelled");
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
    step("Copying the player and its libraries", 0.88);
    const auto name = effective_game_name(settings, project.name);
    const auto stem = game_file_stem(name);
    const auto program_name = stem + kit.program_extension;
    if (!copy_regular_file(kit.program, stage / program_name)) return fail("cannot copy the game player");
    std::filesystem::permissions(stage / program_name,
                                 std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                                     std::filesystem::perms::others_exec,
                                 std::filesystem::perm_options::add, ec);
    for (const auto& [destination, source] : kit.files) {
        if (stopped()) return fail("the build was cancelled");
        if (!copy_regular_file(source, stage / destination)) return fail("cannot copy " + destination);
    }

    // What the finished game asks of the system it runs on.
    {
        ElfRequirements newest;
        std::string from;
        const auto consider = [&](const std::filesystem::path& file) {
            ElfRequirements found;
            std::string read_error;
            if (!read_elf_requirements(file, found, read_error)) return;
            if (!found.glibc.empty() && compare_dotted_versions(found.glibc, newest.glibc) > 0) from = file.filename().string();
            merge_requirements(newest, found);
        };
        consider(stage / program_name);
        for (const auto& [destination, source] : kit.files)
            if (destination.rfind("lib/", 0) == 0) consider(stage / destination);
        if (scripts_entry != "null") consider(stage / "scripts" / "relay-scripts.so");
        report.glibc = newest.glibc;
        report.glibc_from = from;
        report.libstdcxx = newest.libstdcxx;
        report.cxxabi = newest.cxxabi;
        if (!profile.minimum_glibc.empty() && compare_dotted_versions(newest.glibc, profile.minimum_glibc) > 0)
            return fail("the game needs glibc " + newest.glibc + " (" + from + "), newer than the minimum " +
                        profile.minimum_glibc + " this profile asks for. Build Relay in an environment with an older glibc "
                        "(see docs/portable-builds.md), or lower the minimum.");
    }
    std::string left_out_json = "[";
    for (std::size_t i = 0; i < plan.left_out.size() && i < maximum_reported_left_out; ++i)
        left_out_json += (i ? "," : "") + std::string{"\""} + json_escape(plan.left_out[i]) + '"';
    left_out_json += ']';
    std::string warnings_json = "[";
    for (std::size_t i = 0; i < plan.warnings.size(); ++i)
        warnings_json += (i ? "," : "") + std::string{"\""} + json_escape(plan.warnings[i]) + '"';
    warnings_json += ']';
    report.left_out_count = plan.left_out.size();
    report.left_out.assign(plan.left_out.begin(),
                           plan.left_out.begin() + static_cast<std::ptrdiff_t>(std::min(plan.left_out.size(), maximum_reported_left_out)));
    report.warnings = plan.warnings;
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
        ", \"left_out\": " + left_out_json + ", \"warnings\": " + warnings_json +
        ", \"requirements\": {\"glibc\": \"" + json_escape(report.glibc) + "\", \"glibc_from\": \"" +
        json_escape(report.glibc_from) + "\", \"libstdcxx\": \"" + json_escape(report.libstdcxx) +
        "\", \"cxxabi\": \"" + json_escape(report.cxxabi) + "\"}" +
        ", \"cooked_models\": " + std::to_string(cooked_models) +
        ", \"textures_compressed\": " + std::to_string(cook_stats.textures_compressed) +
        ", \"texture_bytes_before\": " + std::to_string(cook_stats.texture_bytes_before) +
        ", \"texture_bytes_after\": " + std::to_string(cook_stats.texture_bytes_after) +
        ", \"sounds_compressed\": " + std::to_string(sounds_compressed) +
        ", \"sound_bytes_before\": " + std::to_string(sound_bytes_before) +
        ", \"sound_bytes_after\": " + std::to_string(sound_bytes_after) +
        ", \"cooked_shaders\": " + std::to_string(cooked_shaders) +
        ", \"startup_scene\": \"" +
        json_escape(project.startup_scene) + "\", \"files\": " + std::to_string(inputs.size()) +
        ", \"bytes\": " + std::to_string(bytes) + "},\n  \"scripts\": " + scripts_entry +
        ",\n  \"executable\": \"" + json_escape(program_name) + "\"\n}\n";
    if (!write_text_file(stage / "build.json", manifest)) return fail("cannot write build.json");
    if (stopped()) return fail("the build was cancelled");
    step("Finishing", 0.97);

    std::filesystem::remove_all(target, ec);
    if (ec) return fail("cannot replace the previous build");
    std::filesystem::rename(stage, target, ec);
    if (ec) return fail("cannot finish the build folder");
    report.directory = target;
    report.files = files.size();
    report.bytes = bytes;
    report.executable = true;
    report.executable_path = target / program_name;
    error.clear();
    return true;
}

} // namespace relay
