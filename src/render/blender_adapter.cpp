#include "relay/render/blender_adapter.hpp"

#include "relay/core/hash.hpp"
#include "relay/core/process.hpp"
#include "relay/core/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace relay {
namespace {

constexpr std::uintmax_t maximum_conversion_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_diagnostic_bytes = 64U * 1024U;
constexpr std::string_view adapter_version = "relay-blender-glb-v2";


std::filesystem::path selected_executable(const BlenderConversionSettings& settings) {
    if (!settings.executable.empty()) return settings.executable;
    if (const auto* configured = std::getenv("RELAY_BLENDER_EXECUTABLE");
        configured != nullptr && *configured != '\0') {
        return std::filesystem::path{configured};
    }
    return std::filesystem::path{"blender"};
}

bool trusted_conversion_enabled(const BlenderConversionSettings& settings) {
    if (settings.allow_unsandboxed) return true;
    const auto* value = std::getenv("RELAY_BLENDER_TRUSTED");
    return value != nullptr && std::string_view{value} == "1";
}

std::vector<std::string> contained_arguments(const std::filesystem::path& root,
                                            const std::vector<std::string>& blender_arguments,
                                            const bool trusted,
                                            const std::filesystem::path& writable_cache = {}) {
    if (trusted) return blender_arguments;
#ifdef __linux__
    std::vector<std::string> arguments{
        "bwrap", "--unshare-all", "--die-with-parent", "--new-session",
        "--ro-bind", "/usr", "/usr", "--proc", "/proc", "--dev", "/dev",
        "--tmpfs", "/tmp", "--dir", "/tmp/relay-home", "--clearenv",
        "--setenv", "PATH", "/usr/bin:/bin", "--setenv", "LANG", "C.UTF-8",
        "--setenv", "HOME", "/tmp/relay-home",
        "--setenv", "XDG_CACHE_HOME", "/tmp/relay-home", "--unsetenv", "PYTHONPATH",
        "--unsetenv", "PYTHONHOME", "--unsetenv", "BLENDER_USER_SCRIPTS",
        "--unsetenv", "BLENDER_SYSTEM_SCRIPTS"};
    for (const auto* directory : {"/lib", "/lib64", "/bin", "/opt", "/etc/fonts"}) {
        if (std::filesystem::exists(directory)) {
            arguments.insert(arguments.end(), {"--ro-bind", directory, directory});
        }
    }
    if (std::filesystem::exists("/etc/ld.so.cache")) {
        arguments.insert(arguments.end(), {"--ro-bind", "/etc/ld.so.cache", "/etc/ld.so.cache"});
    }
    // Bubblewrap creates the destination parents as private empty directories.
    arguments.insert(arguments.end(), {"--ro-bind", root.string(), root.string()});
    if (!writable_cache.empty()) {
        arguments.insert(arguments.end(), {"--bind", writable_cache.string(),
                                           writable_cache.string()});
    }
    arguments.insert(arguments.end(), {"--chdir", root.string(), "--"});
    arguments.insert(arguments.end(), blender_arguments.begin(), blender_arguments.end());
    return arguments;
#else
    (void)root;
    (void)blender_arguments;
    (void)writable_cache;
    return {};
#endif
}

bool is_inside(const std::filesystem::path& root, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && relative.begin() != relative.end() &&
           *relative.begin() != std::filesystem::path{".."};
}

bool is_valid_glb(const std::filesystem::path& path) {
    std::error_code code;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, code))) return false;
    const auto size = std::filesystem::file_size(path, code);
    if (code || size < 12U || size > maximum_conversion_bytes) return false;
    std::array<char, 4> magic{};
    std::ifstream input(path, std::ios::binary);
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    return input && magic == std::array<char, 4>{'g', 'l', 'T', 'F'};
}

std::string source_digest(const std::filesystem::path& source, std::string& error) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        error = "Blender source could not be opened";
        return {};
    }
    Sha256 digest;
    std::array<char, 64U * 1024U> block{};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        if (input.gcount() > 0) {
            digest.update(block.data(), static_cast<std::size_t>(input.gcount()));
        }
    }
    if (input.bad()) {
        error = "Blender input ended with a read error";
        return {};
    }
    return digest.hex();
}

std::string first_line(std::string text) {
    if (const auto newline = text.find_first_of("\r\n"); newline != std::string::npos) {
        text.resize(newline);
    }
    return text;
}

} // namespace

BlenderConversionResult convert_blend_to_glb(const std::filesystem::path& assets_root,
                                              const std::filesystem::path& source,
                                              const BlenderConversionSettings& settings,
                                              std::string& error) {
    BlenderConversionResult result;
    error.clear();
    std::error_code code;
    const auto root = std::filesystem::weakly_canonical(assets_root, code);
    if (code) {
        error = "project assets directory is not accessible";
        return result;
    }
    const auto canonical_source = std::filesystem::weakly_canonical(source, code);
    if (code || !is_inside(root, canonical_source) ||
        canonical_source.extension() != ".blend" ||
        !std::filesystem::is_regular_file(canonical_source, code)) {
        error = "Blender source must be a regular .blend file inside the assets directory";
        return result;
    }
    const auto source_size = std::filesystem::file_size(canonical_source, code);
    if (code || source_size > maximum_conversion_bytes) {
        error = "Blender source exceeds the 64 MiB import limit";
        return result;
    }
    if (settings.timeout_seconds == 0U || settings.timeout_seconds > 900U) {
        error = "Blender conversion timeout must be between 1 and 900 seconds";
        return result;
    }
    const auto executable = selected_executable(settings);
    const bool trusted = trusted_conversion_enabled(settings);
    const auto version_arguments = contained_arguments(root,
                                                       {executable.string(), "--version"}, trusted);
    if (version_arguments.empty()) {
        error = "sandboxed Blender conversion is unavailable on this platform; trusted-input "
                "conversion requires administrator opt-in with RELAY_BLENDER_TRUSTED=1";
        return result;
    }
    result.sandboxed = !trusted;
    const auto version = run_process(version_arguments,
                                     std::chrono::seconds{10}, maximum_diagnostic_bytes);
    if (!version.started || version.timed_out || version.exit_code != 0) {
        error = version.timed_out ? "Blender version probe timed out"
                                  : "Blender or its Bubblewrap sandbox could not start; configure "
                                    "RELAY_BLENDER_EXECUTABLE and ensure bwrap is installed";
        result.diagnostics = version.output;
        return result;
    }
    result.blender_version = first_line(version.output);
    // Limit preflight to data that can affect glTF export. blend_paths() also includes unrelated
    // startup brush assets and output paths, which must not become project dependencies.
    constexpr std::string_view dependency_expression =
        "import bpy,json; paths=[bpy.path.abspath(x.filepath,library=x.library) "
        "for x in bpy.data.images if x.filepath and x.source not in {'GENERATED','VIEWER'} "
        "and not x.packed_file and not x.packed_files]; "
        "paths += [bpy.path.abspath(x.filepath) for x in bpy.data.libraries]; "
        "paths += [bpy.path.abspath(x.filepath,library=x.library) for x in bpy.data.fonts "
        "if x.filepath and x.filepath != '<builtin>' and not x.packed_file]; "
        "paths += [bpy.path.abspath(x.filepath,library=x.library) for x in bpy.data.cache_files "
        "if x.filepath]; print('RELAY_DEPENDENCIES:'+json.dumps(sorted(set(paths))))";
    const auto preflight = run_process(contained_arguments(root,
        {executable.string(), "--background", "--factory-startup", "--disable-autoexec",
         canonical_source.string(), "--python-exit-code", "1", "--python-expr",
         std::string{dependency_expression}, "--", "relay-dependencies"}, trusted),
        std::chrono::seconds{settings.timeout_seconds}, maximum_diagnostic_bytes);
    result.diagnostics = preflight.output;
    if (!preflight.started || preflight.timed_out || preflight.exit_code != 0) {
        error = preflight.timed_out ? "Blender dependency preflight timed out"
                                    : "Blender dependency preflight failed";
        return result;
    }
    constexpr std::string_view marker = "RELAY_DEPENDENCIES:";
    const auto marker_position = preflight.output.find(marker);
    if (marker_position == std::string::npos) {
        error = "Blender dependency preflight returned no dependency list";
        return result;
    }
    const auto list_begin = marker_position + marker.size();
    const auto list_end = preflight.output.find_first_of("\r\n", list_begin);
    const auto list_text = preflight.output.substr(list_begin, list_end - list_begin);
    JsonParser parser(list_text);
    const auto dependencies = parser.parse();
    if (!dependencies || dependencies->array() == nullptr ||
        dependencies->array()->size() > 64U) {
        error = "Blender dependency list is malformed or exceeds 64 files";
        return result;
    }
    auto digest_text = source_digest(canonical_source, error);
    if (digest_text.empty()) return result;
    for (const auto& value : *dependencies->array()) {
        if (value.string() == nullptr) {
            error = "Blender dependency list contains a non-path value";
            return result;
        }
        const auto dependency = std::filesystem::weakly_canonical(*value.string(), code);
        if (code || !is_inside(root, dependency) ||
            !std::filesystem::is_regular_file(dependency, code)) {
            error = "Blender dependency resolves outside the assets directory or is missing: " +
                    *value.string();
            return result;
        }
        const auto size = std::filesystem::file_size(dependency, code);
        if (code || size > maximum_conversion_bytes) {
            error = "Blender dependency exceeds the 64 MiB import limit";
            return result;
        }
        const auto dependency_digest = source_digest(dependency, error);
        if (dependency_digest.empty()) return result;
        const auto relative = dependency.lexically_relative(root);
        result.dependencies.push_back(relative);
        digest_text += relative.generic_string() + ':' + dependency_digest;
    }
    digest_text += std::string{adapter_version};
    digest_text += trusted ? ":trusted" : ":bubblewrap";
    digest_text += result.blender_version;
    digest_text += settings.export_animations ? ":animations" : ":no-animations";
    digest_text += settings.export_cameras ? ":cameras" : ":no-cameras";
    digest_text += settings.export_lights ? ":lights" : ":no-lights";
    Sha256 cache_digest;
    cache_digest.update(digest_text.data(), digest_text.size());

    const auto cache_directory = root / ".relay-cache" / "blender";
    const auto canonical_cache = std::filesystem::weakly_canonical(cache_directory, code);
    if (code || !is_inside(root, canonical_cache)) {
        error = "Blender conversion cache resolves outside the assets directory";
        return result;
    }
    std::filesystem::create_directories(canonical_cache, code);
    if (code) {
        error = "Blender conversion cache could not be created";
        return result;
    }
    const auto output = canonical_cache / (cache_digest.hex() + ".glb");
    if (is_valid_glb(output)) {
        result.converted = true;
        result.cache_hit = true;
        result.converted_model = output.lexically_relative(root);
        return result;
    }
    std::filesystem::remove(output, code);

    static std::atomic<std::uint64_t> serial{0U};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temporary = output.parent_path() /
                           (output.stem().string() + ".tmp." +
                            std::to_string(ticks) + '.' +
                            std::to_string(serial.fetch_add(1U, std::memory_order_relaxed)) +
                            ".glb");
    const auto python_bool = [](const bool value) { return value ? "True" : "False"; };
    const std::string expression =
        "import bpy,sys; bpy.ops.export_scene.gltf(filepath=sys.argv[sys.argv.index('--')+1], "
        "export_format='GLB', export_animations=" +
        std::string{python_bool(settings.export_animations)} + ", export_cameras=" +
        python_bool(settings.export_cameras) + ", export_lights=" +
        python_bool(settings.export_lights) + ")";
    const auto conversion = run_process(contained_arguments(root,
        {executable.string(), "--background", "--factory-startup", "--disable-autoexec",
         canonical_source.string(), "--python-exit-code", "1", "--python-expr", expression, "--",
         temporary.string()}, trusted, canonical_cache),
        std::chrono::seconds{settings.timeout_seconds}, maximum_diagnostic_bytes);
    result.diagnostics = conversion.output;
    if (!conversion.started || conversion.timed_out || conversion.exit_code != 0) {
        std::filesystem::remove(temporary, code);
        if (!conversion.started) error = "Blender conversion process could not start";
        else if (conversion.timed_out) error = "Blender conversion timed out";
        else error = "Blender conversion failed with exit code " +
                     std::to_string(conversion.exit_code);
        return result;
    }
    if (!is_valid_glb(temporary)) {
        std::filesystem::remove(temporary, code);
        error = "Blender conversion did not produce a valid GLB below the 64 MiB limit";
        return result;
    }
#ifdef _WIN32
    if (MoveFileExW(std::filesystem::path{temporary}.c_str(), output.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        std::filesystem::remove(temporary, code);
        error = "Blender conversion cache replacement failed";
        return result;
    }
#else
    std::filesystem::rename(temporary, output, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "Blender conversion cache replacement failed";
        return result;
    }
#endif
    result.converted = true;
    result.converted_model = output.lexically_relative(root);
    return result;
}

} // namespace relay
