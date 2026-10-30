#include "relay/scene/build_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

namespace relay {
namespace {

bool has_control_character(const std::string_view text, const bool allow_newlines) {
    return std::any_of(text.begin(), text.end(), [&](const char c) {
        const auto value = static_cast<unsigned char>(c);
        return value == 0x7F || (value < 0x20 && !(allow_newlines && (c == '\n' || c == '\t')));
    });
}

bool listed(const std::vector<std::string_view>& values, const std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

// A relative path inside the project: no traversal, hidden parts, links or odd characters.
bool safe_relative_path(const std::string_view text) {
    if (text.empty() || text.size() > 128) return false;
    const std::filesystem::path path{text};
    if (path.is_absolute()) return false;
    for (const auto& part : path) {
        const auto name = part.string();
        if (name.empty() || name.front() == '.') return false;
        for (const char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_' || c == '.' || c == ' '))
                return false;
    }
    return true;
}

bool valid_date(const std::string_view text) {
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9')) return false;
    const int year = std::stoi(std::string{text.substr(0, 4)});
    const int month = std::stoi(std::string{text.substr(5, 2)});
    const int day = std::stoi(std::string{text.substr(8, 2)});
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

// 1, 1.2 or 1.2.3.4, then optionally -beta.1 or +build5.
bool valid_version(const std::string_view text) {
    if (text.empty() || text.size() > maximum_game_version_length) return false;
    std::size_t at = 0;
    std::size_t groups = 0;
    while (true) {
        const auto start = at;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') ++at;
        if (at == start || at - start > 9) return false;
        ++groups;
        if (at < text.size() && text[at] == '.' && groups < 4) { ++at; continue; }
        break;
    }
    if (at == text.size()) return true;
    if (text[at] != '-' && text[at] != '+') return false;
    if (++at == text.size()) return false;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return false;
    }
    return true;
}

// An absolute folder path elsewhere on disk: no traversal, control characters or relative parts.
bool plain_absolute_path(const std::string_view text) {
    if (text.empty() || text.size() > maximum_output_directory_length || has_control_character(text, false))
        return false;
    const std::filesystem::path path{text};
    if (!path.is_absolute()) return false;
    for (const auto& part : path)
        if (part == ".." || part == ".") return false;
    return true;
}

bool valid_profile_name(const std::string_view text) {
    if (text.empty() || text.size() > maximum_profile_name_length || has_control_character(text, false))
        return false;
    return text.front() != ' ' && text.back() != ' ';
}

} // namespace

const BuildProfile* BuildSettings::find_profile(const std::string_view name) const {
    for (const auto& profile : profiles)
        if (profile.name == name) return &profile;
    return nullptr;
}

BuildProfile* BuildSettings::find_profile(const std::string_view name) {
    for (auto& profile : profiles)
        if (profile.name == name) return &profile;
    return nullptr;
}

const BuildProfile& BuildSettings::active() const {
    const auto* profile = find_profile(active_profile);
    return profile ? *profile : profiles.front();
}

const std::vector<std::string_view>& build_window_modes() {
    static const std::vector<std::string_view> values{"windowed", "borderless", "fullscreen"};
    return values;
}

bool valid_build_target_word(const std::string& word) {
    if (word.empty() || word.size() > 24U || word.front() < 'a' || word.front() > 'z') return false;
    return std::all_of(word.begin(), word.end(),
                       [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

const std::vector<std::string_view>& build_configurations() {
    static const std::vector<std::string_view> values{"release", "debug"};
    return values;
}

bool validate_build_profile(const BuildProfile& profile, std::string& error) {
    if (!valid_profile_name(profile.name)) {
        error = "a build profile name must be 1 to " + std::to_string(maximum_profile_name_length) +
                " characters without leading or trailing spaces";
        return false;
    }
    // Which targets can be built is up to the players installed; a profile may name any target.
    if (!valid_build_target_word(profile.target_platform)) {
        error = "a target platform is a short lower-case word, such as linux or windows";
        return false;
    }
    if (!valid_build_target_word(profile.architecture)) {
        error = "an architecture is a short lower-case word, such as x86_64 or aarch64";
        return false;
    }
    if (!listed(build_configurations(), profile.configuration)) {
        error = "the configuration must be release or debug";
        return false;
    }
    if (!safe_relative_path(profile.output_directory) && !plain_absolute_path(profile.output_directory)) {
        error = "the build folder must be a plain folder inside the project or an absolute path";
        return false;
    }
    if (!profile.minimum_glibc.empty()) {
        const auto dot = profile.minimum_glibc.find('.');
        const bool shaped = dot != std::string::npos && dot > 0U && dot + 1U < profile.minimum_glibc.size() &&
                            profile.minimum_glibc.size() <= 8U &&
                            std::all_of(profile.minimum_glibc.begin(), profile.minimum_glibc.end(),
                                        [](const char c) { return (c >= '0' && c <= '9') || c == '.'; }) &&
                            profile.minimum_glibc.find('.', dot + 1U) == std::string::npos;
        if (!shaped) {
            error = "the minimum glibc must look like 2.31, or be empty";
            return false;
        }
    }
    if (profile.always_include.size() > maximum_always_include) {
        error = "a profile can always include at most " + std::to_string(maximum_always_include) + " files or folders";
        return false;
    }
    for (std::size_t i = 0; i < profile.always_include.size(); ++i) {
        const auto& entry = profile.always_include[i];
        if (!safe_relative_path(entry)) {
            error = "always_include entries must be plain project-relative files or folders: " + entry;
            return false;
        }
        for (std::size_t j = 0; j < i; ++j)
            if (profile.always_include[j] == entry) {
                error = "always_include lists " + entry + " twice";
                return false;
            }
    }
    if (!listed(build_window_modes(), profile.window_mode)) {
        error = "the window mode must be windowed, borderless or fullscreen";
        return false;
    }
    if (profile.window_width < minimum_window_size || profile.window_width > maximum_window_size ||
        profile.window_height < minimum_window_size || profile.window_height > maximum_window_size) {
        error = "the window size must be from " + std::to_string(minimum_window_size) + " to " +
                std::to_string(maximum_window_size) + " pixels in each direction";
        return false;
    }
    error.clear();
    return true;
}

bool validate_build_settings(const BuildSettings& settings, std::string& error) {
    if (settings.game_name.size() > maximum_game_name_length ||
        has_control_character(settings.game_name, false)) {
        error = "the game name must be at most " + std::to_string(maximum_game_name_length) +
                " characters without control characters";
        return false;
    }
    if (settings.description.size() > maximum_game_description_length ||
        has_control_character(settings.description, true)) {
        error = "the description must be at most " +
                std::to_string(maximum_game_description_length) + " characters";
        return false;
    }
    if (settings.author.size() > maximum_game_author_length ||
        has_control_character(settings.author, false)) {
        error = "the author must be at most " + std::to_string(maximum_game_author_length) +
                " characters without control characters";
        return false;
    }
    if (!settings.release_date.empty() && !valid_date(settings.release_date)) {
        error = "the release date must be a real date written YYYY-MM-DD";
        return false;
    }
    if (!settings.icon.empty()) {
        std::string lowered = settings.icon;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!safe_relative_path(settings.icon) || !lowered.ends_with(".png")) {
            error = "the icon must be a PNG inside the project";
            return false;
        }
    }
    if (!valid_version(settings.version)) {
        error = "the version must be dotted numbers such as 1.0.0, optionally followed by -beta.1";
        return false;
    }
    if (settings.profiles.empty() || settings.profiles.size() > maximum_build_profiles) {
        error = "a project needs from 1 to " + std::to_string(maximum_build_profiles) + " build profiles";
        return false;
    }
    for (std::size_t i = 0; i < settings.profiles.size(); ++i) {
        if (!validate_build_profile(settings.profiles[i], error)) return false;
        for (std::size_t j = 0; j < i; ++j)
            if (settings.profiles[j].name == settings.profiles[i].name) {
                error = "there are two build profiles named " + settings.profiles[i].name;
                return false;
            }
    }
    if (settings.find_profile(settings.active_profile) == nullptr) {
        error = "the active build profile " + settings.active_profile + " does not exist";
        return false;
    }
    error.clear();
    return true;
}

std::string build_profile_json(const BuildProfile& profile) {
    std::string always = "[";
    for (const auto& entry : profile.always_include) {
        if (always.size() > 1) always += ',';
        always += '"' + json_escape(entry) + '"';
    }
    always += ']';
    return "{\"name\":\"" + json_escape(profile.name) + "\",\"target_platform\":\"" +
           json_escape(profile.target_platform) + "\",\"architecture\":\"" +
           json_escape(profile.architecture) + "\",\"configuration\":\"" +
           json_escape(profile.configuration) + "\",\"output_directory\":\"" +
           json_escape(profile.output_directory) + "\",\"window_mode\":\"" +
           json_escape(profile.window_mode) + "\",\"window_width\":" +
           std::to_string(profile.window_width) + ",\"window_height\":" +
           std::to_string(profile.window_height) + ",\"include_unused_assets\":" +
           (profile.include_unused_assets ? "true" : "false") + ",\"compress_textures\":" +
           (profile.compress_textures ? "true" : "false") + ",\"compress_audio\":" +
           (profile.compress_audio ? "true" : "false") + ",\"minimum_glibc\":\"" +
           json_escape(profile.minimum_glibc) + "\",\"always_include\":" + always + '}';
}

std::string build_settings_json(const BuildSettings& settings) {
    std::string profiles = "[";
    for (const auto& profile : settings.profiles) {
        if (profiles.size() > 1) profiles += ',';
        profiles += build_profile_json(profile);
    }
    return "{\"game_name\":\"" + json_escape(settings.game_name) + "\",\"description\":\"" +
           json_escape(settings.description) + "\",\"author\":\"" + json_escape(settings.author) +
           "\",\"release_date\":\"" + json_escape(settings.release_date) + "\",\"icon\":\"" +
           json_escape(settings.icon) + "\",\"version\":\"" + json_escape(settings.version) +
           "\",\"active_profile\":\"" + json_escape(settings.active_profile) +
           "\",\"profiles\":" + profiles + "]}";
}

namespace {

std::optional<BuildProfile> parse_build_profile(const JsonValue::Object& object, BuildProfile profile,
                                                std::string& error) {
    for (const auto& [name, entry] : object) {
        if (name == "window_width" || name == "window_height") {
            const auto* number = entry.number();
            if (number == nullptr || *number < 0.0 || *number > maximum_window_size ||
                std::floor(*number) != *number) {
                error = "build profile setting " + name + " must be a whole number of pixels";
                return std::nullopt;
            }
            (name == "window_width" ? profile.window_width : profile.window_height) =
                static_cast<std::uint32_t>(*number);
            continue;
        }
        if (name == "always_include") {
            const auto* list = entry.array();
            if (list == nullptr) {
                error = "build profile setting always_include must be a list of project paths";
                return std::nullopt;
            }
            profile.always_include.clear();
            for (const auto& item : *list) {
                if (item.string() == nullptr) {
                    error = "build profile setting always_include must be a list of project paths";
                    return std::nullopt;
                }
                profile.always_include.push_back(*item.string());
            }
            continue;
        }
        if (name == "include_unused_assets" || name == "compress_textures" || name == "compress_audio") {
            if (entry.boolean() == nullptr) {
                error = "build profile setting " + name + " must be true or false";
                return std::nullopt;
            }
            (name == "include_unused_assets" ? profile.include_unused_assets
             : name == "compress_textures" ? profile.compress_textures : profile.compress_audio) = *entry.boolean();
            continue;
        }
        std::string* target = name == "name"               ? &profile.name
                              : name == "target_platform"  ? &profile.target_platform
                              : name == "architecture"     ? &profile.architecture
                              : name == "configuration"    ? &profile.configuration
                              : name == "output_directory" ? &profile.output_directory
                              : name == "window_mode"      ? &profile.window_mode
                              : name == "minimum_glibc"    ? &profile.minimum_glibc
                                                           : nullptr;
        if (target == nullptr) {
            error = "unknown build profile setting " + name;
            return std::nullopt;
        }
        if (entry.string() == nullptr) {
            error = "build profile setting " + name + " must be text";
            return std::nullopt;
        }
        *target = *entry.string();
    }
    return profile;
}

} // namespace

std::optional<BuildSettings> parse_build_settings(const JsonValue& value, std::string& error) {
    const auto* object = value.object();
    if (object == nullptr) {
        error = "build settings must be an object";
        return std::nullopt;
    }
    BuildSettings settings;
    bool has_active = false;
    bool has_profiles = false;
    // Projects saved before profiles existed kept the target fields beside the game's.
    JsonValue::Object legacy;
    for (const auto& [name, entry] : *object) {
        if (name == "target_platform" || name == "architecture" || name == "configuration" ||
            name == "output_directory" || name == "window_mode" || name == "window_width" ||
            name == "window_height") {
            legacy.emplace(name, entry);
            continue;
        }
        if (name == "profiles") {
            has_profiles = true;
            const auto* list = entry.array();
            if (list == nullptr) {
                error = "build profiles must be a list";
                return std::nullopt;
            }
            settings.profiles.clear();
            for (const auto& item : *list) {
                const auto* profile_object = item.object();
                if (profile_object == nullptr) {
                    error = "each build profile must be an object";
                    return std::nullopt;
                }
                auto profile = parse_build_profile(*profile_object, BuildProfile{}, error);
                if (!profile) return std::nullopt;
                settings.profiles.push_back(std::move(*profile));
            }
            continue;
        }
        std::string* target = name == "game_name"        ? &settings.game_name
                              : name == "description"    ? &settings.description
                              : name == "author"         ? &settings.author
                              : name == "release_date"   ? &settings.release_date
                              : name == "icon"           ? &settings.icon
                              : name == "version"        ? &settings.version
                              : name == "active_profile" ? &settings.active_profile
                                                         : nullptr;
        if (target == nullptr) {
            error = "unknown build setting " + name;
            return std::nullopt;
        }
        if (entry.string() == nullptr) {
            error = "build setting " + name + " must be text";
            return std::nullopt;
        }
        *target = *entry.string();
        has_active = has_active || name == "active_profile";
    }
    if (!legacy.empty() && !has_profiles) {
        auto profile = parse_build_profile(legacy, BuildProfile{}, error);
        if (!profile) return std::nullopt;
        settings.profiles = {std::move(*profile)};
    }
    if (!has_active && !settings.profiles.empty()) settings.active_profile = settings.profiles.front().name;
    if (!validate_build_settings(settings, error)) return std::nullopt;
    return settings;
}

std::string effective_game_name(const BuildSettings& settings, const std::string_view project_name) {
    return settings.game_name.empty() ? std::string{project_name} : settings.game_name;
}

std::string game_file_stem(const std::string_view game_name) {
    std::string stem;
    for (const char c : game_name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem += plain ? c : (c == ' ' ? '_' : '-');
    }
    while (!stem.empty() && (stem.front() == '-' || stem.front() == '_')) stem.erase(stem.begin());
    return stem.empty() ? "game" : stem;
}

std::filesystem::path build_output_root(const std::filesystem::path& project_root,
                                        const BuildProfile& profile) {
    const std::filesystem::path output{profile.output_directory};
    return output.is_absolute() ? output : project_root / output;
}

std::string output_inside_project(const std::filesystem::path& project_root, const BuildProfile& profile) {
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(project_root), ec);
    const auto output = std::filesystem::weakly_canonical(
        std::filesystem::absolute(build_output_root(project_root, profile)), ec);
    if (ec) return {};
    const auto relative = output.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") return {};
    return relative.generic_string();
}

} // namespace relay
