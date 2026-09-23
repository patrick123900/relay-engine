#pragma once

#include "relay/scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

class Engine;

// Project scripts live under scripts/; builds and their libraries under the hidden cache below.
inline constexpr std::string_view script_source_directory = "scripts";
inline constexpr std::string_view script_cache_directory = ".relay-cache/scripts";
inline constexpr std::size_t maximum_script_files = 256U;
inline constexpr std::uintmax_t maximum_script_bytes = 1024U * 1024U;
inline constexpr std::size_t maximum_script_diagnostics = 200U;
inline constexpr std::size_t maximum_script_runtime_errors = 64U;

enum class ScriptBuildState : std::uint8_t { idle, building, ready, failed };
[[nodiscard]] std::string_view script_build_state_name(ScriptBuildState state);

struct ScriptDiagnostic {
    std::string file; // Project-relative when the file is inside the project.
    std::uint32_t line{};
    std::uint32_t column{};
    std::string severity; // error, warning or note
    std::string message;
};

struct ScriptRuntimeError {
    std::uint64_t frame{};
    Entity entity{};
    std::size_t component{}; // Index among the entity's script components.
    std::string behaviour;
    std::string callback;
    std::string message;
};

// A behaviour the loaded library registers, with its declared properties at their code defaults.
struct ScriptBehaviourInfo {
    std::string name;
    std::vector<ScriptProperty> properties;
};

struct ScriptStatus {
    bool project{};   // A project is open; scripts need one.
    bool trusted{};   // A person has allowed this project's native code to build and run.
    bool supported{}; // This platform can load script libraries.
    ScriptBuildState state{ScriptBuildState::idle};
    bool stale{};     // Sources changed since the latest build started; a build is due.
    std::uint64_t build{};       // Increments with every started build.
    std::uint64_t loaded_build{}; // Build number of the loaded library, zero when none.
    double build_seconds{};
    std::uint32_t compiled_files{}; // Sources recompiled by the latest build; the rest were cached.
    std::uint32_t source_files{};
    std::string error;  // Why the latest build or load failed.
    std::string output; // Bounded raw compiler output of the latest build.
    std::string compiler;
    std::string sdk;
    std::vector<ScriptBehaviourInfo> behaviours; // Registered by the loaded library, sorted.
    std::vector<ScriptDiagnostic> diagnostics;
    std::vector<ScriptRuntimeError> runtime_errors;
    std::uint64_t runtime_error_count{};
    std::uint64_t reloads{};
    std::size_t instances{};
};

// Trust is kept per user, outside every project, so a project can never mark itself trusted.
// RELAY_SCRIPT_TRUST_PATH overrides the file (tests use temporary files).
[[nodiscard]] std::filesystem::path script_trust_path();
[[nodiscard]] bool project_scripts_trusted(const std::filesystem::path& project_root);
[[nodiscard]] bool set_project_scripts_trusted(const std::filesystem::path& project_root,
                                               bool trusted, std::string& error);

// Validates a scripts/ relative path (as sent over the protocol) and returns it joined to the
// project root. Only .cpp/.cc/.cxx/.hpp/.h files, no traversal or hidden components.
[[nodiscard]] std::optional<std::filesystem::path> script_source_path(
    const std::filesystem::path& project_root, std::string_view path);
inline constexpr std::size_t maximum_script_source_bytes = 256U * 1024U;
[[nodiscard]] std::optional<std::string> read_script_source(const std::filesystem::path& project_root,
                                                            std::string_view path,
                                                            std::string& error);
// Writes atomically, creating folders below scripts/. `create_only` refuses to replace a file.
[[nodiscard]] bool write_script_source(const std::filesystem::path& project_root,
                                       std::string_view path, std::string_view text,
                                       bool create_only, std::string& error);
// The SDK folder scripts compile against: RELAY_SCRIPT_SDK_DIR, else the build's configured copy.
[[nodiscard]] std::filesystem::path script_sdk_directory();
// The starter file for a new behaviour class.
[[nodiscard]] std::string script_template(std::string_view behaviour);

// Compiles project scripts into a native library in the background, loads it, and drives
// behaviour instances through Run Game. Everything except the compiler runs on the engine thread.
class ScriptSystem {
public:
    explicit ScriptSystem(Engine& engine);
    ~ScriptSystem();
    ScriptSystem(const ScriptSystem&) = delete;
    ScriptSystem& operator=(const ScriptSystem&) = delete;

    [[nodiscard]] bool set_trusted(bool trusted, std::string& error);
    [[nodiscard]] bool start_build(std::string& error);
    // Adopts finished builds (hot reloading a running game) and notices project changes.
    void poll();
    [[nodiscard]] ScriptStatus status();

    // Run Game refuses to start when the scene's enabled scripts cannot run as authored.
    [[nodiscard]] bool can_start(const Scene& scene, std::string& error);
    void start();
    void update(double delta_seconds);
    void dispatch_contacts();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace relay
