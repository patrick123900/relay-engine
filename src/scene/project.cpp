#include "relay/scene/project.hpp"
#include "relay/core/json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <set>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace relay {

std::optional<std::filesystem::path> workspace_file(const std::string_view directory,
                                                   const std::string_view filename,
                                                   const std::string_view suffix) {
    if (filename.empty() || filename.size() > 128 || !filename.ends_with(suffix)) return {};
    const auto relative = std::filesystem::path(filename);
    if (relative.is_absolute()) return {};
    for (const auto& part : relative) {
        const auto name = part.string();
        if (name.empty() || name.front() == '.') return {};
        for (const char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ' ')) return {};
    }
    std::error_code ec;
    const auto root = std::filesystem::path(directory);
    auto path = root;
    // Check every ancestor, including the root, so nested links cannot escape the workspace.
    auto ancestor = std::filesystem::path{};
    for (const auto& part : root / relative) {
        ancestor /= part;
        if (std::filesystem::is_symlink(ancestor, ec)) return {};
    }
    path /= relative;
    return path;
}

std::filesystem::path Project::root() const {
    const auto parent = std::filesystem::path(filename).parent_path();
    return parent.empty() ? std::filesystem::path{"."} : parent;
}

std::string Project::json(const bool with_settings) const {
    std::string files = "[";
    for (const auto& scene : scenes) {
        if (files.size() > 1) files += ", ";
        files += '"' + json_escape(scene) + '"';
    }
    std::string settings;
    if (with_settings && (input || graphics || audio)) {
        std::string entries;
        if (input) {
            // The map keeps its own format and version; indented to sit inside the project object.
            std::string map = input_map_json(*input);
            while (!map.empty() && map.back() == '\n') map.pop_back();
            for (std::size_t at = map.find('\n'); at != std::string::npos; at = map.find('\n', at + 1))
                map.insert(at + 1, "    ");
            entries += "\n    \"input\": " + map;
        }
        if (graphics) {
            if (!entries.empty()) entries += ',';
            entries += "\n    \"graphics\": " + graphics_settings_json(*graphics);
        }
        if (audio) {
            if (!entries.empty()) entries += ',';
            entries += "\n    \"audio\": " + audio_settings_json(*audio);
        }
        settings = ",\n  \"settings\": {" + entries + "\n  }";
    }
    return "{\n  \"format\": \"relay.project\",\n  \"version\": " +
           std::to_string(project_file_version) + ",\n  \"filename\": \"" + json_escape(filename) +
           "\",\n  \"root\": \"" + json_escape(root().generic_string()) + "\",\n  \"name\": \"" +
           json_escape(name) + "\",\n  \"assets_directory\": \".\",\n  \"scenes_directory\": \"scenes\","
           "\n  \"scenes\": " + files + "],\n  \"startup_scene\": \"" + json_escape(startup_scene) +
           '"' + settings + "\n}";
}

namespace {
bool valid(const Project& project) {
    if (!workspace_file(".", project.filename, ".relayproject") ||
        project.name.empty() || project.name.size() > 128 || project.scenes.size() > 128) return false;
    std::set<std::string> unique;
    for (const auto& file : project.scenes)
        if (!workspace_file((project.root() / "scenes").generic_string(), file, ".relay.json") || !unique.insert(file).second) return false;
    return project.scenes.empty() ? project.startup_scene.empty() : unique.contains(project.startup_scene);
}
} // namespace

std::optional<Project> load_project(const std::string_view filename, std::string& error) {
    std::string warning;
    return load_project(filename, error, warning);
}

std::optional<Project> load_project(const std::string_view filename, std::string& error,
                                    std::string& warning) {
    warning.clear();
    const auto path = workspace_file(".", filename, ".relayproject");
    std::error_code ec;
    // Room for the largest input map (256 KiB) and the rest of the settings.
    if (!path || !std::filesystem::is_regular_file(*path, ec) ||
        std::filesystem::file_size(*path, ec) > 512U * 1024U || ec) {
        error = "project must be an existing safe .relayproject file inside the workspace";
        return {};
    }
    std::ifstream stream(*path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    JsonParser parser(text);
    const auto value = parser.parse();
    const auto* object = value ? value->object() : nullptr;
    if (!object) { error = "invalid project JSON"; return {}; }
    const auto string_at = [&](const char* key) {
        const auto* entry = field(*object, key);
        return entry && entry->string() ? *entry->string() : std::string{};
    };
    const auto* version = field(*object, "version");
    const auto* scenes = field(*object, "scenes");
    if (string_at("format") != "relay.project" || !version || !version->number() ||
        (*version->number() != 1 && *version->number() != project_file_version) || !scenes ||
        !scenes->array() ||
        string_at("assets_directory") != "." || string_at("scenes_directory") != "scenes") {
        error = "unsupported project format, version or directories";
        return {};
    }
    Project project{std::string(filename), string_at("name"), {}, string_at("startup_scene"), {}, false,
                    {}, {}};
    for (const auto& scene : *scenes->array()) {
        if (!scene.string()) { error = "project scenes must be filenames"; return {}; }
        project.scenes.push_back(*scene.string());
    }
    if (!valid(project)) { error = "invalid scene membership, startup scene or project name"; return {}; }
    const auto* settings = field(*object, "settings");
    if (settings && !settings->object()) { error = "project settings must be an object"; return {}; }
    if (const auto* input = settings ? field(*settings->object(), "input") : nullptr) {
        std::string problem;
        project.input = parse_input_map(json_stringify(*input), problem);
        if (!project.input) { error = "invalid input map in the project settings: " + problem; return {}; }
    } else if (auto legacy = load_legacy_input_map(project.root(), warning)) {
        project.input = std::move(*legacy);
        project.legacy_input_file = true;
    }
    if (const auto* graphics = settings ? field(*settings->object(), "graphics") : nullptr) {
        std::string problem;
        project.graphics = parse_graphics_settings(*graphics, problem);
        if (!project.graphics) { error = "invalid graphics settings in the project: " + problem; return {}; }
    }
    if (const auto* audio = settings ? field(*settings->object(), "audio") : nullptr) {
        std::string problem;
        project.audio = parse_audio_settings(*audio, problem);
        if (!project.audio) { error = "invalid audio settings in the project: " + problem; return {}; }
    }
    error.clear();
    return project;
}

bool save_project(const Project& project, std::string& error, const bool create) {
    if (!valid(project)) { error = "invalid project metadata or unsafe path"; return false; }
    const auto path = *workspace_file(".", project.filename, ".relayproject");
    std::error_code ec;
    if (create && std::filesystem::exists(path, ec)) {
        error = "project already exists";
        return false;
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "cannot create projects directory"; return false; }
    static std::atomic<std::uint64_t> serial{};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = std::filesystem::path(path.string() + "." + std::to_string(stamp) +
                                                "." + std::to_string(++serial) + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << project.json() << '\n';
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            error = "cannot write project metadata";
            return false;
        }
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        ec = std::make_error_code(std::errc::io_error);
#else
    std::filesystem::rename(temporary, path, ec);
#endif
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "cannot atomically replace project metadata";
        return false;
    }
    // The map now lives in the project file, so the old file would only mislead.
    if (project.legacy_input_file && project.input) {
        const auto legacy = project.root() / legacy_input_map_filename;
        if (!std::filesystem::is_symlink(legacy, ec) && std::filesystem::is_regular_file(legacy, ec))
            std::filesystem::remove(legacy, ec);
    }
    error.clear();
    return true;
}

std::vector<std::string> available_projects() {
    std::vector<std::string> result;
    std::error_code ec;
    if (std::filesystem::is_symlink("projects", ec)) return result;
    for (std::filesystem::recursive_directory_iterator i("projects", ec), end; !ec && i != end; i.increment(ec)) {
        if (i->is_symlink(ec) || i->path().filename().string().starts_with(".")) {
            if (i->is_directory(ec)) i.disable_recursion_pending();
            continue;
        }
        if (result.size() >= 128) break;
        const auto filename = i->path().generic_string();
        if (workspace_file(".", filename, ".relayproject") && i->is_regular_file(ec))
            result.push_back(filename);
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace relay
