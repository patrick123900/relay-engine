#include "relay/script/script_system.hpp"

#include "relay/core/engine.hpp"
#include "relay/core/hash.hpp"
#include "relay/core/process.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/scene/scene_edit.hpp"
#include "relay/scene/templates.hpp"

#include "../../sdk/relay_script_abi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#ifndef RELAY_SCRIPT_DEFAULT_COMPILER
#define RELAY_SCRIPT_DEFAULT_COMPILER "c++"
#endif
#ifndef RELAY_SCRIPT_DEFAULT_SDK_DIR
#define RELAY_SCRIPT_DEFAULT_SDK_DIR "sdk"
#endif

namespace relay {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t maximum_build_output = 64U * 1024U;
// Scripts cannot grow the scene past the size a scene file may hold.
constexpr std::size_t maximum_scene_entities = 100000U;
constexpr std::chrono::seconds compile_timeout{120};
constexpr std::string_view compile_flags =
    "-std=c++20 -O2 -g -fPIC -fvisibility=hidden -fvisibility-inlines-hidden "
    "-fdiagnostics-color=never -Wall -Wextra";

#ifdef __APPLE__
constexpr std::string_view library_suffix = ".dylib";
#else
constexpr std::string_view library_suffix = ".so";
#endif

std::string environment(const char* name, std::string_view fallback) {
    const auto* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::string{value} : std::string{fallback};
}

std::optional<std::string> read_file(const fs::path& path, std::uintmax_t maximum) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(size))) return std::nullopt;
    return text;
}

bool write_file_atomically(const fs::path& path, std::string_view text, std::string& error) {
    std::error_code code;
    fs::create_directories(path.parent_path(), code);
    const auto temporary = path.parent_path() / (".relay-tmp-" + path.filename().string());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) {
            error = "could not write " + path.filename().string();
            fs::remove(temporary, code);
            return false;
        }
    }
    fs::rename(temporary, path, code);
    if (code) {
        error = "could not replace " + path.filename().string() + ": " + code.message();
        fs::remove(temporary, code);
        return false;
    }
    return true;
}

std::string hash_text(std::initializer_list<std::string_view> parts) {
    Sha256 hash;
    for (const auto part : parts) {
        const auto size = static_cast<std::uint64_t>(part.size());
        hash.update(&size, sizeof size);
        hash.update(part.data(), part.size());
    }
    return hash.hex();
}

bool source_extension(const fs::path& path) {
    const auto extension = path.extension().string();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx";
}

bool header_extension(const fs::path& path) {
    const auto extension = path.extension().string();
    return extension == ".hpp" || extension == ".h" || extension == ".hh";
}

bool safe_component(std::string_view name) {
    if (name.empty() || name.front() == '.' || name == "..") return false;
    return std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.';
    });
}

struct SourceFile {
    std::string relative; // Relative to scripts/.
    fs::path path;        // Absolute.
    bool header{};
    std::uintmax_t size{};
    std::int64_t modified{};
};

struct SourceScan {
    std::vector<SourceFile> files;
    std::string fingerprint;
    std::string error;
};

// Lists script files below scripts/, skipping hidden entries and symlinks, in a stable order.
SourceScan scan_sources(const fs::path& root) {
    SourceScan scan;
    const auto directory = root / script_source_directory;
    std::error_code error;
    if (!fs::is_directory(directory, error)) {
        scan.fingerprint = hash_text({"empty"});
        return scan;
    }
    if (fs::is_symlink(directory, error)) {
        scan.error = "scripts folder must not be a symbolic link";
        return scan;
    }
    fs::recursive_directory_iterator iterator(directory, error), end;
    for (; !error && iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().string();
        if (name.starts_with('.') || entry.is_symlink(error)) {
            if (entry.is_directory(error)) iterator.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(error)) continue;
        const bool header = header_extension(entry.path());
        if (!header && !source_extension(entry.path())) continue;
        if (scan.files.size() == maximum_script_files) {
            scan.error = "scripts folder exceeds " + std::to_string(maximum_script_files) + " files";
            return scan;
        }
        SourceFile file;
        file.path = fs::absolute(entry.path());
        file.relative = fs::relative(entry.path(), directory).generic_string();
        file.header = header;
        file.size = entry.file_size(error);
        file.modified = static_cast<std::int64_t>(
            entry.last_write_time(error).time_since_epoch().count());
        if (file.size > maximum_script_bytes) {
            scan.error = file.relative + " exceeds the 1 MiB script size limit";
            return scan;
        }
        scan.files.push_back(std::move(file));
    }
    std::sort(scan.files.begin(), scan.files.end(),
              [](const SourceFile& a, const SourceFile& b) { return a.relative < b.relative; });
    Sha256 hash;
    for (const auto& file : scan.files) {
        hash.update(file.relative.data(), file.relative.size() + 1U);
        hash.update(&file.size, sizeof file.size);
        hash.update(&file.modified, sizeof file.modified);
    }
    scan.fingerprint = hash.hex();
    return scan;
}

std::vector<ScriptDiagnostic> parse_diagnostics(std::string_view output, const fs::path& root) {
    static const std::regex pattern(
        R"(^(.+?):(\d+):(\d+): (fatal error|error|warning|note): (.*)$)");
    std::vector<ScriptDiagnostic> diagnostics;
    const auto root_text = root.generic_string() + "/";
    std::istringstream stream{std::string{output}};
    for (std::string line; std::getline(stream, line);) {
        std::smatch match;
        if (!std::regex_match(line, match, pattern)) continue;
        if (diagnostics.size() == maximum_script_diagnostics) break;
        ScriptDiagnostic diagnostic;
        diagnostic.file = fs::path(match[1].str()).generic_string();
        if (diagnostic.file.starts_with(root_text)) diagnostic.file.erase(0, root_text.size());
        diagnostic.line = static_cast<std::uint32_t>(std::stoul(match[2].str()));
        diagnostic.column = static_cast<std::uint32_t>(std::stoul(match[3].str()));
        diagnostic.severity = match[4].str() == "fatal error" ? "error" : match[4].str();
        diagnostic.message = match[5].str();
        diagnostics.push_back(std::move(diagnostic));
    }
    return diagnostics;
}

std::vector<std::string> split_flags() {
    std::vector<std::string> flags;
    std::istringstream stream{std::string{compile_flags}};
    for (std::string flag; stream >> flag;) flags.push_back(flag);
    return flags;
}

struct BuildRequest {
    std::uint64_t build{};
    fs::path root;
    fs::path cache;
    std::string compiler;
    fs::path sdk;
    std::vector<SourceFile> files;
    std::string fingerprint;
};

struct BuildResult {
    std::uint64_t build{};
    bool succeeded{};
    bool cancelled{};
    fs::path library;
    std::string fingerprint;
    std::string error;
    std::string output;
    std::vector<ScriptDiagnostic> diagnostics;
    std::uint32_t compiled{};
    std::uint32_t sources{};
    double seconds{};
};

void append_output(std::string& destination, std::string_view text) {
    const auto room = maximum_build_output > destination.size()
                          ? maximum_build_output - destination.size() : 0U;
    destination.append(text.substr(0, room));
}

BuildResult run_build(const BuildRequest& request, const std::atomic<bool>& cancel) {
    const auto started = std::chrono::steady_clock::now();
    BuildResult result;
    result.build = request.build;
    result.fingerprint = request.fingerprint;
    const auto finish = [&]() -> BuildResult {
        result.diagnostics = parse_diagnostics(result.output, request.root);
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                             .count();
        return result;
    };
    const auto sdk_header = read_file(request.sdk / "relay_script.hpp", maximum_script_bytes);
    const auto sdk_abi = read_file(request.sdk / "relay_script_abi.h", maximum_script_bytes);
    if (!sdk_header || !sdk_abi) {
        result.error = "the Relay script SDK was not found at " + request.sdk.string() +
                       "; set RELAY_SCRIPT_SDK_DIR";
        return finish();
    }
    // Any header change recompiles every source; per-file dependency tracking is not worth its
    // complexity at script scale.
    Sha256 headers;
    for (const auto& file : request.files) {
        if (!file.header) continue;
        const auto text = read_file(file.path, maximum_script_bytes);
        if (!text) {
            result.error = "could not read " + file.relative;
            return finish();
        }
        headers.update(file.relative.data(), file.relative.size() + 1U);
        headers.update(text->data(), text->size());
    }
    const auto toolchain = hash_text({std::to_string(RELAY_SCRIPT_ABI_VERSION), request.compiler,
                                      compile_flags, *sdk_header, *sdk_abi});
    const auto header_hash = headers.hex();
    const auto objects = request.cache / "obj";
    std::error_code code;
    fs::create_directories(objects, code);
    fs::create_directories(request.cache / "lib", code);
    if (code) {
        result.error = "could not create the script cache: " + code.message();
        return finish();
    }

    struct Unit { fs::path source; fs::path object; bool cached; };
    std::vector<Unit> units;
    const auto entry_key = hash_text({toolchain, "entry"});
    const auto entry_source = objects / ("entry-" + entry_key + ".cpp");
    if (!fs::exists(entry_source, code) &&
        !write_file_atomically(entry_source,
                               "#define RELAY_SCRIPT_DEFINE_ENTRY\n#include \"relay_script.hpp\"\n",
                               result.error))
        return finish();
    units.push_back({entry_source, objects / (entry_key + ".o"), false});
    for (const auto& file : request.files) {
        if (file.header) continue;
        ++result.sources;
        const auto text = read_file(file.path, maximum_script_bytes);
        if (!text) {
            result.error = "could not read " + file.relative;
            return finish();
        }
        const auto key = hash_text({toolchain, header_hash, file.relative, *text});
        units.push_back({file.path, objects / (key + ".o"), false});
    }
    for (auto& unit : units) unit.cached = fs::exists(unit.object, code);

    const auto flags = split_flags();
    std::mutex output_mutex;
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    std::atomic<std::uint32_t> compiled{0};
    const auto worker = [&] {
        for (auto index = next++; index < units.size(); index = next++) {
            const auto& unit = units[index];
            if (unit.cached) continue;
            if (cancel || failed) return;
            const auto temporary = unit.object.string() + ".tmp";
            std::vector<std::string> arguments{request.compiler};
            arguments.insert(arguments.end(), flags.begin(), flags.end());
            arguments.insert(arguments.end(), {"-I", request.sdk.string(), "-I",
                                               (request.root / script_source_directory).string(),
                                               "-c", unit.source.string(), "-o", temporary});
            const auto process = run_process(arguments, compile_timeout, maximum_build_output);
            {
                std::lock_guard lock(output_mutex);
                append_output(result.output, process.output);
                if (!process.started)
                    result.error = "could not start the C++ compiler '" + request.compiler +
                                   "'; install one or set RELAY_SCRIPT_COMPILER";
                else if (process.timed_out)
                    result.error = "compiling " + unit.source.filename().string() + " timed out";
                else if (process.exit_code != 0 && result.error.empty())
                    result.error = "script compilation failed";
            }
            std::error_code ignored;
            if (!process.started || process.timed_out || process.exit_code != 0) {
                fs::remove(temporary, ignored);
                failed = true;
                return;
            }
            fs::rename(temporary, unit.object, ignored);
            ++compiled;
        }
    };
    const auto parallel = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1U, 8U);
    std::vector<std::thread> threads;
    for (unsigned index = 1; index < parallel; ++index) threads.emplace_back(worker);
    worker();
    for (auto& thread : threads) thread.join();
    result.compiled = compiled;
    if (cancel) {
        result.cancelled = true;
        result.error = "build cancelled";
        return finish();
    }
    if (failed) return finish();

    std::string link_key_input;
    for (const auto& unit : units) link_key_input += unit.object.filename().string();
    const auto library_key = hash_text({link_key_input}).substr(0, 24);
    result.library = request.cache / "lib" /
                     ("relay-scripts-" + library_key + std::string{library_suffix});
    if (!fs::exists(result.library, code)) {
        const auto temporary = result.library.string() + ".tmp";
        std::vector<std::string> arguments{request.compiler, "-shared", "-o", temporary};
        for (const auto& unit : units) arguments.push_back(unit.object.string());
        const auto process = run_process(arguments, compile_timeout, maximum_build_output);
        append_output(result.output, process.output);
        if (!process.started || process.timed_out || process.exit_code != 0) {
            fs::remove(temporary, code);
            result.error = process.timed_out ? "linking scripts timed out" : "linking scripts failed";
            return finish();
        }
        fs::rename(temporary, result.library, code);
        if (code) {
            result.error = "could not store the script library: " + code.message();
            return finish();
        }
    }
    result.succeeded = true;
    return finish();
}

Entity unpack(RelayEntity handle) {
    return Entity{static_cast<std::uint32_t>(handle & 0xffffffffULL),
                  static_cast<std::uint32_t>(handle >> 32U)};
}

bool finite(const RelayVec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

RelayVec3 raw(const Vec3 value) { return {value.x, value.y, value.z}; }

std::string_view callback_name(int callback) {
    switch (callback) {
    case RELAY_CALLBACK_START: return "on_start";
    case RELAY_CALLBACK_UPDATE: return "on_update";
    case RELAY_CALLBACK_CONTACT_BEGIN: return "on_contact_begin";
    case RELAY_CALLBACK_CONTACT_END: return "on_contact_end";
    case RELAY_CALLBACK_STOP: return "on_stop";
    case RELAY_CALLBACK_RELOAD: return "on_reload";
    case RELAY_CALLBACK_DESTROY: return "on_destroy";
    default: return "unknown";
    }
}

fs::path config_directory() {
#ifdef _WIN32
    return fs::path{environment("APPDATA", ".")} / "Relay";
#elif defined(__APPLE__)
    return fs::path{environment("HOME", ".")} / "Library" / "Application Support" / "Relay";
#else
    const auto xdg = environment("XDG_CONFIG_HOME", "");
    return (xdg.empty() ? fs::path{environment("HOME", ".")} / ".config" : fs::path{xdg}) /
           "relay-engine";
#endif
}

std::string trust_key(const fs::path& root) {
    std::error_code error;
    auto canonical = fs::weakly_canonical(fs::absolute(root, error), error);
    return canonical.lexically_normal().generic_string();
}

std::vector<std::string> read_trust_list() {
    std::vector<std::string> entries;
    const auto text = read_file(script_trust_path(), 1024U * 1024U);
    if (!text) return entries;
    std::istringstream stream{*text};
    for (std::string line; std::getline(stream, line);)
        if (!line.empty() && line.front() == '/') entries.push_back(line);
#ifdef _WIN32
        else if (line.size() > 2 && line[1] == ':') entries.push_back(line);
#endif
    return entries;
}

} // namespace

std::string_view script_build_state_name(const ScriptBuildState state) {
    switch (state) {
    case ScriptBuildState::idle: return "idle";
    case ScriptBuildState::building: return "building";
    case ScriptBuildState::ready: return "ready";
    case ScriptBuildState::failed: return "failed";
    }
    return "idle";
}

fs::path script_trust_path() {
    const auto configured = environment("RELAY_SCRIPT_TRUST_PATH", "");
    return configured.empty() ? config_directory() / "trusted-script-projects" : fs::path{configured};
}

bool project_scripts_trusted(const fs::path& project_root) {
    const auto entries = read_trust_list();
    return std::find(entries.begin(), entries.end(), trust_key(project_root)) != entries.end();
}

bool set_project_scripts_trusted(const fs::path& project_root, const bool trusted,
                                 std::string& error) {
    auto entries = read_trust_list();
    const auto key = trust_key(project_root);
    std::erase(entries, key);
    if (trusted) entries.push_back(key);
    std::string text;
    for (const auto& entry : entries) text += entry + "\n";
    return write_file_atomically(script_trust_path(), text, error);
}

std::optional<fs::path> script_source_path(const fs::path& project_root, const std::string_view path) {
    if (path.empty() || path.size() > 128U) return std::nullopt;
    const fs::path relative{path};
    if (relative.is_absolute() || (!source_extension(relative) && !header_extension(relative)))
        return std::nullopt;
    std::size_t depth = 0;
    for (const auto& part : relative)
        if (!safe_component(part.string()) || ++depth > 4U) return std::nullopt;
    // Reject links anywhere along the way so a script path cannot leave the project.
    std::error_code error;
    auto current = project_root / script_source_directory;
    if (fs::is_symlink(current, error)) return std::nullopt;
    for (const auto& part : relative) {
        current /= part;
        if (fs::is_symlink(current, error)) return std::nullopt;
    }
    return current;
}

fs::path script_sdk_directory() {
    return fs::absolute(environment("RELAY_SCRIPT_SDK_DIR", RELAY_SCRIPT_DEFAULT_SDK_DIR));
}

std::optional<std::string> read_script_source(const fs::path& project_root, const std::string_view path,
                                              std::string& error) {
    const auto file = script_source_path(project_root, path);
    std::error_code code;
    if (!file) {
        error = "script path must be a .cpp, .cc, .cxx, .hpp, .h or .hh file inside scripts/";
        return std::nullopt;
    }
    if (!fs::is_regular_file(*file, code)) {
        error = "script file does not exist";
        return std::nullopt;
    }
    auto text = read_file(*file, maximum_script_source_bytes);
    if (!text) error = "script file is unreadable or larger than 256 KiB";
    return text;
}

bool write_script_source(const fs::path& project_root, const std::string_view path,
                         const std::string_view text, const bool create_only, std::string& error) {
    const auto file = script_source_path(project_root, path);
    if (!file) {
        error = "script path must be a .cpp, .cc, .cxx, .hpp, .h or .hh file inside scripts/";
        return false;
    }
    if (text.size() > maximum_script_source_bytes) {
        error = "script source exceeds 256 KiB";
        return false;
    }
    std::error_code code;
    if (fs::exists(*file, code) && (create_only || !fs::is_regular_file(*file, code))) {
        error = create_only ? "a script with that name already exists" : "script path is not a file";
        return false;
    }
    return write_file_atomically(*file, text, error);
}

std::string script_template(const std::string_view behaviour) {
    const std::string name{behaviour};
    return "#include \"relay_script.hpp\"\n\n"
           "// Add " + name + " to a node with + Add Component in the Inspector, then press Run Game.\n"
           "class " + name + " : public relay::Behaviour {\n"
           "public:\n"
           "    // Fields listed here are editable per node in the Inspector.\n"
           "    void properties(relay::Properties& p) override {\n"
           "        p.add(\"height\", height);\n"
           "        p.add(\"speed\", speed);\n"
           "    }\n\n"
           "    void on_start() override {\n"
           "        start_position = self().position();\n"
           "    }\n\n"
           "    void on_update(double delta_seconds) override {\n"
           "        (void)delta_seconds;\n"
           "        // Bob gently above the starting position.\n"
           "        auto position = start_position;\n"
           "        position.y += height * std::sin(relay::world::time() * speed);\n"
           "        self().set_position(position);\n"
           "    }\n\n"
           "private:\n"
           "    double height = 0.25;\n"
           "    double speed = 2.0;\n"
           "    relay::Vec3 start_position;\n"
           "};\n"
           "RELAY_BEHAVIOUR(" + name + ")\n";
}

struct ScriptSystem::Impl {
    struct Library {
#ifndef _WIN32
        void* handle{};
#endif
        const RelayScriptModule* module{};
        std::vector<ScriptBehaviourInfo> behaviours;
        std::map<std::string, std::uint32_t, std::less<>> index;
        std::uint64_t build{};
        std::string fingerprint;
        fs::path path;
        ~Library() {
#ifndef _WIN32
            if (handle) dlclose(handle);
#endif
        }
    };
    struct Instance {
        Entity entity;
        std::size_t component{};
        Script script;
        void* object{};
        bool failed{};
        bool started{};   // on_start has run; instances spawned mid-game start before their update.
        bool destroyed{}; // on_destroy has run; the instance goes when the entity leaves the scene.
    };

    explicit Impl(Engine& host_engine) : engine(host_engine) {
        host.abi_version = RELAY_SCRIPT_ABI_VERSION;
        host.size = sizeof(RelayHostApi);
        host.context = this;
        host.time = [](void* context) { return self(context).engine.status().elapsed_seconds; };
        host.frame = [](void* context) { return self(context).engine.status().frame_index; };
        host.log = [](void* context, int level, const char* text, size_t length) {
            auto& impl = self(context);
            auto message = impl.log_prefix();
            message.append(text, std::min<size_t>(length, 2048U));
            impl.engine.logs().write(level == RELAY_LOG_ERROR     ? LogLevel::error
                                     : level == RELAY_LOG_WARNING ? LogLevel::warning
                                                                  : LogLevel::info,
                                     message);
        };
        host.alive = [](void* context, RelayEntity entity) {
            return self(context).engine.scene().contains(unpack(entity)) ? 1 : 0;
        };
        host.find = [](void* context, const char* name, size_t length) -> RelayEntity {
            const std::string_view wanted{name, length};
            auto& scene = self(context).engine.scene();
            for (const auto entity : scene.entities())
                if (scene.get(entity)->name == wanted) return entity.packed();
            return 0;
        };
        host.name = [](void* context, RelayEntity entity, char* buffer, size_t capacity) -> size_t {
            const auto* record = self(context).engine.scene().get(unpack(entity));
            if (!record) return 0;
            std::memcpy(buffer, record->name.data(), std::min(capacity, record->name.size()));
            return record->name.size();
        };
        host.parent = [](void* context, RelayEntity entity) -> RelayEntity {
            const auto* record = self(context).engine.scene().get(unpack(entity));
            return record && record->parent.valid() ? record->parent.packed() : 0;
        };
        host.get_position = [](void* context, RelayEntity entity, RelayVec3* value) {
            return self(context).read(entity, value, &Transform::position);
        };
        host.set_position = [](void* context, RelayEntity entity, RelayVec3 value) {
            return self(context).write(entity, value, &Transform::position);
        };
        host.get_rotation = [](void* context, RelayEntity entity, RelayVec3* value) {
            return self(context).read(entity, value, &Transform::rotation_degrees);
        };
        host.set_rotation = [](void* context, RelayEntity entity, RelayVec3 value) {
            return self(context).write(entity, value, &Transform::rotation_degrees);
        };
        host.get_scale = [](void* context, RelayEntity entity, RelayVec3* value) {
            return self(context).read(entity, value, &Transform::scale);
        };
        host.set_scale = [](void* context, RelayEntity entity, RelayVec3 value) {
            return self(context).write(entity, value, &Transform::scale);
        };
        host.world_position = [](void* context, RelayEntity entity, RelayVec3* value) {
            const auto& scene = self(context).engine.scene();
            auto current = unpack(entity);
            if (!scene.contains(current)) return 0;
            // Keyframed transforms are sampled so the result matches what is rendered.
            auto world = editor_identity();
            for (std::size_t depth = 0; current.valid() && depth < 4096U; ++depth) {
                const auto* record = scene.get(current);
                if (!record) return 0;
                const auto local = record->transform_animation &&
                                           !record->transform_animation->keys.empty()
                    ? sample_transform_animation(*record->transform_animation, record->transform)
                    : record->transform;
                world = editor_multiply(editor_compose(local.position, local.rotation_degrees,
                                                       local.scale), world);
                current = record->parent;
            }
            const Vec3 point{world[12], world[13], world[14]};
            *value = raw(point);
            return 1;
        };
        host.get_velocity = [](void* context, RelayEntity entity, RelayVec3* value) {
            auto& owner = self(context).engine;
            const auto velocity = owner.physics().velocity(owner.scene(), unpack(entity));
            if (velocity) *value = raw(*velocity);
            return velocity ? 1 : 0;
        };
        host.set_velocity = [](void* context, RelayEntity entity, RelayVec3 value) {
            auto& owner = self(context).engine;
            return owner.physics().set_velocity(owner.scene(), unpack(entity),
                                                 {value.x, value.y, value.z}) ? 1 : 0;
        };
        host.get_angular_velocity = [](void* context, RelayEntity entity, RelayVec3* value) {
            auto& owner = self(context).engine;
            const auto velocity = owner.physics().angular_velocity(owner.scene(), unpack(entity));
            if (velocity) *value = raw(*velocity);
            return velocity ? 1 : 0;
        };
        host.set_angular_velocity = [](void* context, RelayEntity entity, RelayVec3 value) {
            auto& owner = self(context).engine;
            return owner.physics().set_angular_velocity(owner.scene(), unpack(entity),
                                                         {value.x, value.y, value.z}) ? 1 : 0;
        };
        host.apply_impulse = [](void* context, RelayEntity entity, RelayVec3 impulse,
                                const RelayVec3* point) {
            auto& owner = self(context).engine;
            return owner.physics().apply_impulse(
                       owner.scene(), unpack(entity), {impulse.x, impulse.y, impulse.z},
                       point ? std::optional<Vec3>{Vec3{point->x, point->y, point->z}}
                             : std::nullopt) ? 1 : 0;
        };
        host.raycast = [](void* context, RelayVec3 origin, RelayVec3 direction,
                          double maximum_distance, uint32_t layer_mask, RelayEntity ignore,
                          RelayRayHit* hit) {
            auto& owner = self(context).engine;
            const auto result = owner.physics().raycast(
                owner.scene(), {origin.x, origin.y, origin.z},
                {direction.x, direction.y, direction.z}, maximum_distance, layer_mask,
                unpack(ignore));
            if (!result.hit) return 0;
            *hit = {result.entity.packed(), result.distance, raw(result.point),
                    raw(result.normal)};
            return 1;
        };
        host.child = [](void* context, RelayEntity parent, const char* name,
                        size_t length) -> RelayEntity {
            const auto& scene = self(context).engine.scene();
            const auto owner = unpack(parent);
            const std::string_view wanted{name, length};
            if (!scene.contains(owner)) return 0;
            for (const auto entity : scene.entities()) {
                const auto* record = scene.get(entity);
                if (record->parent == owner && record->name == wanted) return entity.packed();
            }
            return 0;
        };
        host.activate_camera = [](void* context, RelayEntity entity) {
            auto& scene = self(context).engine.scene();
            const auto target = unpack(entity);
            const auto* record = scene.get(target);
            if (!record || !record->camera) return 0;
            auto camera = *record->camera;
            camera.active = true;
            return scene.set_camera(target, camera) ? 1 : 0;
        };
        host.input_action = [](void* context, const char* name, size_t length, int query) {
            return self(context).engine.input().action({name, length}, query_of(query)) ? 1 : 0;
        };
        host.input_axis = [](void* context, const char* name, size_t length) {
            return self(context).engine.input().axis({name, length});
        };
        host.input_control = [](void* context, const char* control, size_t length, int query) {
            return self(context).engine.input().control({control, length}, query_of(query)) ? 1 : 0;
        };
        host.input_control_value = [](void* context, const char* control, size_t length) {
            return self(context).engine.input().control_value({control, length});
        };
        host.input_mouse = [](void* context, RelayVec3* position, RelayVec3* delta) {
            const auto& input = self(context).engine.input();
            *position = {input.mouse_x(), input.mouse_y(), 0.0};
            *delta = {input.mouse_dx(), input.mouse_dy(), input.mouse_wheel()};
        };
        host.create_entity = [](void* context, const char* name, size_t length,
                                RelayEntity parent) -> RelayEntity {
            auto& impl = self(context);
            auto& scene = impl.engine.scene();
            const std::string wanted{name, length};
            if (!impl.can_spawn("create " + wanted) || !impl.parent_alive(parent, "create " + wanted))
                return 0;
            const auto entity = scene.create("Entity", unpack(parent));
            if (!entity.valid()) return 0;
            if (!wanted.empty() && !scene.set_name(entity, wanted)) {
                (void)scene.destroy(entity);
                impl.warn("create: names are 1 to 128 bytes");
                return 0;
            }
            return entity.packed();
        };
        host.instantiate = [](void* context, const char* name, size_t length, RelayEntity parent,
                              const RelayVec3* position, const RelayVec3* rotation) -> RelayEntity {
            auto& impl = self(context);
            auto& scene = impl.engine.scene();
            const std::string wanted{name, length};
            const auto action = "instantiate " + wanted;
            if (!impl.can_spawn(action) || !impl.parent_alive(parent, action)) return 0;
            if ((position && !finite(*position)) || (rotation && !finite(*rotation))) {
                impl.warn(action + ": the position or rotation is not finite");
                return 0;
            }
            const auto* loaded = impl.template_named(wanted);
            if (!loaded) return 0;
            std::string error;
            const auto placed = place_template(scene, *loaded, unpack(parent), {}, error);
            if (!placed) {
                impl.warn(action + ": " + error);
                return 0;
            }
            auto transform = scene.get(*placed)->transform;
            if (position) transform.position = {position->x, position->y, position->z};
            if (rotation) transform.rotation_degrees = {rotation->x, rotation->y, rotation->z};
            (void)scene.set_transform(*placed, transform);
            return impl.finish_spawn(*placed);
        };
        host.clone = [](void* context, RelayEntity entity) -> RelayEntity {
            auto& impl = self(context);
            auto& scene = impl.engine.scene();
            const auto source = unpack(entity);
            if (!scene.contains(source) || !impl.can_spawn("clone")) return 0;
            const std::array selection{source};
            const auto clipboard = copy_selection(scene, selection);
            const auto pasted = clipboard ? paste_selection(scene, *clipboard, {}, true)
                                          : std::vector<Entity>{};
            if (pasted.size() != 1U) {
                impl.warn("clone " + source.to_string() + " failed");
                return 0;
            }
            (void)scene.set_name(pasted.front(), scene.get(source)->name);
            return impl.finish_spawn(pasted.front());
        };
        host.destroy = [](void* context, RelayEntity entity) {
            auto& impl = self(context);
            const auto target = unpack(entity);
            if (!impl.running || impl.stopping || !impl.engine.scene().contains(target)) return 0;
            impl.pending_destroy.push_back(target);
            return 1;
        };
        host.children = [](void* context, RelayEntity parent, RelayEntity* out,
                           size_t capacity) -> size_t {
            const auto& scene = self(context).engine.scene();
            const auto owner = unpack(parent);
            if (!scene.contains(owner)) return 0;
            std::vector<Entity> found;
            for (const auto entity : scene.entities())
                if (scene.get(entity)->parent == owner) found.push_back(entity);
            return copy_entities(found, out, capacity);
        };
        host.overlaps = [](void* context, RelayEntity entity, RelayEntity* out,
                           size_t capacity) -> size_t {
            auto& owner = self(context).engine;
            return copy_entities(owner.physics().overlaps(owner.scene(), unpack(entity)).entities,
                                 out, capacity);
        };
        host.overlap_sphere = [](void* context, RelayVec3 center, double radius,
                                 uint32_t layer_mask, RelayEntity ignore, RelayEntity* out,
                                 size_t capacity) -> size_t {
            auto& owner = self(context).engine;
            return copy_entities(owner.physics()
                                     .overlap_sphere(owner.scene(), {center.x, center.y, center.z},
                                                     radius, layer_mask, unpack(ignore))
                                     .entities,
                                 out, capacity);
        };
    }

    static size_t copy_entities(const std::vector<Entity>& entities, RelayEntity* out,
                                size_t capacity) {
        for (std::size_t index = 0; index < std::min(capacity, entities.size()); ++index)
            out[index] = entities[index].packed();
        return entities.size();
    }

    [[nodiscard]] std::string log_prefix() const {
        return "[" + (active ? active->script.behaviour + " " + active->entity.to_string()
                             : std::string{"script"}) + "] ";
    }

    void warn(const std::string& message) {
        engine.logs().write(LogLevel::warning, log_prefix() + message);
    }

    // Spawning is possible only while the game runs, and the scene stays loadable in size.
    bool can_spawn(const std::string& action) {
        if (!running || stopping) return false;
        if (engine.scene().entities().size() < maximum_scene_entities) return true;
        if (!entity_limit_warned) warn(action + ": the scene already holds the maximum 100000 entities");
        entity_limit_warned = true;
        return false;
    }

    bool parent_alive(const RelayEntity parent, const std::string& action) {
        if (parent == 0 || engine.scene().contains(unpack(parent))) return true;
        warn(action + ": the parent entity is gone");
        return false;
    }

    // Project templates are read once per run; a missing one is reported once.
    const LoadedTemplate* template_named(const std::string& name) {
        auto found = templates.find(name);
        if (found == templates.end()) {
            std::string error = "template names use letters, digits, spaces, '-' and '_'";
            std::optional<LoadedTemplate> loaded;
            if (valid_template_name(name))
                loaded = load_template(project_root(), "project:" + name, error);
            if (!loaded) warn("instantiate " + name + ": " + error);
            found = templates.emplace(name, std::move(loaded)).first;
        }
        return found->second ? &*found->second : nullptr;
    }

    // Entities in `top`'s subtree, including it, in scene order.
    [[nodiscard]] std::vector<Entity> subtree(const Entity top) const {
        const auto& scene = engine.scene();
        std::vector<Entity> result;
        for (const auto entity : scene.entities())
            for (auto current = entity; current.valid() && scene.contains(current);
                 current = scene.get(current)->parent)
                if (current == top) {
                    result.push_back(entity);
                    break;
                }
        return result;
    }

    // A spawned tree gets its physics bodies and script instances; the scripts start before
    // their first update.
    RelayEntity finish_spawn(const Entity spawned) {
        engine.physics().add_bodies(engine.scene(), spawned);
        if (library)
            for (const auto entity : subtree(spawned)) {
                const auto& scripts = engine.scene().get(entity)->scripts;
                for (std::size_t component = 0; component < scripts.size(); ++component) {
                    if (!scripts[component].enabled) continue;
                    auto instance = std::make_unique<Instance>(
                        Instance{entity, component, scripts[component]});
                    create(*instance);
                    by_entity[entity.packed()].push_back(instance.get());
                    instances.push_back(std::move(instance));
                }
            }
        return spawned.packed();
    }

    // Calls on_start on every instance that has not started, including ones spawned meanwhile.
    void start_pending() {
        for (std::size_t index = 0; index < instances.size(); ++index) {
            auto& instance = *instances[index];
            if (instance.started) continue;
            instance.started = true;
            if (engine.scene().contains(instance.entity)) call(instance, RELAY_CALLBACK_START);
        }
    }

    // Applies queued destruction: on_destroy for every script in each doomed tree, then the
    // entities, their instances and their physics bodies go. on_destroy may destroy more.
    void flush_destroyed() {
        auto& scene = engine.scene();
        bool removed = false;
        for (std::size_t pass = 0; !pending_destroy.empty() && pass < 64U; ++pass) {
            const auto batch = std::exchange(pending_destroy, {});
            std::set<Entity> doomed;
            for (const auto top : batch)
                if (scene.contains(top))
                    for (const auto entity : subtree(top)) doomed.insert(entity);
            for (std::size_t index = 0; index < instances.size(); ++index) {
                auto& instance = *instances[index];
                if (instance.destroyed || !doomed.contains(instance.entity)) continue;
                if (instance.started) call(instance, RELAY_CALLBACK_DESTROY);
                instance.destroyed = true;
            }
            for (const auto top : batch) (void)scene.destroy(top);
            removed = true;
        }
        if (!removed) return;
        std::erase_if(instances, [&](const std::unique_ptr<Instance>& instance) {
            if (scene.contains(instance->entity)) return false;
            if (instance->object) library->module->destroy(instance->object);
            return true;
        });
        by_entity.clear();
        for (const auto& instance : instances)
            by_entity[instance->entity.packed()].push_back(instance.get());
        engine.physics().remove_missing_bodies(scene);
    }

    ~Impl() { cancel_build(); }

    static InputState::Query query_of(int query) {
        return query == RELAY_INPUT_PRESSED    ? InputState::Query::pressed
               : query == RELAY_INPUT_RELEASED ? InputState::Query::released
                                               : InputState::Query::held;
    }

    static Impl& self(void* context) { return *static_cast<Impl*>(context); }

    int read(RelayEntity entity, RelayVec3* value, Vec3 Transform::*member) const {
        const auto* record = engine.scene().get(unpack(entity));
        if (!record) return 0;
        *value = raw(record->transform.*member);
        return 1;
    }

    int write(RelayEntity entity, RelayVec3 value, Vec3 Transform::*member) {
        auto& scene = engine.scene();
        const auto target = unpack(entity);
        auto* record = scene.get(target);
        if (!record || !finite(value)) return 0;
        auto transform = record->transform;
        transform.*member = {value.x, value.y, value.z};
        (void)scene.set_transform(target, transform);
        engine.physics().sync_transforms(scene, target);
        return 1;
    }

    [[nodiscard]] std::optional<fs::path> project_root() const {
        if (!engine.project()) return std::nullopt;
        std::error_code error;
        return fs::absolute(engine.project()->root(), error).lexically_normal();
    }

    void cancel_build() {
        cancel = true;
        if (worker.joinable()) worker.join();
        cancel = false;
        std::lock_guard lock(mutex);
        finished.reset();
    }

    // Returns to a clean state when the project changes, so nothing leaks between projects.
    void observe_project() {
        const auto current = project_root();
        if (current == root) return;
        cancel_build();
        instances.clear();
        by_entity.clear();
        pending_destroy.clear();
        templates.clear();
        running = false;
        library.reset();
        root = current;
        trusted = root && project_scripts_trusted(*root);
        state = ScriptBuildState::idle;
        attempted_fingerprint.clear();
        last_error.clear();
        last_output.clear();
        diagnostics.clear();
        runtime_errors.clear();
        runtime_error_count = 0;
        compiled_files = source_files = 0;
        build_seconds = 0.0;
    }

    void record_error(const Instance& instance, std::string_view callback, std::string message) {
        ++runtime_error_count;
        engine.logs().write(LogLevel::error, "Script " + instance.script.behaviour + " on " +
                                                 instance.entity.to_string() + " failed in " +
                                                 std::string{callback} + ": " + message +
                                                 "; the instance is disabled");
        if (runtime_errors.size() == maximum_script_runtime_errors)
            runtime_errors.erase(runtime_errors.begin());
        runtime_errors.push_back({engine.status().frame_index, instance.entity, instance.component,
                                  instance.script.behaviour, std::string{callback},
                                  std::move(message)});
    }

    // Callbacks nest when a script spawns entities, so the caller's `active` is restored.
    void call(Instance& instance, int callback, double delta = 0.0, RelayEntity other = 0) {
        if (instance.failed || instance.destroyed || !instance.object) return;
        char error[RELAY_SCRIPT_ERROR_CAPACITY] = {};
        auto* const caller = std::exchange(active, &instance);
        const int ok = library->module->call(instance.object, callback, delta, other, error,
                                             sizeof error);
        active = caller;
        if (!ok) {
            instance.failed = true;
            record_error(instance, callback_name(callback),
                         error[0] ? std::string{error} : std::string{"unknown error"});
        }
    }

    void create(Instance& instance) {
        instance.object = nullptr;
        instance.failed = false;
        const auto found = library->index.find(instance.script.behaviour);
        if (found == library->index.end()) {
            instance.failed = true;
            record_error(instance, "create", "behaviour is not defined by the current scripts");
            return;
        }
        char error[RELAY_SCRIPT_ERROR_CAPACITY] = {};
        auto* const caller = std::exchange(active, &instance);
        instance.object = library->module->create(found->second, instance.entity.packed(), error,
                                                  sizeof error);
        active = caller;
        if (!instance.object) {
            instance.failed = true;
            record_error(instance, "create", error[0] ? std::string{error} : "construction failed");
            return;
        }
        // Authored values that code no longer declares are skipped with a warning, not fatal:
        // renaming a field must not stop the game.
        for (const auto& property : instance.script.properties) {
            RelayPropertyValue value{};
            value.type = static_cast<int>(property.type);
            value.boolean = property.boolean ? 1 : 0;
            value.number = property.number;
            value.vector = raw(property.vector);
            value.text = property.text.data();
            value.text_length = property.text.size();
            error[0] = '\0';
            auto* const property_caller = std::exchange(active, &instance);
            const int ok = library->module->set_property(instance.object, property.name.data(),
                                                         property.name.size(), &value, error,
                                                         sizeof error);
            active = property_caller;
            if (!ok)
                engine.logs().write(LogLevel::warning,
                                    "Script " + instance.script.behaviour + " on " +
                                        instance.entity.to_string() + " ignored property " +
                                        property.name + ": " + std::string{error});
        }
    }

    void destroy_objects() {
        for (auto& instance : instances) {
            if (instance->object) library->module->destroy(instance->object);
            instance->object = nullptr;
        }
    }

    std::unique_ptr<Library> load(const BuildResult& result, std::string& error) {
#ifdef _WIN32
        (void)result;
        error = "native scripts are not yet supported on Windows";
        return nullptr;
#else
        auto loaded = std::make_unique<Library>();
        loaded->handle = dlopen(result.library.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!loaded->handle) {
            const auto* message = dlerror();
            error = std::string{"could not load the script library: "} +
                    (message ? message : "unknown error");
            return nullptr;
        }
        const auto entry = reinterpret_cast<RelayScriptEntry>(
            dlsym(loaded->handle, RELAY_SCRIPT_ENTRY_SYMBOL));
        loaded->module = entry ? entry(&host) : nullptr;
        if (!loaded->module || loaded->module->abi_version != RELAY_SCRIPT_ABI_VERSION ||
            loaded->module->size < sizeof(RelayScriptModule) || !loaded->module->set_property) {
            error = "the script library was built against an incompatible Relay SDK";
            return nullptr;
        }
        const auto count = loaded->module->behaviour_count();
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto* name = loaded->module->behaviour_name(index);
            if (!name || !valid_behaviour_name(name)) {
                error = "a behaviour name is not a plain C++ identifier; register unqualified "
                        "class names with RELAY_BEHAVIOUR";
                return nullptr;
            }
            if (!loaded->index.emplace(name, index).second) {
                error = std::string{"behaviour "} + name + " is registered more than once";
                return nullptr;
            }
            ScriptBehaviourInfo info{name, {}};
            const auto properties = std::min<std::uint32_t>(
                loaded->module->property_count(index),
                static_cast<std::uint32_t>(maximum_script_properties));
            for (std::uint32_t item = 0; item < properties; ++item) {
                RelayPropertyInfo declared{};
                if (!loaded->module->property_info(index, item, &declared) || !declared.name ||
                    declared.value.type < RELAY_PROPERTY_BOOLEAN ||
                    declared.value.type > RELAY_PROPERTY_TEXT) continue;
                ScriptProperty property;
                property.name = declared.name;
                property.type = static_cast<ScriptProperty::Type>(declared.value.type);
                property.boolean = declared.value.boolean != 0;
                property.number = declared.value.number;
                property.vector = {declared.value.vector.x, declared.value.vector.y,
                                   declared.value.vector.z};
                if (declared.value.text)
                    property.text.assign(declared.value.text,
                                         std::min(declared.value.text_length,
                                                  maximum_script_text_bytes));
                const bool duplicate = std::any_of(
                    info.properties.begin(), info.properties.end(),
                    [&](const ScriptProperty& other) { return other.name == property.name; });
                if (!valid_behaviour_name(property.name) || duplicate ||
                    !std::isfinite(property.number) || !std::isfinite(property.vector.x) ||
                    !std::isfinite(property.vector.y) || !std::isfinite(property.vector.z)) {
                    error = "behaviour " + info.name + " declares an invalid or duplicate "
                            "property; property names must be unique C++ identifiers";
                    return nullptr;
                }
                info.properties.push_back(std::move(property));
            }
            loaded->behaviours.push_back(std::move(info));
        }
        loaded->build = result.build;
        loaded->fingerprint = result.fingerprint;
        loaded->path = result.library;
        return loaded;
#endif
    }

    void adopt(BuildResult result) {
        if (result.cancelled) {
            state = library ? ScriptBuildState::ready : ScriptBuildState::idle;
            return;
        }
        last_output = std::move(result.output);
        diagnostics = std::move(result.diagnostics);
        compiled_files = result.compiled;
        source_files = result.sources;
        build_seconds = result.seconds;
        last_error = std::move(result.error);
        if (!result.succeeded) {
            state = ScriptBuildState::failed;
            engine.logs().write(LogLevel::error, "Script build " + std::to_string(result.build) +
                                                     " failed: " + last_error);
            return;
        }
        std::string error;
        auto loaded = load(result, error);
        if (!loaded) {
            state = ScriptBuildState::failed;
            last_error = error;
            engine.logs().write(LogLevel::error, "Script load failed: " + error);
            return;
        }
        state = ScriptBuildState::ready;
        if (running) {
            // Old objects must be destroyed while their code is still mapped.
            destroy_objects();
            library = std::move(loaded);
            // Index loops: constructors and on_reload may spawn, which appends instances.
            const auto count = instances.size();
            for (std::size_t index = 0; index < count; ++index) create(*instances[index]);
            for (std::size_t index = 0; index < count; ++index)
                if (instances[index]->started) call(*instances[index], RELAY_CALLBACK_RELOAD);
            flush_destroyed();
            ++reloads;
            engine.logs().write(LogLevel::info, "Scripts reloaded from build " +
                                                    std::to_string(result.build));
        } else {
            library = std::move(loaded);
            engine.logs().write(LogLevel::info,
                                "Scripts built: " + std::to_string(library->behaviours.size()) +
                                    " behaviours, " + std::to_string(compiled_files) + " of " +
                                    std::to_string(source_files) + " files compiled");
        }
        remove_stale_libraries();
    }

    void remove_stale_libraries() const {
        if (!root || !library) return;
        std::error_code error;
        const auto& current = library->path;
        for (fs::directory_iterator it(*root / script_cache_directory / "lib", error), end;
             !error && it != end; it.increment(error)) {
            if (it->path() != current && it->path().extension() == library_suffix)
                fs::remove(it->path(), error);
        }
    }

    Engine& engine;
    RelayHostApi host{};
    std::optional<fs::path> root;
    bool trusted{};
    ScriptBuildState state{ScriptBuildState::idle};
    std::unique_ptr<Library> library;
    // Heap-allocated, so spawning during a callback never moves the instance being called.
    std::vector<std::unique_ptr<Instance>> instances;
    std::unordered_map<std::uint64_t, std::vector<Instance*>> by_entity;
    std::vector<Entity> pending_destroy;
    std::map<std::string, std::optional<LoadedTemplate>, std::less<>> templates;
    bool entity_limit_warned{};
    Instance* active{};
    bool running{};
    bool stopping{};
    std::uint64_t contact_cursor{};
    std::uint64_t build_counter{};
    std::string attempted_fingerprint;
    std::string last_error, last_output;
    std::vector<ScriptDiagnostic> diagnostics;
    std::vector<ScriptRuntimeError> runtime_errors;
    std::uint64_t runtime_error_count{};
    std::uint64_t reloads{};
    std::uint32_t compiled_files{}, source_files{};
    double build_seconds{};

    std::thread worker;
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::optional<BuildResult> finished;
};

ScriptSystem::ScriptSystem(Engine& engine) : impl_(std::make_unique<Impl>(engine)) {}
ScriptSystem::~ScriptSystem() = default;

bool ScriptSystem::set_trusted(const bool trusted, std::string& error) {
    impl_->observe_project();
    if (!impl_->root) {
        error = "open a project before changing script trust";
        return false;
    }
    if (impl_->running) {
        error = "stop the game before changing script trust";
        return false;
    }
    if (!set_project_scripts_trusted(*impl_->root, trusted, error)) return false;
    impl_->trusted = trusted;
    if (!trusted) {
        impl_->cancel_build();
        impl_->library.reset();
        impl_->state = ScriptBuildState::idle;
        impl_->attempted_fingerprint.clear();
    }
    impl_->engine.logs().write(LogLevel::info, trusted ? "Project scripts trusted"
                                                       : "Project script trust revoked");
    return true;
}

bool ScriptSystem::start_build(std::string& error) {
    impl_->observe_project();
    auto& impl = *impl_;
    if (!impl.root) {
        error = "scripts need an open project";
        return false;
    }
    if (!impl.trusted) {
        error = "this project is not trusted to build native scripts; a person must trust it "
                "in the editor first";
        return false;
    }
#ifdef _WIN32
    error = "native scripts are not yet supported on Windows";
    return false;
#endif
    if (impl.state == ScriptBuildState::building) {
        error = "a script build is already running";
        return false;
    }
    auto scan = scan_sources(*impl.root);
    if (!scan.error.empty()) {
        error = scan.error;
        return false;
    }
    if (impl.worker.joinable()) impl.worker.join();
    BuildRequest request;
    request.build = ++impl.build_counter;
    request.root = *impl.root;
    request.cache = *impl.root / script_cache_directory;
    request.compiler = environment("RELAY_SCRIPT_COMPILER", RELAY_SCRIPT_DEFAULT_COMPILER);
    request.sdk = script_sdk_directory();
    request.files = std::move(scan.files);
    request.fingerprint = scan.fingerprint;
    impl.attempted_fingerprint = scan.fingerprint;
    impl.state = ScriptBuildState::building;
    impl.worker = std::thread([&impl, request = std::move(request)] {
        auto result = run_build(request, impl.cancel);
        std::lock_guard lock(impl.mutex);
        impl.finished = std::move(result);
    });
    return true;
}

void ScriptSystem::poll() {
    impl_->observe_project();
    std::optional<BuildResult> result;
    {
        std::lock_guard lock(impl_->mutex);
        result.swap(impl_->finished);
    }
    if (!result) return;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->adopt(std::move(*result));
}

ScriptStatus ScriptSystem::status() {
    poll();
    const auto& impl = *impl_;
    ScriptStatus status;
    status.project = impl.root.has_value();
    status.trusted = impl.trusted;
#ifdef _WIN32
    status.supported = false;
#else
    status.supported = true;
#endif
    status.state = impl.state;
    if (impl.root && impl.trusted) {
        const auto scan = scan_sources(*impl.root);
        status.stale = scan.fingerprint != impl.attempted_fingerprint &&
                       impl.state != ScriptBuildState::building;
        if (!scan.error.empty() && status.stale) status.error = scan.error;
    }
    status.build = impl.build_counter;
    status.loaded_build = impl.library ? impl.library->build : 0U;
    status.build_seconds = impl.build_seconds;
    status.compiled_files = impl.compiled_files;
    status.source_files = impl.source_files;
    if (status.error.empty()) status.error = impl.last_error;
    status.output = impl.last_output;
    status.compiler = environment("RELAY_SCRIPT_COMPILER", RELAY_SCRIPT_DEFAULT_COMPILER);
    status.sdk = script_sdk_directory().generic_string();
    if (impl.library) status.behaviours = impl.library->behaviours;
    status.diagnostics = impl.diagnostics;
    status.runtime_errors = impl.runtime_errors;
    status.runtime_error_count = impl.runtime_error_count;
    status.reloads = impl.reloads;
    status.instances = impl.instances.size();
    return status;
}

bool ScriptSystem::can_start(const Scene& scene, std::string& error) {
    poll();
    bool scripted = false;
    for (const auto entity : scene.entities())
        for (const auto& script : scene.get(entity)->scripts) scripted |= script.enabled;
    if (!scripted) return true;
    const auto& impl = *impl_;
    if (!impl.root) {
        error = "the scene uses scripts, which need an open project";
        return false;
    }
    if (!impl.trusted) {
        error = "the scene uses scripts, but this project is not trusted to run native code; "
                "trust it in the editor or disable the script components";
        return false;
    }
    if (impl.state == ScriptBuildState::building) {
        error = "scripts are still building; poll scripts.status and try again";
        return false;
    }
    if (impl.state == ScriptBuildState::failed) {
        error = "the latest script build failed: " + impl.last_error +
                "; see scripts.status for diagnostics";
        return false;
    }
    if (!impl.library) {
        error = "scripts have not been built yet; run scripts.build first";
        return false;
    }
    const auto scan = scan_sources(*impl.root);
    if (scan.fingerprint != impl.library->fingerprint) {
        error = "scripts changed since the last build; run scripts.build first";
        return false;
    }
    for (const auto entity : scene.entities())
        for (const auto& script : scene.get(entity)->scripts)
            if (script.enabled && !impl.library->index.contains(script.behaviour)) {
                error = "entity " + entity.to_string() + " uses behaviour " + script.behaviour +
                        ", which the scripts do not define";
                return false;
            }
    return true;
}

void ScriptSystem::start() {
    auto& impl = *impl_;
    impl.instances.clear();
    impl.by_entity.clear();
    impl.pending_destroy.clear();
    impl.templates.clear();
    impl.entity_limit_warned = false;
    impl.running = true;
    impl.contact_cursor = impl.engine.physics().contact_events().latest_sequence;
    if (!impl.library) return;
    const auto& scene = impl.engine.scene();
    for (const auto entity : scene.entities()) {
        const auto& scripts = scene.get(entity)->scripts;
        for (std::size_t component = 0; component < scripts.size(); ++component) {
            if (!scripts[component].enabled) continue;
            impl.instances.push_back(std::make_unique<Impl::Instance>(
                Impl::Instance{entity, component, scripts[component]}));
            impl.by_entity[entity.packed()].push_back(impl.instances.back().get());
        }
    }
    const auto count = impl.instances.size();
    for (std::size_t index = 0; index < count; ++index) impl.create(*impl.instances[index]);
    impl.start_pending();
    impl.flush_destroyed();
}

void ScriptSystem::update(const double delta_seconds) {
    auto& impl = *impl_;
    if (!impl.running || !impl.library) return;
    impl.start_pending();
    // Instances spawned during this loop are appended and first update next step.
    const auto& scene = impl.engine.scene();
    const auto count = impl.instances.size();
    for (std::size_t index = 0; index < count; ++index) {
        auto& instance = *impl.instances[index];
        if (instance.started && scene.contains(instance.entity))
            impl.call(instance, RELAY_CALLBACK_UPDATE, delta_seconds);
    }
    impl.flush_destroyed();
}

void ScriptSystem::dispatch_contacts() {
    auto& impl = *impl_;
    if (!impl.running || !impl.library) return;
    // Scripts spawned during this step's updates start before they hear about contacts.
    impl.start_pending();
    const auto events = impl.engine.physics().contact_events(impl.contact_cursor);
    if (events.oldest_sequence > impl.contact_cursor + 1U && !events.events.empty())
        impl.engine.logs().write(LogLevel::warning,
                                 "Scripts missed contact events that left the bounded history");
    for (const auto& event : events.events) {
        const int callback = event.began ? RELAY_CALLBACK_CONTACT_BEGIN : RELAY_CALLBACK_CONTACT_END;
        for (const auto& [target, other] : {std::pair{event.first, event.second},
                                            std::pair{event.second, event.first}}) {
            const auto found = impl.by_entity.find(target.packed());
            if (found == impl.by_entity.end()) continue;
            // A copy: callbacks may spawn, which adds to the index.
            const auto targets = found->second;
            for (auto* instance : targets) impl.call(*instance, callback, 0.0, other.packed());
        }
    }
    impl.contact_cursor = events.latest_sequence;
    impl.flush_destroyed();
}

void ScriptSystem::stop() {
    auto& impl = *impl_;
    if (!impl.running) return;
    impl.stopping = true;
    if (impl.library) {
        const auto count = impl.instances.size();
        for (std::size_t index = 0; index < count; ++index)
            if (impl.instances[index]->started)
                impl.call(*impl.instances[index], RELAY_CALLBACK_STOP);
        impl.destroy_objects();
    }
    impl.instances.clear();
    impl.by_entity.clear();
    impl.pending_destroy.clear();
    impl.templates.clear();
    impl.running = false;
    impl.stopping = false;
}

} // namespace relay
