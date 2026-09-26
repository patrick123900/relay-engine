#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"

#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/observe/profiler.hpp"
#include "relay/physics/collision.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/scene/scene_edit.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/project_files.hpp"
#include "relay/render/vulkan_device.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/scene/components.hpp"
#include "relay/scene/node_types.hpp"
#include "relay/scene/scene_io.hpp"
#include "relay/scene/templates.hpp"
#include "relay/script/script_system.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <set>
#include <utility>

namespace relay {
namespace {

std::string escape_json(const std::string_view value) {
    return json_escape(value);
}

std::string string_field(const std::string_view json, const std::string_view key) {
    JsonParser parser(json);
    const auto parsed = parser.parse();
    if (!parsed || !parsed->object()) return {};
    const auto* value = field(*parsed->object(), key);
    return value && value->string() ? *value->string() : std::string{};
}

std::optional<std::string> optional_string_field(const std::string_view json,
                                                 const std::string_view key) {
    JsonParser parser(json);
    const auto parsed = parser.parse();
    const auto* value = parsed && parsed->object() ? field(*parsed->object(), key) : nullptr;
    return value && value->string() ? std::optional{*value->string()} : std::nullopt;
}

std::uint64_t unsigned_field(const std::string_view json, const std::string_view key,
                             const std::uint64_t fallback) {
    const std::string marker = "\"" + std::string(key) + "\"";
    auto position = json.find(marker);
    if (position == std::string_view::npos) return fallback;
    position = json.find(':', position + marker.size());
    if (position == std::string_view::npos) return fallback;
    ++position;
    while (position < json.size() && (json[position] == ' ' || json[position] == '\t')) ++position;
    auto end = position;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    std::uint64_t value{};
    const auto result = std::from_chars(json.data() + position, json.data() + end, value);
    return result.ec == std::errc{} ? value : fallback;
}

std::optional<double> number_field(const std::string_view json, const std::string_view key) {
    JsonParser parser(json);
    const auto value = parser.parse();
    const auto* entry = value && value->object() ? field(*value->object(), key) : nullptr;
    return entry && entry->number() ? std::optional{*entry->number()} : std::nullopt;
}

bool boolean_field(const std::string_view json, const std::string_view key, const bool fallback) {
    JsonParser parser(json);
    const auto value = parser.parse();
    const auto* entry = value && value->object() ? field(*value->object(), key) : nullptr;
    return entry && entry->boolean() ? *entry->boolean() : fallback;
}

std::optional<Entity> entity_field(const std::string_view json, const std::string_view key) {
    const auto encoded = string_field(json, key);
    if (encoded.empty() || encoded == "null") return Entity{};
    return Entity::parse(encoded);
}

std::string profile_report_json(const ProfileReport& report) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    const auto kind = [](const ProfileKind value) {
        return value == ProfileKind::wait ? "\"wait\"" : "\"work\"";
    };
    out << "{\"paused\":" << (report.paused ? "true" : "false") << ",\"capacity\":" << report.capacity
        << ",\"frames\":" << report.frames << ",\"first_frame\":" << report.first_frame
        << ",\"last_frame\":" << report.last_frame << ",\"average_ms\":" << report.average_ms
        << ",\"minimum_ms\":" << report.minimum_ms << ",\"maximum_ms\":" << report.maximum_ms
        << ",\"p95_ms\":" << report.p95_ms << ",\"fps\":" << report.fps
        << ",\"cpu_ms\":" << report.cpu_ms << ",\"wait_ms\":" << report.wait_ms << ",\"gpu_ms\":";
    if (report.gpu_ms >= 0.0) out << report.gpu_ms;
    else out << "null";
    out << ",\"bottleneck\":\"" << report.bottleneck << "\",\"scopes\":[";
    for (std::size_t index = 0; index < report.scopes.size(); ++index) {
        const auto& scope = report.scopes[index];
        if (index != 0U) out << ',';
        out << "{\"name\":\"" << escape_json(scope.name) << "\",\"parent\":" << scope.parent
            << ",\"depth\":" << scope.depth << ",\"kind\":" << kind(scope.kind)
            << ",\"total_ms\":" << scope.total_ms << ",\"self_ms\":" << scope.self_ms
            << ",\"max_ms\":" << scope.max_ms << ",\"calls\":" << scope.calls << '}';
    }
    out << "],\"hotspots\":[";
    for (std::size_t index = 0; index < report.hotspots.size(); ++index) {
        const auto& hotspot = report.hotspots[index];
        if (index != 0U) out << ',';
        out << "{\"name\":\"" << escape_json(hotspot.name) << "\",\"kind\":" << kind(hotspot.kind)
            << ",\"self_ms\":" << hotspot.self_ms << ",\"total_ms\":" << hotspot.total_ms
            << ",\"calls\":" << hotspot.calls << '}';
    }
    out << "],\"gpu_passes\":[";
    for (std::size_t index = 0; index < report.gpu_passes.size(); ++index) {
        const auto& pass = report.gpu_passes[index];
        if (index != 0U) out << ',';
        out << "{\"name\":\"" << escape_json(pass.name) << "\",\"average_ms\":" << pass.average_ms
            << ",\"max_ms\":" << pass.max_ms << '}';
    }
    out << "],\"history\":[";
    for (std::size_t index = 0; index < report.history.size(); ++index) {
        const auto& frame = report.history[index];
        if (index != 0U) out << ',';
        out << "{\"frame\":" << frame.index << ",\"ms\":" << frame.milliseconds << ",\"gpu_ms\":";
        if (frame.gpu_milliseconds >= 0.0) out << frame.gpu_milliseconds;
        else out << "null";
        out << ",\"game\":" << (frame.game ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

std::string history_json(const SceneHistory& history) {
    return "{\"undo_depth\":" + std::to_string(history.undo_depth()) +
           ",\"redo_depth\":" + std::to_string(history.redo_depth()) +
           ",\"revision\":" + std::to_string(history.revision()) +
           ",\"next_undo\":\"" + escape_json(history.next_undo_label()) +
           "\",\"next_redo\":\"" + escape_json(history.next_redo_label()) + "\"}";
}

std::string response_prefix(const std::uint64_t id) {
    return "{\"id\":" + std::to_string(id) + ",\"ok\":true,\"result\":";
}

std::string error_response(const std::uint64_t id, const std::string_view message) {
    return "{\"id\":" + std::to_string(id) +
           ",\"ok\":false,\"error\":\"" + escape_json(message) + "\"}";
}

std::optional<std::filesystem::path> safe_scene_path(const Engine& engine, const std::string_view filename) {
    return workspace_file(engine.project() ? (engine.project()->root() / "scenes").generic_string() : "scenes", filename, ".relay.json");
}

std::optional<std::filesystem::path> safe_trace_path(const std::string_view filename) {
    if (filename.empty() || filename.size() > 128U || !filename.ends_with(".relay-trace.jsonl") ||
        filename.front() == '.') return std::nullopt;
    for (const char character : filename) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_' ||
              character == '.')) return std::nullopt;
    }
    return std::filesystem::path{"traces"} / filename;
}

std::optional<std::filesystem::path> safe_video_path(const std::string_view filename) {
    if (filename.empty() || filename.size() > 128U || !filename.ends_with(".webm") ||
        filename.front() == '.') return std::nullopt;
    for (const char character : filename) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_' ||
              character == '.')) return std::nullopt;
    }
    return std::filesystem::path{"captures"} / filename;
}

// Captures always land in the project captures directory. The wire form tolerates an explicit
// "captures/" prefix because the MCP bridge sends one, but the directory is decided here rather
// than by the caller, so a native agent cannot write into the process working directory.
std::optional<std::filesystem::path> safe_capture_path(std::string_view filename) {
    constexpr std::string_view directory = "captures";
    if (filename.starts_with("captures/")) filename.remove_prefix(directory.size() + 1U);
    if (filename.empty() || filename.size() > 128U || filename.front() == '.') return std::nullopt;
    if (!filename.ends_with(".png") && !filename.ends_with(".bmp")) return std::nullopt;
    for (const char character : filename) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_' ||
              character == '.')) return std::nullopt;
    }
    return std::filesystem::path{directory} / filename;
}

// Returns a validated relative path. The importer joins it against the project root itself
// and sandboxes every dependency the model goes on to reference.
std::optional<std::string> safe_model_filename(const std::string_view filename) {
    if (!workspace_file(".", filename, "")) return {};
    static constexpr std::array extensions{".gltf", ".glb", ".fbx", ".obj", ".dae", ".blend"};
    const auto extension = std::filesystem::path{filename}.extension().string();
    if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
        return std::nullopt;
    }
    return std::string{filename};
}

std::string capture_state_name(const CaptureJobState state) {
    switch (state) {
    case CaptureJobState::queued: return "queued";
    case CaptureJobState::writing: return "writing";
    case CaptureJobState::complete: return "complete";
    case CaptureJobState::failed: return "failed";
    }
    return "failed";
}

void append_property(std::ostringstream& output, const ScriptProperty& property) {
    output << "{\"name\":\"" << escape_json(property.name) << "\",\"type\":\""
           << script_property_type_name(property.type) << "\",\"value\":";
    switch (property.type) {
    case ScriptProperty::Type::boolean: output << (property.boolean ? "true" : "false"); break;
    case ScriptProperty::Type::number: output << property.number; break;
    case ScriptProperty::Type::vector:
        output << '[' << property.vector.x << ',' << property.vector.y << ',' << property.vector.z
               << ']';
        break;
    case ScriptProperty::Type::text: output << '"' << escape_json(property.text) << '"'; break;
    }
    output << '}';
}

void append_behaviours(std::ostringstream& output, const std::vector<ScriptBehaviourInfo>& behaviours) {
    output << '[';
    for (std::size_t index = 0; index < behaviours.size(); ++index) {
        output << (index ? "," : "") << "{\"name\":\"" << escape_json(behaviours[index].name)
               << "\",\"properties\":[";
        const auto& properties = behaviours[index].properties;
        for (std::size_t item = 0; item < properties.size(); ++item) {
            if (item) output << ',';
            append_property(output, properties[item]);
        }
        output << "]}";
    }
    output << ']';
}

std::string script_status_json(const ScriptStatus& status) {
    std::ostringstream output;
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    // The tail of the compiler output is the useful part; diagnostics carry the structure.
    constexpr std::size_t output_tail = 8U * 1024U;
    const auto tail = status.output.size() > output_tail
                          ? status.output.substr(status.output.size() - output_tail)
                          : status.output;
    output << "{\"project\":" << flag(status.project) << ",\"trusted\":" << flag(status.trusted)
           << ",\"supported\":" << flag(status.supported) << ",\"state\":\""
           << script_build_state_name(status.state) << "\",\"stale\":" << flag(status.stale)
           << ",\"build\":" << status.build << ",\"loaded_build\":" << status.loaded_build
           << ",\"build_seconds\":" << status.build_seconds
           << ",\"compiled_files\":" << status.compiled_files
           << ",\"source_files\":" << status.source_files << ",\"error\":\""
           << escape_json(status.error) << "\",\"compiler\":\"" << escape_json(status.compiler)
           << "\",\"sdk\":\"" << escape_json(status.sdk) << "\",\"behaviours\":";
    append_behaviours(output, status.behaviours);
    output << ",\"instances\":" << status.instances << ",\"reloads\":" << status.reloads
           << ",\"diagnostics\":[";
    for (std::size_t index = 0; index < status.diagnostics.size(); ++index) {
        const auto& diagnostic = status.diagnostics[index];
        output << (index ? "," : "") << "{\"file\":\"" << escape_json(diagnostic.file)
               << "\",\"line\":" << diagnostic.line << ",\"column\":" << diagnostic.column
               << ",\"severity\":\"" << diagnostic.severity << "\",\"message\":\""
               << escape_json(diagnostic.message) << "\"}";
    }
    output << "],\"runtime_errors\":[";
    for (std::size_t index = 0; index < status.runtime_errors.size(); ++index) {
        const auto& error = status.runtime_errors[index];
        output << (index ? "," : "") << "{\"frame\":" << error.frame << ",\"entity\":\""
               << error.entity.to_string() << "\",\"component\":" << error.component
               << ",\"behaviour\":\"" << escape_json(error.behaviour)
               << "\",\"callback\":\"" << error.callback << "\",\"message\":\""
               << escape_json(error.message) << "\"}";
    }
    output << "],\"runtime_error_count\":" << status.runtime_error_count << ",\"output\":\""
           << escape_json(tail) << "\"}";
    return output.str();
}

} // namespace

ControlProtocol::ControlProtocol(Engine& engine, CaptureHandler capture_handler,
                                 RenderInspectionHandler render_inspection_handler)
    : engine_(engine), capture_handler_(std::move(capture_handler)),
      render_inspection_handler_(std::move(render_inspection_handler)) {}

std::string ControlProtocol::handle(const std::string_view request) {
    const auto id = unsigned_field(request, "id", 0);
    JsonParser request_parser(request);
    const auto request_value = request_parser.parse();
    if (!request_value || !request_value->object()) {
        return error_response(id, request_parser.error().find("duplicate") != std::string::npos
                                      ? "request contains a duplicate field"
                                      : "request must be a valid JSON object");
    }
    const auto method = string_field(request, "method");
    if (method.empty()) {
        return error_response(id, "missing method");
    }
    std::string validation_error;
    if (!validate_protocol_request(request, method, validation_error)) {
        return error_response(id, validation_error);
    }
    const auto* const specification = find_protocol_method(method);
    const bool read_only = specification != nullptr && specification->read_only;
    if (engine_.game_session_active() && !read_only &&
        (method.starts_with("scene.") || method.starts_with("project.") ||
         method.starts_with("assets.") || method.starts_with("component.") ||
         method.starts_with("templates.") || method == "trace.replay"))
        return error_response(id, "stop the game before editing or saving the authored scene");
    if (method.starts_with("session.") || method.starts_with("chat.") || method.starts_with("bridge."))
        return session_dispatch(request);
    if (method == "project.open" || method == "project.create" || method == "project.close") {
        agent_grants_.clear();
        scoped_grants_.clear();
        policy_audit("session.project_revoked", true);
    }
    // Entity grants cannot survive scene replacement or history restoration to another allocation.
    if (method == "scene.load" || method == "scene.clear" || method == "scene.undo" || method == "scene.redo") {
        const auto size = scoped_grants_.size();
        std::erase_if(scoped_grants_, [](const ScopedGrant& grant) { return grant.kind == "entity"; });
        if (scoped_grants_.size() != size) policy_audit("session.entity_revoked", true);
    }
    // Traces exist to reproduce state changes, so read-only queries are not recorded. The editor
    // polls scene.list, logs.read and friends continuously; tracing those would bury the operations
    // that actually changed the scene and make trace files grow with idle time rather than work.
    if (!method.starts_with("trace.") && !read_only) {
        engine_.record_trace_event("command", std::string(request));
    }

    if (method.starts_with("editor.camera.")) {
        if (!editor_camera_handler_) return error_response(id, "editor inspection camera requires a live editor");
        return editor_camera_handler_(request);
    }

    if (method == "runtime.status") {
        const auto status = engine_.status();
        std::ostringstream result;
        result << response_prefix(id) << "{\"running\":" << (status.running ? "true" : "false")
               << ",\"paused\":" << (status.paused ? "true" : "false")
               << ",\"mode\":\"" << (status.mode == RuntimeMode::editor ? "editor" : "game") << '"'
               << ",\"frame\":" << status.frame_index
               << ",\"elapsed_seconds\":" << status.elapsed_seconds
               << ",\"fixed_delta_seconds\":" << engine_.fixed_delta_seconds()
               << ",\"random_seed\":" << engine_.random_seed()
               << ",\"width\":" << status.width << ",\"height\":" << status.height << "}}";
        return result.str();
    }
    if (method == "runtime.play") {
        if (!engine_.run_game()) return error_response(id, engine_.run_game_error());
        return response_prefix(id) + "{\"mode\":\"game\",\"paused\":false}}";
    }
    if (method == "runtime.stop") {
        if (!engine_.stop_game()) return error_response(id, "no editor game session is running");
        const auto size = scoped_grants_.size();
        std::erase_if(scoped_grants_, [](const ScopedGrant& grant) { return grant.kind == "entity"; });
        if (scoped_grants_.size() != size) policy_audit("session.entity_revoked", true);
        return response_prefix(id) + "{\"mode\":\"editor\",\"paused\":false}}";
    }
    if (method == "scripts.status")
        return response_prefix(id) + script_status_json(engine_.scripts().status()) + "}";
    if (method == "scripts.build") {
        std::string error;
        if (!engine_.scripts().start_build(error)) return error_response(id, error);
        return response_prefix(id) + script_status_json(engine_.scripts().status()) + "}";
    }
    if (method == "scripts.trust") {
        std::string error;
        if (!engine_.scripts().set_trusted(boolean_field(request, "trusted", false), error))
            return error_response(id, error);
        return response_prefix(id) + script_status_json(engine_.scripts().status()) + "}";
    }
    if (method == "scripts.sdk") {
        const auto directory = script_sdk_directory();
        std::ifstream input(directory / "relay_script.hpp", std::ios::binary);
        std::ostringstream text;
        text << input.rdbuf();
        if (!input) return error_response(id, "the script SDK was not found at " + directory.string());
        return response_prefix(id) + "{\"path\":\"relay_script.hpp\",\"source\":\"" +
               escape_json(text.str()) + "\"}}";
    }
    if (method == "scripts.read" || method == "scripts.write" || method == "scripts.create") {
        if (!engine_.project()) return error_response(id, "scripts need an open project");
        const auto root = engine_.project()->root();
        std::string error;
        if (method == "scripts.read") {
            const auto path = string_field(request, "path");
            const auto source = read_script_source(root, path, error);
            if (!source) return error_response(id, error);
            return response_prefix(id) + "{\"path\":\"" + escape_json(path) + "\",\"source\":\"" +
                   escape_json(*source) + "\"}}";
        }
        const bool create = method == "scripts.create";
        const auto behaviour = string_field(request, "behaviour");
        if (create && !valid_behaviour_name(behaviour))
            return error_response(id, "behaviour must be a C++ identifier");
        const auto path = create ? behaviour + ".cpp" : string_field(request, "path");
        if (!write_script_source(root, path, create ? script_template(behaviour)
                                                     : string_field(request, "source"),
                                 create, error))
            return error_response(id, error);
        engine_.logs().write(LogLevel::info, (create ? "Created script " : "Wrote script ") +
                                                 std::string{script_source_directory} + "/" + path);
        return response_prefix(id) + "{\"path\":\"" + escape_json(path) + "\"}}";
    }
    if (method == "runtime.pause") {
        if (engine_.status().mode != RuntimeMode::game)
            return error_response(id, "run the game before pausing simulation");
        engine_.pause();
        return response_prefix(id) + "{\"paused\":true}}";
    }
    if (method == "runtime.resume") {
        if (engine_.status().mode != RuntimeMode::game)
            return error_response(id, "run the game before resuming simulation");
        engine_.resume();
        return response_prefix(id) + "{\"paused\":false}}";
    }
    if (method == "runtime.step") {
        if (engine_.status().mode != RuntimeMode::game)
            return error_response(id, "run the game before stepping simulation");
        const auto requested_frames = unsigned_field(request, "frames", 1);
        const auto frames = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(requested_frames, 1, 10000));
        engine_.step(frames);
        return response_prefix(id) + "{\"frame\":" +
               std::to_string(engine_.status().frame_index) + "}}";
    }
    if (method == "runtime.quit") {
        engine_.request_shutdown();
        return response_prefix(id) + "{\"running\":false}}";
    }
    if (method == "render.capture") {
        const auto path_value = string_field(request, "path");
        const auto path = safe_capture_path(path_value.empty() ? "frame.bmp" : path_value);
        if (!path) return error_response(id, "path must be a safe .png or .bmp name without directories");
        std::string error;
        const auto source = string_field(request, "source");
        if (source == "vulkan" && !capture_handler_) return error_response(id, "Vulkan capture requires a live GPU renderer");
        const bool captured = capture_handler_ && source != "deterministic"
                                  ? capture_handler_(*path, error)
                                  : engine_.capture(*path, error);
        if (!captured) {
            return error_response(id, error);
        }
        return response_prefix(id) + "{\"path\":\"" + escape_json(path->generic_string()) +
               "\",\"source\":\"" + (capture_handler_ && source != "deterministic" ? "vulkan" : "deterministic") +
               "\"}}";
    }
    if (method == "render.capabilities") {
        const auto capabilities = probe_vulkan_capabilities();
        std::ostringstream result;
        result << response_prefix(id)
               << "{\"vulkan\":{\"loader_available\":"
               << (capabilities.loader_available ? "true" : "false")
               << ",\"device_available\":" << (capabilities.device_available ? "true" : "false")
               << ",\"logical_device_created\":"
               << (capabilities.logical_device_created ? "true" : "false")
               << ",\"api_version\":\"" << capabilities.api_major << '.'
               << capabilities.api_minor << '.' << capabilities.api_patch << "\""
               << ",\"device_name\":\"" << escape_json(capabilities.device_name) << "\""
               << ",\"device_type\":\"" << escape_json(capabilities.device_type) << "\""
               << ",\"ray_tracing_pipeline\":"
               << (capabilities.ray_tracing_pipeline ? "true" : "false")
               << ",\"ray_query\":" << (capabilities.ray_query ? "true" : "false")
               << ",\"mesh_shader\":" << (capabilities.mesh_shader ? "true" : "false")
               << ",\"descriptor_buffer\":" << (capabilities.descriptor_buffer ? "true" : "false")
               << ",\"error\":\"" << escape_json(capabilities.error)
               << "\"},\"shadows\":{\"directional_cascades\":3,\"spot_maps\":1,"
                  "\"point_maps\":1,\"pcf_kernel\":3,\"maximum_distance\":120,"
                  "\"budget_policy\":\"brightest_per_type\"}}}";
        return result.str();
    }
    if (method == "render.graph") {
        if (render_inspection_handler_) {
            const auto live = render_inspection_handler_("graph");
            if (!live.empty()) return response_prefix(id) + live + '}';
        }
        return response_prefix(id) + make_scene_render_graph().json() + '}';
    }
    if (method == "render.shader_interfaces") {
        if (render_inspection_handler_) {
            const auto live = render_inspection_handler_("shader_interfaces");
            if (!live.empty()) return response_prefix(id) + live + '}';
        }
        return response_prefix(id) +
               "{\"available\":false,\"reason\":\"no live Vulkan renderer is attached\"}}";
    }
    if (method == "render.upload_status") {
        if (render_inspection_handler_) {
            const auto live = render_inspection_handler_("upload_status");
            if (!live.empty()) return response_prefix(id) + live + '}';
        }
        return response_prefix(id) +
               "{\"available\":false,\"reason\":\"no live Vulkan renderer is attached\"}}";
    }
    if (method == "render.assets") {
        return response_prefix(id) + engine_.assets().to_json() + '}';
    }
    if (method == "assets.formats") {
        return response_prefix(id) + model_import_capabilities_json() + '}';
    }
    if (method == "assets.import_model") {
        const auto filename = string_field(request, "filename");
        const auto safe_name = safe_model_filename(filename);
        if (!safe_name) return error_response(id, "filename must be a safe project-relative model path");
        const bool instantiate = boolean_field(request, "instantiate", true);
        ModelImportSettings settings;
        settings.preset = string_field(request, "preset");
        if (settings.preset.empty()) settings.preset = "scene";
        const auto assets_root = engine_.project() ? engine_.project()->root() : std::filesystem::path{"assets"};
        if (!workspace_file(assets_root.generic_string(), filename, ""))
            return error_response(id, "model path must stay inside its project without symlinks");
        std::string error;
        ModelImportResult imported;
        if (instantiate) {
            const bool changed = engine_.scene_history().execute("Import " + filename, [&](Scene& scene) {
                imported = import_model_asset(assets_root, *safe_name, engine_.assets(), &scene,
                                              error, settings);
                return imported.imported;
            });
            if (!changed) return error_response(id, error.empty() ? "model import failed" : error);
        } else {
            imported = import_model_asset(assets_root, *safe_name, engine_.assets(), nullptr,
                                          error, settings);
            if (!imported.imported) return error_response(id, error);
        }
        ImportManifest manifest;
        std::string manifest_error;
        if (manifest.load(assets_root, manifest_error)) {
            ImportManifestEntry entry{
                *safe_name,      model_importer_version, imported.content_id, imported.dependencies,
                imported.meshes, imported.materials,     imported.textures,   imported.preset};
            if (const auto *model = engine_.assets().find_model(imported.model)) {
                entry.nodes = model->binding_layout;
                for (const auto &clip : model->clips)
                    entry.clips.push_back(clip.name);
            }
            manifest.record(std::move(entry));
            if (!manifest.save(assets_root, manifest_error)) {
                engine_.logs().write(LogLevel::warning,
                                     "Import manifest not updated: " + manifest_error);
            }
        } else {
            engine_.logs().write(LogLevel::warning,
                                 "Import manifest not updated: " + manifest_error);
        }
        engine_.logs().write(LogLevel::info, "Imported model " + filename + " as " + imported.content_id);
        return response_prefix(id) + imported.json() + '}';
    }
    if (method == "render.capture_async") {
        const auto path_value = string_field(request, "path");
        const auto path = safe_capture_path(path_value.empty() ? "frame.png" : path_value);
        if (!path) return error_response(id, "path must be a safe .png or .bmp name without directories");
        std::string error;
        const auto source = string_field(request, "source").empty() ? engine_.capture_source() : string_field(request, "source");
        const auto job = engine_.capture_async(*path, error, source);
        if (job == 0U) return error_response(id, error);
        return response_prefix(id) + "{\"job\":" + std::to_string(job) +
               ",\"path\":\"" + escape_json(path->generic_string()) + "\",\"source\":\"" + source + "\",\"state\":\"queued\"}}";
    }
    if (method == "render.capture_cancel") {
        const auto cancelled = engine_.cancel_capture(unsigned_field(request, "job", 0));
        return response_prefix(id) + "{\"cancelled\":" + (cancelled ? "true" : "false") + "}}";
    }
    if (method == "render.capture_status") {
        const auto job = unsigned_field(request, "job", 0);
        const auto status = engine_.capture_status(job);
        if (!status.error.empty() && status.path.empty()) return error_response(id, status.error);
        return response_prefix(id) + "{\"job\":" + std::to_string(job) +
               ",\"path\":\"" + escape_json(status.path.string()) + "\",\"state\":\"" +
               capture_state_name(status.state) + "\",\"source\":\"" + status.source + "\",\"error\":\"" +
               escape_json(status.error) + "\"}}";
    }
    if (method == "performance.read") {
        const auto after = unsigned_field(request, "after_frame", 0);
        const auto performance = engine_.performance(after);
        const auto limit = static_cast<std::size_t>(std::clamp<std::uint64_t>(
            unsigned_field(request, "limit", 30), 1U, 240U));
        const auto first_sample = performance.samples.size() > limit
                                      ? performance.samples.size() - limit : 0U;
        std::ostringstream result;
        result << response_prefix(id) << "{\"average_cpu_ms\":" << performance.average_cpu_ms
               << ",\"maximum_cpu_ms\":" << performance.maximum_cpu_ms
               << ",\"latest_gpu_ms\":" << performance.latest_gpu_ms
               << ",\"resident_memory_bytes\":" << performance.resident_memory_bytes
               << ",\"samples\":[";
        for (std::size_t index = first_sample; index < performance.samples.size(); ++index) {
            if (index != first_sample) result << ',';
            const auto& sample = performance.samples[index];
            result << "{\"frame\":" << sample.frame << ",\"cpu_frame_ms\":"
                   << sample.cpu_frame_ms << ",\"gpu_frame_ms\":" << sample.gpu_frame_ms
                   << ",\"draw_calls\":" << sample.draw_calls << ",\"render_resources\":"
                   << sample.render_resources << ",\"entities\":" << sample.entity_count << '}';
        }
        result << "]}}";
        return result.str();
    }
    if (method == "profiler.read") {
        ProfileQuery query;
        query.frames = static_cast<std::size_t>(unsigned_field(request, "frames", 120));
        query.frame = unsigned_field(request, "frame", 0);
        query.game_only = boolean_field(request, "game_only", false);
        query.history = static_cast<std::size_t>(unsigned_field(request, "history", 0));
        return response_prefix(id) + profile_report_json(profiler().report(query)) + '}';
    }
    if (method == "profiler.set") {
        JsonParser parser(request);
        const auto parsed = parser.parse();
        if (const auto* paused = field(*parsed->object(), "paused"); paused && paused->boolean())
            profiler().set_paused(*paused->boolean());
        if (boolean_field(request, "clear", false)) profiler().clear();
        return response_prefix(id) + "{\"paused\":" + (profiler().paused() ? "true" : "false") + "}}";
    }
    if (method == "graphics.settings") {
        const bool saved = engine_.project() && engine_.project()->graphics.has_value();
        std::string renderer = "null";
        if (render_inspection_handler_) {
            const auto live = render_inspection_handler_("lighting");
            if (!live.empty()) renderer = live;
        }
        return response_prefix(id) + "{\"settings\":" +
               graphics_settings_json(engine_.graphics_settings()) +
               ",\"saved\":" + (saved ? "true" : "false") +
               ",\"defaults\":" + graphics_settings_json(GraphicsSettings{}) +
               ",\"renderer\":" + renderer + "}}";
    }
    if (method == "graphics.set_settings") {
        auto settings = engine_.graphics_settings();
        settings.global_illumination =
            boolean_field(request, "global_illumination", settings.global_illumination);
        settings.reflections = boolean_field(request, "reflections", settings.reflections);
        settings.vsync = boolean_field(request, "vsync", settings.vsync);
        settings.frame_rate_limit = static_cast<std::uint32_t>(
            unsigned_field(request, "frame_rate_limit", settings.frame_rate_limit));
        std::string error;
        if (!engine_.set_graphics_settings(settings, error)) return error_response(id, error);
        engine_.logs().write(LogLevel::info, "Graphics settings saved");
        return response_prefix(id) + "{\"settings\":" + graphics_settings_json(settings) + "}}";
    }
    if (method == "audio.settings") {
        const bool saved = engine_.project() && engine_.project()->audio.has_value();
        return response_prefix(id) + "{\"settings\":" +
               audio_settings_json(engine_.audio_settings()) +
               ",\"saved\":" + (saved ? "true" : "false") +
               ",\"previewing\":" + (engine_.previewing_audio_settings() ? "true" : "false") +
               ",\"defaults\":" + audio_settings_json(default_audio_settings()) + "}}";
    }
    if (method == "audio.set_bus" || method == "audio.remove_bus") {
        auto settings = engine_.audio_settings();
        const auto name = string_field(request, "name");
        auto& buses = settings.buses;
        auto found = std::find_if(buses.begin(), buses.end(),
                                  [&](const AudioBus& bus) { return bus.name == name; });
        if (method == "audio.remove_bus") {
            if (found == buses.end()) return error_response(id, "no bus is named " + name);
            if (name == master_audio_bus) return error_response(id, "Master cannot be removed");
            const auto parent = found->parent;
            for (auto& bus : buses)
                if (bus.parent == name) bus.parent = parent;
            buses.erase(found);
        } else {
            if (found == buses.end()) {
                buses.push_back({name, std::string(master_audio_bus), 0.0, false, false, {}});
                found = std::prev(buses.end());
            }
            auto& bus = *found;
            if (const auto parent = optional_string_field(request, "parent")) {
                if (bus.name == master_audio_bus) return error_response(id, "Master cannot have a parent");
                bus.parent = *parent;
            }
            if (const auto volume = number_field(request, "volume_db")) bus.volume_db = *volume;
            bus.mute = boolean_field(request, "mute", bus.mute);
            bus.solo = boolean_field(request, "solo", bus.solo);
            if (const auto renamed = optional_string_field(request, "new_name");
                renamed && *renamed != bus.name) {
                if (bus.name == master_audio_bus) return error_response(id, "Master cannot be renamed");
                const auto old_name = bus.name;
                for (auto& other : buses)
                    if (other.parent == old_name) other.parent = *renamed;
                found->name = *renamed;
            }
        }
        std::string error;
        const bool preview = method == "audio.set_bus" && boolean_field(request, "preview", false);
        if (!(preview ? engine_.preview_audio_settings(settings, error)
                      : engine_.set_audio_settings(settings, error)))
            return error_response(id, error);
        return response_prefix(id) + "{\"settings\":" +
               audio_settings_json(engine_.audio_settings()) + "}}";
    }
    if (method == "audio.set_effect" || method == "audio.remove_effect" ||
        method == "audio.move_effect") {
        auto settings = engine_.audio_settings();
        const auto name = string_field(request, "bus");
        const auto bus = std::find_if(settings.buses.begin(), settings.buses.end(),
                                      [&](const AudioBus& item) { return item.name == name; });
        if (bus == settings.buses.end()) return error_response(id, "no bus is named " + name);
        auto& effects = bus->effects;
        const auto index = number_field(request, "index");
        const auto position = index ? static_cast<std::size_t>(*index) : effects.size();
        if (index && position >= effects.size())
            return error_response(id, "bus " + name + " has no effect " + std::to_string(position));
        std::size_t changed = position;
        if (method == "audio.remove_effect") {
            effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(position));
        } else if (method == "audio.move_effect") {
            const auto to = std::min(static_cast<std::size_t>(unsigned_field(request, "to", 0U)),
                                     effects.size() - 1U);
            auto moved = effects[position];
            effects.erase(effects.begin() + static_cast<std::ptrdiff_t>(position));
            effects.insert(effects.begin() + static_cast<std::ptrdiff_t>(to), moved);
            changed = to;
        } else {
            const auto type_name = optional_string_field(request, "type");
            const auto type = type_name ? audio_effect_type_from_name(*type_name) : std::nullopt;
            if (type_name && !type) return error_response(id, "unknown effect type " + *type_name);
            if (!index) {
                if (!type) return error_response(id, "a new effect needs a type");
                if (effects.size() >= maximum_bus_effects)
                    return error_response(id, "a bus holds at most 8 effects");
                AudioEffect effect;
                effect.type = *type;
                effects.push_back(effect);
            } else if (type) {
                effects[position].type = *type;
            }
            auto& effect = effects[changed];
            effect.enabled = boolean_field(request, "enabled", effect.enabled);
            for (const char* parameter :
                 {"mix", "room_size", "damping", "width", "pre_delay_ms", "time_ms", "feedback",
                  "low_db", "mid_db", "mid_frequency", "high_db", "threshold_db", "ratio",
                  "attack_ms", "release_ms", "makeup_db", "ceiling_db", "cutoff_hz", "resonance"})
                if (const auto value = number_field(request, parameter))
                    *audio_effect_parameter(effect, parameter) = *value;
        }
        std::string error;
        const bool preview = boolean_field(request, "preview", false);
        if (!(preview ? engine_.preview_audio_settings(settings, error)
                      : engine_.set_audio_settings(settings, error)))
            return error_response(id, error);
        return response_prefix(id) + "{\"bus\":\"" + escape_json(name) +
               "\",\"index\":" + std::to_string(changed) + ",\"settings\":" +
               audio_settings_json(engine_.audio_settings()) + "}}";
    }
    if (method == "audio.debug_shapes") {
        return response_prefix(id) + AudioSystem::debug_shapes_json(engine_.scene()) + '}';
    }
    if (method == "audio.status") {
        return response_prefix(id) + engine_.audio().status_json(engine_.scene()) + '}';
    }
    if (method == "audio.play") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::string error;
        const bool game = engine_.status().mode == RuntimeMode::game;
        if (!engine_.audio().play(engine_.scene(), *entity, game, error))
            return error_response(id, error);
        return response_prefix(id) + "{\"entity\":\"" + entity->to_string() +
               "\",\"playing\":true,\"preview\":" + (game ? "false" : "true") + "}}";
    }
    if (method == "audio.stop") {
        const auto handle = optional_string_field(request, "entity");
        if (const auto sound = number_field(request, "sound"); sound && !handle) {
            const bool was_playing = engine_.audio().stop_sound(static_cast<std::uint64_t>(*sound));
            return response_prefix(id) + "{\"sound\":" + std::to_string(static_cast<std::uint64_t>(*sound)) +
                   ",\"was_playing\":" + (was_playing ? "true" : "false") + "}}";
        }
        if (!handle) {
            engine_.audio().stop_all();
            return response_prefix(id) + "{\"stopped\":\"all\"}}";
        }
        const auto entity = Entity::parse(*handle);
        if (!entity) return error_response(id, "invalid entity handle");
        const bool was_playing = engine_.audio().stop(*entity);
        return response_prefix(id) + "{\"entity\":\"" + entity->to_string() +
               "\",\"was_playing\":" + (was_playing ? "true" : "false") + "}}";
    }
    if (method == "audio.play_clip") {
        AudioOneShot options;
        const auto x = number_field(request, "x"), y = number_field(request, "y"),
                   z = number_field(request, "z");
        if (x || y || z) options.position = Vec3{x.value_or(0.0), y.value_or(0.0), z.value_or(0.0)};
        options.volume_db = number_field(request, "volume_db").value_or(0.0);
        options.pitch = number_field(request, "pitch").value_or(1.0);
        options.bus = optional_string_field(request, "bus").value_or("SFX");
        options.min_distance = number_field(request, "min_distance").value_or(1.0);
        options.max_distance = number_field(request, "max_distance").value_or(50.0);
        std::string error;
        const auto sound = engine_.audio().play_clip(engine_.scene(), string_field(request, "clip"),
                                                     options, error);
        if (sound == 0U) return error_response(id, error);
        return response_prefix(id) + "{\"sound\":" + std::to_string(sound) + "}}";
    }
    if (method == "audio.music") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto action = string_field(request, "action");
        if (action == "stop") {
            if (!engine_.audio().music_stop(*entity, number_field(request, "fade_seconds").value_or(1.0)))
                return error_response(id, "the music player is not playing");
            return response_prefix(id) + "{\"entity\":\"" + entity->to_string() + "\",\"track\":-1}}";
        }
        std::optional<MusicPlayer::Sync> sync;
        if (const auto name = optional_string_field(request, "sync")) sync = music_sync_from_name(*name);
        const auto track = action == "play" && number_field(request, "track")
                               ? static_cast<int>(*number_field(request, "track"))
                               : -1;
        std::string error;
        if (!engine_.audio().music_play(engine_.scene(), *entity, track, sync, error))
            return error_response(id, error);
        return response_prefix(id) + "{\"entity\":\"" + entity->to_string() + "\",\"track\":" +
               std::to_string(engine_.audio().music_track(*entity)) + "}}";
    }
    if (method == "audio.set_spatialization") {
        auto settings = engine_.audio_settings();
        settings.spatialization = string_field(request, "mode") == "binaural"
                                      ? AudioSettings::Spatialization::binaural
                                      : AudioSettings::Spatialization::stereo;
        std::string error;
        if (!engine_.set_audio_settings(settings, error)) return error_response(id, error);
        return response_prefix(id) + "{\"settings\":" + audio_settings_json(engine_.audio_settings()) + "}}";
    }
    if (method == "audio.clip") {
        std::string error;
        const auto peaks = static_cast<std::size_t>(unsigned_field(request, "peaks", 0U));
        const auto clip = engine_.audio().clip_json(string_field(request, "clip"), peaks, error);
        if (!clip) return error_response(id, error);
        return response_prefix(id) + *clip + '}';
    }
    if (method == "input.map") {
        const bool saved = engine_.project() && engine_.project()->input.has_value();
        return response_prefix(id) + "{\"map\":" + input_map_json(engine_.input().map()) +
               ",\"saved\":" + (saved ? "true" : "false") + ",\"path\":\"" +
               (engine_.project() ? json_escape(engine_.project()->filename) : std::string{}) +
               "\",\"defaults\":" +
               input_map_json(default_input_map()) + "}}";
    }
    if (method == "input.set_map") {
        std::string error;
        auto map = parse_input_map(string_field(request, "map"), error);
        if (!map || !engine_.set_input_map(std::move(*map), error)) return error_response(id, error);
        engine_.logs().write(LogLevel::info, "Input map saved");
        return response_prefix(id) + "{\"map\":" + input_map_json(engine_.input().map()) + "}}";
    }
    if (method == "input.state")
        return response_prefix(id) + engine_.input().state_json() + "}";
    if (method == "input.simulate") {
        if (engine_.status().mode != RuntimeMode::game)
            return error_response(id, "run the game before simulating input");
        const auto name = string_field(request, "name");
        const auto frames = static_cast<std::uint32_t>(unsigned_field(request, "frames", 1));
        if (!engine_.input().simulate(name, number_field(request, "value").value_or(1.0), frames))
            return error_response(id, "no action or axis named " + name + " in the input map");
        return response_prefix(id) + "{\"name\":\"" + escape_json(name) + "\",\"frames\":" +
               std::to_string(frames) + "}}";
    }
    if (method == "input.release") {
        engine_.apply_input_event("input:reset");
        return response_prefix(id) + "{\"released\":true}}";
    }
    if (method == "input.recent") {
        const auto& inputs = engine_.recent_input_events();
        std::ostringstream result;
        result << response_prefix(id) << "{\"events\":[";
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            if (index != 0U) result << ',';
            result << '"' << escape_json(inputs[index]) << '"';
        }
        result << "]}}";
        return result.str();
    }
    if (method == "video.start") {
        const auto filename = string_field(request, "filename");
        const auto path = safe_video_path(filename);
        if (!path) return error_response(id, "filename must be a safe .webm name without directories");
        const auto fps = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            unsigned_field(request, "fps", 30), 1U, 60U));
        const auto maximum_frames = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(
            unsigned_field(request, "maximum_frames", 300), 1U, 3600U));
        std::string error;
        const auto source = string_field(request, "source").empty() ? engine_.capture_source() : string_field(request, "source");
        if (!engine_.start_video(*path, fps, maximum_frames, error, source)) return error_response(id, error);
        return response_prefix(id) + "{\"recording\":true,\"path\":\"" +
               escape_json(path->string()) + "\",\"source\":\"" + source + "\",\"fps\":" + std::to_string(fps) +
               ",\"maximum_frames\":" + std::to_string(maximum_frames) + "}}";
    }
    if (method == "video.capabilities") {
        return response_prefix(id) + "{\"webm\":" +
               (video_encoder_available() ? "true" : "false") +
               ",\"encoder\":\"ffmpeg/libvpx-vp9\"}}";
    }
    if (method == "video.stop") {
        std::string error;
        if (!engine_.stop_video(error)) return error_response(id, error);
        const auto status = engine_.video_status();
        return response_prefix(id) + "{\"recording\":false,\"path\":\"" +
               escape_json(status.path.string()) + "\",\"frames\":" +
               std::to_string(status.submitted_frames) + ",\"dropped_frames\":" +
               std::to_string(status.dropped_frames) + ",\"source\":\"" + status.source + "\",\"finalizing\":" + (status.finalizing ? "true" : "false") + ",\"error\":\"" + escape_json(status.error) + "\"}}";
    }
    if (method == "video.status") {
        const auto status = engine_.video_status();
        return response_prefix(id) + "{\"recording\":" +
               (status.recording ? "true" : "false") + ",\"path\":\"" +
               escape_json(status.path.string()) + "\",\"fps\":" + std::to_string(status.fps) +
               ",\"frames\":" + std::to_string(status.submitted_frames) +
               ",\"dropped_frames\":" + std::to_string(status.dropped_frames) + ",\"source\":\"" + status.source + "\",\"finalizing\":" + (status.finalizing ? "true" : "false") + ",\"error\":\"" + escape_json(status.error) + "\"}}";
    }
    if (method == "scene.list") {
        return response_prefix(id) + engine_.scene().list_json() + '}';
    }
    if (method == "scene.inspect") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid() || !engine_.scene().contains(*entity)) {
            return error_response(id, "entity does not exist or has a stale handle");
        }
        return response_prefix(id) + engine_.scene().entity_json(*entity) + '}';
    }
    if (method == "scene.rename") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid()) return error_response(id, "invalid entity handle");
        auto name = string_field(request, "name");
        if (name.empty()) return error_response(id, "name must not be empty");
        if (!engine_.scene_history().execute("Rename " + entity->to_string(),
                                             [&](Scene& scene) {
                                                 return scene.set_name(*entity, name);
                                             })) {
            return error_response(id, "entity does not exist or has a stale handle");
        }
        engine_.logs().write(LogLevel::info, "Renamed " + entity->to_string() + " to " + name);
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.history") {
        const auto& history = engine_.scene_history();
        const auto undo = history.undo_labels();
        const auto redo = history.redo_labels();
        std::string output = "{\"undo\":[";
        for (std::size_t index = 0; index < undo.size(); ++index) {
            if (index != 0) output += ',';
            output += '"' + escape_json(undo[index]) + '"';
        }
        output += "],\"redo\":[";
        for (std::size_t index = 0; index < redo.size(); ++index) {
            if (index != 0) output += ',';
            output += '"' + escape_json(redo[index]) + '"';
        }
        output += "],\"state\":" + history_json(history) + '}';
        return response_prefix(id) + output + '}';
    }
    if (method == "assets.browse" || method == "assets.search" || method == "assets.create_folder" ||
        method == "assets.move" || method == "assets.delete") {
        const auto root = engine_.project() ? engine_.project()->root() : std::filesystem::path{"assets"};
        // The project file and member scenes are owned by the project, not the asset browser.
        const auto protected_path = [&](const std::string& path) {
            if (!engine_.project()) return false;
            const auto project_file =
                std::filesystem::path(engine_.project()->filename).filename().generic_string();
            if (path == project_file) return true;
            for (const auto& scene : engine_.project()->scenes) {
                const auto member = "scenes/" + scene;
                if (member == path || member.starts_with(path + '/')) return true;
            }
            return false;
        };
        std::string error;
        const auto entries_json = [&](const AssetDirectoryListing& listing) {
            std::string output = "\"entries\":[";
            for (std::size_t index = 0; index < listing.entries.size(); ++index) {
                const auto& entry = listing.entries[index];
                if (index) output += ',';
                output += "{\"name\":\"" + escape_json(entry.name) + "\",\"path\":\"" +
                          escape_json(entry.path) + "\",\"type\":\"" +
                          (entry.directory ? "folder" : "file") + "\",\"kind\":\"" +
                          std::string(asset_kind_name(entry.kind)) + "\",\"size\":" +
                          std::to_string(entry.size) + ",\"importable\":" +
                          (!entry.directory && safe_model_filename(entry.path) ? "true" : "false") +
                          ",\"protected\":" + (protected_path(entry.path) ? "true" : "false") + '}';
            }
            return output + "],\"truncated\":" + (listing.truncated ? "true" : "false");
        };
        if (method == "assets.browse") {
            const auto directory = string_field(request, "directory");
            const auto listing = list_asset_directory(root, directory, error);
            if (!listing) return error_response(id, error);
            return response_prefix(id) + "{\"root\":\"" + escape_json(root.generic_string()) +
                   "\",\"directory\":\"" + escape_json(directory) + "\"," + entries_json(*listing) +
                   "}}";
        }
        if (method == "assets.search") {
            std::vector<AssetKind> kinds;
            JsonParser parser(request);
            const auto parsed = parser.parse();
            if (const auto* list = parsed && parsed->object() ? field(*parsed->object(), "kinds") : nullptr;
                list && list->array())
                for (const auto& value : *list->array())
                    if (const auto kind = value.string() ? asset_kind_from_name(*value.string())
                                                         : std::nullopt)
                        kinds.push_back(*kind);
            const auto query = string_field(request, "query");
            const auto listing = search_assets(root, query, kinds);
            return response_prefix(id) + "{\"root\":\"" + escape_json(root.generic_string()) +
                   "\",\"query\":\"" + escape_json(query) + "\"," + entries_json(listing) + "}}";
        }
        if (method == "assets.create_folder") {
            const auto path = string_field(request, "path");
            if (!create_asset_folder(root, path, error)) return error_response(id, error);
            engine_.logs().write(LogLevel::info, "Created folder " + path);
            return response_prefix(id) + "{\"path\":\"" + escape_json(path) + "\"}}";
        }
        if (method == "assets.move") {
            const auto from = string_field(request, "from");
            const auto to = string_field(request, "to");
            if (protected_path(from))
                return error_response(id, "project files and member scenes cannot be moved");
            if (!move_asset(root, from, to, error)) return error_response(id, error);
            ImportManifest manifest;
            std::string manifest_error;
            if (manifest.load(root, manifest_error)) {
                if (manifest.move_sources(from, to) && !manifest.save(root, manifest_error))
                    engine_.logs().write(LogLevel::warning,
                                         "Import manifest not updated: " + manifest_error);
            } else {
                engine_.logs().write(LogLevel::warning, "Import manifest not updated: " + manifest_error);
            }
            engine_.logs().write(LogLevel::info, "Moved " + from + " to " + to);
            return response_prefix(id) + "{\"from\":\"" + escape_json(from) + "\",\"to\":\"" +
                   escape_json(to) + "\"}}";
        }
        const auto path = string_field(request, "path");
        if (protected_path(path))
            return error_response(id, "project files and member scenes cannot be deleted");
        const auto trashed = delete_asset(root, path, error);
        if (!trashed) return error_response(id, error);
        engine_.logs().write(LogLevel::info, "Moved " + path + " to " + *trashed);
        return response_prefix(id) + "{\"path\":\"" + escape_json(path) + "\",\"trash\":\"" +
               escape_json(*trashed) + "\"}}";
    }
    if (method == "assets.available") {
        std::vector<std::string> names, files;
        std::error_code failure;
        const auto root = engine_.project() ? engine_.project()->root() : std::filesystem::path{"assets"};
        std::filesystem::recursive_directory_iterator item{root, failure}, end;
        for (; !failure && item != end && files.size() < 4096; item.increment(failure)) {
            const auto basename = item->path().filename().string();
            if (item->is_symlink() || basename.starts_with(".")) {
                if (item->is_directory()) item.disable_recursion_pending();
                continue;
            }
            std::error_code status_error;
            if (!item->is_regular_file(status_error)) continue;
            const auto name = item->path().lexically_relative(root).generic_string();
            if (!workspace_file(root.generic_string(), name, "")) continue;
            files.push_back(name);
            if (safe_model_filename(name)) names.push_back(name);
        }
        std::sort(files.begin(), files.end());
        std::sort(names.begin(), names.end());
        std::string output = "{\"available\":" + std::string(failure ? "false" : "true") + ",\"models\":[";
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (index != 0) output += ',';
            output += '"' + escape_json(names[index]) + '"';
        }
        output += "],\"root\":\"" + escape_json(root.generic_string()) + "\",\"files\":[";
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (i) output += ',';
            output += '\"' + escape_json(files[i]) + '\"';
        }
        return response_prefix(id) + output + "]}}";
    }
    if (method == "physics.raycast") {
        const auto number = [&](const char* key) { return number_field(request, key).value_or(0.0); };
        const Vec3 origin{number("origin_x"), number("origin_y"), number("origin_z")};
        const Vec3 direction{number("direction_x"), number("direction_y"),
                             number("direction_z")};
        const auto result = collision_raycast(
            engine_.scene(), origin, direction,
            number_field(request, "maximum_distance").value_or(1'000'000.0),
            static_cast<std::uint32_t>(unsigned_field(request, "layer_mask", 0xffffffffU)),
            &engine_.assets());
        if (!result.error.empty()) return error_response(id, result.error);
        if (!result.hit) return response_prefix(id) + "{\"entity\":null,\"distance\":null,\"point\":null,\"normal\":null}}";
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
               << "{\"entity\":\"" << result.entity.to_string() << "\",\"distance\":"
               << result.distance << ",\"point\":[" << result.point.x << ',' << result.point.y
               << ',' << result.point.z << "],\"normal\":[" << result.normal.x << ','
               << result.normal.y << ',' << result.normal.z << "]}";
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "physics.debug_boxes") {
        const auto result = collision_debug_boxes(engine_.scene(), false, &engine_.assets());
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
               << "{\"boxes\":[";
        const auto vector = [&](const Vec3 value) {
            output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
        };
        for (std::size_t index = 0; index < result.boxes.size(); ++index) {
            const auto& box = result.boxes[index];
            if (index) output << ',';
            constexpr std::array<const char*, 5> shapes{"box", "sphere", "capsule", "convex", "mesh"};
            output << "{\"entity\":\"" << box.entity.to_string() << "\",\"enabled\":"
                   << (box.enabled ? "true" : "false") << ",\"type\":\""
                   << shapes[static_cast<std::size_t>(box.type)] << "\",\"center\":";
            vector(box.center);
            output << ",\"edges\":[";
            for (std::size_t edge = 0; edge < 3; ++edge) {
                if (edge) output << ',';
                vector(box.edges[edge]);
            }
            output << "],\"radius\":" << box.radius << ",\"axis\":";
            vector(box.axis);
            // Outline vertices come from single-precision meshes; seven digits keep them exact.
            output << std::setprecision(7) << ",\"lines\":[";
            for (std::size_t line = 0; line < box.lines.size(); ++line) {
                if (line) output << ',';
                vector(box.lines[line][0]);
                output << ',';
                vector(box.lines[line][1]);
            }
            output << std::setprecision(std::numeric_limits<double>::max_digits10)
                   << "],\"lines_truncated\":" << (box.lines_truncated ? "true" : "false") << '}';
        }
        output << "],\"truncated\":" << (result.truncated ? "true" : "false") << ",\"joints\":[";
        for (std::size_t index = 0; index < result.joints.size(); ++index) {
            const auto& joint = result.joints[index];
            output << (index ? "," : "") << "{\"entity\":\"" << joint.entity.to_string()
                   << "\",\"enabled\":" << (joint.enabled ? "true" : "false") << ",\"type\":\""
                   << joint_type_name(joint.type) << "\",\"anchor\":";
            vector(joint.anchor);
            output << ",\"axis\":";
            vector(joint.axis);
            output << ",\"partner\":";
            if (joint.has_partner) vector(joint.partner);
            else output << "null";
            output << '}';
        }
        output << "]}";
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "physics.body_status") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto* record = engine_.scene().get(*entity);
        if (!record->physics_body) return error_response(id, "entity has no physics body");
        const auto velocity = engine_.status().mode == RuntimeMode::game
            ? engine_.physics().velocity(engine_.scene(), *entity) : std::nullopt;
        const auto angular_velocity = engine_.status().mode == RuntimeMode::game
            ? engine_.physics().angular_velocity(engine_.scene(), *entity) : std::nullopt;
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
               << "{\"type\":\""
               << (record->physics_body->type == PhysicsBody::Type::dynamic ? "dynamic" : "static")
               << "\",\"velocity\":";
        if (velocity)
            output << '[' << velocity->x << ',' << velocity->y << ',' << velocity->z << ']';
        else output << "null";
        output << ",\"angular_velocity\":";
        if (angular_velocity)
            output << '[' << angular_velocity->x << ',' << angular_velocity->y << ','
                   << angular_velocity->z << ']';
        else output << "null";
        output << '}';
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "physics.contact_events") {
        const auto events = engine_.physics().contact_events(unsigned_field(request, "after", 0));
        std::ostringstream output;
        output << "{\"latest_sequence\":" << events.latest_sequence
               << ",\"oldest_sequence\":" << events.oldest_sequence << ",\"events\":[";
        for (std::size_t index = 0; index < events.events.size(); ++index) {
            const auto& event = events.events[index];
            if (index) output << ',';
            output << "{\"sequence\":" << event.sequence << ",\"type\":\""
                   << (event.began ? "begin" : "end") << "\",\"first\":\""
                   << event.first.to_string() << "\",\"second\":\""
                   << event.second.to_string() << "\"}";
        }
        output << "]}";
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "physics.apply_impulse") {
        if (engine_.status().mode != RuntimeMode::game)
            return error_response(id, "game is not running");
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity) return error_response(id, "invalid body entity");
        const auto x = number_field(request, "impulse_x");
        const auto y = number_field(request, "impulse_y");
        const auto z = number_field(request, "impulse_z");
        if (!x || !y || !z) return error_response(id, "invalid impulse");
        std::optional<Vec3> point;
        const auto px = number_field(request, "point_x");
        const auto py = number_field(request, "point_y");
        const auto pz = number_field(request, "point_z");
        if (px || py || pz) {
            if (!px || !py || !pz) return error_response(id, "incomplete impulse point");
            point = Vec3{*px, *py, *pz};
        }
        if (!engine_.physics().apply_impulse(engine_.scene(), *entity, {*x, *y, *z}, point))
            return error_response(id, "entity has no dynamic physics body");
        return response_prefix(id) + "{\"applied\":true}}";
    }
    if (method == "physics.overlaps") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity) return error_response(id, "invalid collider entity");
        const auto result = collision_overlaps(engine_.scene(), *entity, &engine_.assets());
        if (!result.error.empty()) return error_response(id, result.error);
        std::string output = "{\"entities\":[";
        for (const auto hit : result.entities) {
            if (output.back() != '[') output += ',';
            output += '"' + hit.to_string() + '"';
        }
        output += "],\"truncated\":";
        output += result.truncated ? "true}" : "false}";
        return response_prefix(id) + output + '}';
    }
    if (method == "scene.pick") {
        const auto component = [&](const std::string_view key) {
            const auto value = number_field(request, key);
            return value.value_or(0.0);
        };
        const Vec3 origin{component("origin_x"), component("origin_y"), component("origin_z")};
        const Vec3 direction{component("direction_x"), component("direction_y"),
                             component("direction_z")};
        if (direction.x == 0.0 && direction.y == 0.0 && direction.z == 0.0) {
            return error_response(id, "pick direction must be nonzero");
        }
        const auto pick = pick_scene_entity(engine_.scene(), engine_.assets(), origin, direction);
        if (!pick.error.empty()) return error_response(id, pick.error);
        if (!pick.hit) {
            return response_prefix(id) + "{\"entity\":null,\"distance\":null}}";
        }
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10);
        output << "{\"entity\":\"" << pick.entity.to_string() << "\",\"distance\":" << pick.distance
               << '}';
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "scene.bounds") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid() || !engine_.scene().contains(*entity)) {
            return error_response(id, "entity does not exist or has a stale handle");
        }
        const auto bounds = compute_scene_bounds(engine_.scene(), engine_.assets(), *entity);
        if (!bounds.valid) return error_response(id, "scene exceeds the deformation query budget");
        std::ostringstream output;
        output << std::setprecision(std::numeric_limits<double>::max_digits10);
        const auto vector = [&output](const Vec3& value) {
            output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
        };
        output << "{\"has_geometry\":" << (bounds.has_geometry ? "true" : "false")
               << ",\"position\":";
        vector(bounds.position);
        output << ",\"minimum\":";
        vector(bounds.minimum);
        output << ",\"maximum\":";
        vector(bounds.maximum);
        output << '}';
        return response_prefix(id) + output.str() + '}';
    }
    if (method == "scene.create") {
        const auto type = string_field(request, "type");
        const auto* type_info = type.empty() ? nullptr : find_node_type(type);
        auto name = string_field(request, "name");
        if (name.empty()) name = type_info && type != "Node" ? std::string{type_info->name} : "Entity";
        const auto parent = entity_field(request, "parent");
        if (!parent.has_value() || (parent->valid() && !engine_.scene().contains(*parent))) {
            return error_response(id, "parent does not exist or has a stale handle");
        }
        Entity created{};
        std::string error;
        const bool changed = engine_.scene_history().execute("Create " + name, [&](Scene& scene) {
            created = scene.create(name, *parent);
            return created.valid() &&
                   (type.empty() || apply_node_type(scene, created, type, error));
        });
        if (!changed) return error_response(id, error.empty() ? "could not create entity" : error);
        engine_.logs().write(LogLevel::info, "Created entity " + created.to_string() + " (" + name + ')');
        return response_prefix(id) + "{\"entity\":\"" + created.to_string() + "\",\"history\":" +
               history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.destroy") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid()) return error_response(id, "invalid entity handle");
        const auto label = "Destroy " + entity->to_string();
        if (!engine_.scene_history().execute(label, [&](Scene& scene) { return scene.destroy(*entity); })) {
            return error_response(id, "entity does not exist or has a stale handle");
        }
        engine_.logs().write(LogLevel::info, "Destroyed entity " + entity->to_string());
        return response_prefix(id) + "{\"destroyed\":\"" + entity->to_string() +
               "\",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "project.status") {
        return response_prefix(id) + "{\"project\":" +
               (engine_.project() ? engine_.project()->json(false) : "null") + "}}";
    }
    if (method == "project.list") {
        std::string files = "[";
        for (const auto& file : available_projects()) {
            if (files.size() > 1) files += ',';
            files += '\"' + escape_json(file) + '\"';
        }
        return response_prefix(id) + "{\"projects\":" + files + "]}}";
    }
    if (method == "project.package") {
        if (!engine_.project()) return error_response(id, "no project is open");
        const auto filename = string_field(request, "filename");
        std::string error;
        std::uint64_t bytes{};
        if (!package_project(*engine_.project(), filename, error, bytes))
            return error_response(id, error);
        return response_prefix(id) + "{\"filename\":\"" +
               escape_json((engine_.project()->root() / "exports" / filename).generic_string()) +
               "\",\"bytes\":" + std::to_string(bytes) + "}}";
    }
    if (method == "project.close") {
        engine_.project().reset();
        return response_prefix(id) + "{\"project\":null}}";
    }
    if (method == "project.create" || method == "project.open") {
        std::string error;
        const auto filename = string_field(request, "filename");
        std::string warning;
        auto project = method == "project.open" ? load_project(filename, error, warning)
                                                : std::optional{Project{filename, string_field(request, "name"), {}, {}, {}, false, {}, {}}};
        if (!project) return error_response(id, error);
        if (!warning.empty()) engine_.logs().write(LogLevel::warning, warning);
        SceneState state;
        if (!project->startup_scene.empty()) {
            const auto path = workspace_file((project->root() / "scenes").generic_string(), project->startup_scene, ".relay.json");
            if (!path) return error_response(id, "unsafe project startup scene");
            auto loaded = load_scene_file(*path);
            if (!loaded) return error_response(id, "startup scene: " + loaded.error);
            state = std::move(*loaded.state);
        }
        if (method == "project.create" && !save_project(*project, error, true))
            return error_response(id, error);
        // Validate disk content before changing either the scene or active project.
        auto reload = reload_imported_assets(project->root(), engine_.assets());
        if (!engine_.scene_history().execute("Open project " + project->name, [&](Scene& scene) {
                if (project->startup_scene.empty()) scene.clear();
                else scene.restore_state(state);
                (void)reload.rebind_scene(scene);
                return true;
            })) return error_response(id, "cannot apply project scene");
        engine_.project() = std::move(project);
        return response_prefix(id) + "{\"project\":" + engine_.project()->json(false) +
               ",\"scene_file\":\"" + escape_json(engine_.project()->startup_scene) +
               "\",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "project.add_scene" || method == "project.remove_scene" || method == "project.set_startup") {
        if (!engine_.project()) return error_response(id, "no project is open");
        auto project = *engine_.project();
        const auto filename = string_field(request, "scene_file");
        const auto path = safe_scene_path(engine_, filename);
        if (!path) return error_response(id, "unsafe scene filename");
        auto found = std::find(project.scenes.begin(), project.scenes.end(), filename);
        if (method == "project.add_scene") {
            auto loaded = load_scene_file(*path);
            if (!loaded) return error_response(id, "cannot add scene: " + loaded.error);
            if (found == project.scenes.end()) project.scenes.push_back(filename);
            if (project.startup_scene.empty()) project.startup_scene = filename;
        } else {
            if (found == project.scenes.end()) return error_response(id, "scene is not a project member");
            if (method == "project.set_startup") project.startup_scene = filename;
            else {
                project.scenes.erase(found);
                if (project.startup_scene == filename)
                    project.startup_scene = project.scenes.empty() ? "" : project.scenes.front();
            }
        }
        std::string error;
        if (!save_project(project, error)) return error_response(id, error);
        engine_.project() = std::move(project);
        return response_prefix(id) + "{\"project\":" + engine_.project()->json(false) + "}}";
    }
    if (method == "animation.clip") {
        const auto* model = engine_.assets().find_model(string_field(request, "model"));
        const auto index = unsigned_field(request, "clip", 0);
        if (!model || index >= model->clips.size()) return error_response(id, "clip is unavailable");
        const auto& clip = model->clips[static_cast<std::size_t>(index)];
        std::ostringstream channels;
        channels << std::setprecision(17) << '[';
        std::size_t count = 0;
        const auto channel = [&](const std::string& name, const auto& keys) {
            if (keys.empty() || count >= 256) return;
            if (count++) channels << ',';
            channels << "{\"name\":\"" << json_escape(name) << "\",\"key_count\":" << keys.size()
                     << ",\"times\":[";
            const auto size = std::min<std::size_t>(keys.size(), 256);
            for (std::size_t i = 0; i < size; ++i) {
                if (i) channels << ',';
                // Sample the whole channel when bounded, retaining both endpoints.
                const auto key = keys.size() <= size ? i : i * (keys.size() - 1) / (size - 1);
                channels << keys[key].time;
            }
            channels << "]}";
        };
        std::size_t total_channels = 0;
        for (const auto& track : clip.tracks) {
            const auto node = track.node < model->nodes.size() ? model->nodes[track.node] : "Node " + std::to_string(track.node);
            channel(node + " / Position", track.positions);
            channel(node + " / Rotation", track.rotations);
            channel(node + " / Scale", track.scales);
            total_channels += !track.positions.empty() + !track.rotations.empty() + !track.scales.empty();
        }
        for (const auto& track : clip.morph_tracks) {
            channel("Mesh " + std::to_string(track.mesh) + " / Morph", track.keys);
            total_channels += !track.keys.empty();
        }
        channels << ']';
        return response_prefix(id) + "{\"name\":\"" + json_escape(clip.name) +
               "\",\"channel_count\":" + std::to_string(total_channels) +
               ",\"channels\":" + channels.str() + "}}";
    }
    if (method == "scene.set_animations") {
        JsonParser parser(request);
        const auto parsed = parser.parse();
        const auto& object = *parsed->object();
        std::vector<std::pair<Entity, Animator>> updates;
        std::set<Entity> seen;
        for (const auto& value : *field(object, "entities")->array()) {
            const auto parsed_entity = Entity::parse(*value.string());
            if (!parsed_entity) return error_response(id, "invalid animation entity handle");
            const auto entity = *parsed_entity;
            if (!seen.insert(entity).second) continue;
            const auto* record = engine_.scene().get(entity);
            if (!record || !record->animator) return error_response(id, "animation root is stale or invalid");
            auto animator = *record->animator;
            const auto* model = engine_.assets().find_model(animator.model);
            if (!model || animator.clip >= model->clips.size()) return error_response(id, "clip is unavailable");
            animator.playing = boolean_field(request, "playing", animator.playing);
            animator.loop = boolean_field(request, "loop", animator.loop);
            if (const auto speed = number_field(request, "speed")) animator.speed = *speed;
            if (const auto time = number_field(request, "time_seconds"))
                animator.time_seconds = std::min(*time, model->clips[animator.clip].duration_seconds);
            updates.emplace_back(entity, animator);
        }
        const auto label = "Configure animation tracks " + json_stringify(*field(object, "entities"));
        if (!engine_.scene_history().execute(label, [&](Scene& scene) {
                for (const auto& [entity, animator] : updates)
                    if (!scene.set_animator(entity, animator)) return false;
                return true;
            }, unsigned_field(request, "gesture", 0))) return error_response(id, "invalid animation configuration");
        return response_prefix(id) + "{\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.clipboard") {
        return response_prefix(id) + "{\"roots\":" + std::to_string(engine_.clipboard().roots.size()) +
               ",\"entities\":" + std::to_string(engine_.clipboard().nodes.size()) + "}}";
    }
    if (method == "scene.copy" || method == "scene.cut" || method == "scene.duplicate_many" ||
        method == "scene.destroy_many" || method == "scene.transform_many" || method == "scene.paste") {
        JsonParser parser(request);
        const auto input = parser.parse();
        const auto& object = *input->object();
        std::vector<Entity> selected;
        if (const auto* values = field(object, "entities"))
            for (const auto& value : *values->array()) {
                const auto entity = Entity::parse(*value.string());
                if (!entity) return error_response(id, "selection contains an invalid entity handle");
                selected.push_back(*entity);
            }
        auto roots = method == "scene.paste" ? std::optional{std::vector<Entity>{}}
                                            : selection_roots(engine_.scene(), selected);
        if (!roots) return error_response(id, "selection contains a stale or invalid entity");
        std::optional<SceneClipboard> copied;
        if (method == "scene.copy" || method == "scene.cut" || method == "scene.duplicate_many") {
            copied = copy_selection(engine_.scene(), selected);
            if (!copied) return error_response(id, "selected subtrees exceed the clipboard limit");
        }
        if (method == "scene.copy") {
            engine_.clipboard() = std::move(*copied);
            return response_prefix(id) + "{\"copied\":" +
                   std::to_string(engine_.clipboard().nodes.size()) + "}}";
        }
        std::vector<Entity> result_roots;
        const auto parent = method == "scene.paste" ? entity_field(request, "parent") : std::optional{Entity{}};
        if (!parent || (parent->valid() && !engine_.scene().contains(*parent)))
            return error_response(id, "paste parent is stale or invalid");
        std::vector<std::pair<Entity, Transform>> transforms;
        if (method == "scene.transform_many") {
            EditorMatrix delta{};
            const auto& values = *field(object, "delta")->array();
            for (std::size_t i = 0; i < 16; ++i) {
                delta[i] = static_cast<float>(*values[i].number());
                if (!std::isfinite(delta[i])) return error_response(id, "delta is not representable");
            }
            if (delta[3] != 0 || delta[7] != 0 || delta[11] != 0 || delta[15] != 1)
                return error_response(id, "delta must be affine");
            const auto world_of = [&](Entity entity) {
                std::vector<Entity> chain;
                while (engine_.scene().contains(entity)) {
                    chain.push_back(entity);
                    entity = engine_.scene().get(entity)->parent;
                }
                auto world = editor_identity();
                for (auto i = chain.rbegin(); i != chain.rend(); ++i) {
                    const auto& t = engine_.scene().get(*i)->transform;
                    world = editor_multiply(world, editor_compose(t.position, t.rotation_degrees, t.scale));
                }
                return world;
            };
            for (const auto entity : *roots) {
                const auto inverse = editor_inverse_affine(world_of(engine_.scene().get(entity)->parent));
                if (!inverse) return error_response(id, "selected entity has a singular parent");
                const auto local = editor_multiply(*inverse, editor_multiply(delta, world_of(entity)));
                Transform t;
                editor_decompose(local, t.position, t.rotation_degrees, t.scale);
                // Relay stores TRS, so refuse a delta that would require shear instead of losing it.
                const auto recomposed = editor_compose(t.position, t.rotation_degrees, t.scale);
                for (std::size_t i = 0; i < 16; ++i)
                    if (!std::isfinite(local[i]) || !std::isfinite(recomposed[i]) ||
                        std::abs(local[i] - recomposed[i]) > 0.001F * std::max(1.0F, std::abs(local[i])))
                        return error_response(id, "group transform cannot be represented without shear");
                transforms.emplace_back(entity, t);
            }
        }
        const auto label = method == "scene.transform_many"
                               ? "Transform selection " + json_stringify(*field(object, "entities"))
                           : method == "scene.paste" ? std::string{"Paste selection"}
                           : method == "scene.cut" ? std::string{"Cut selection"}
                           : method == "scene.duplicate_many" ? std::string{"Duplicate selection"}
                           : std::string{"Delete selection"};
        const bool changed = engine_.scene_history().execute(label, [&](Scene& scene) {
            if (method == "scene.paste" || method == "scene.duplicate_many") {
                const auto& clipboard = method == "scene.paste" ? engine_.clipboard() : *copied;
                result_roots = paste_selection(scene, clipboard, *parent, method == "scene.duplicate_many");
                return !result_roots.empty();
            }
            if (method == "scene.transform_many") {
                for (const auto& [entity, transform] : transforms)
                    if (!scene.set_transform(entity, transform)) return false;
                result_roots = *roots;
            } else for (const auto entity : *roots) if (!scene.destroy(entity)) return false;
            return true;
        }, method == "scene.transform_many" ? unsigned_field(request, "gesture", 0) : 0);
        if (!changed) return error_response(id, "group operation failed or clipboard is empty");
        if (method == "scene.cut") engine_.clipboard() = std::move(*copied);
        std::string handles = "[";
        for (const auto entity : result_roots) {
            if (handles.size() > 1) handles += ',';
            handles += "\"" + entity.to_string() + "\"";
        }
        return response_prefix(id) + "{\"roots\":" + handles + "],\"history\":" +
               history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.duplicate") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid()) return error_response(id, "invalid entity handle");
        Entity copy{};
        if (!engine_.scene_history().execute("Duplicate " + entity->to_string(),
                                             [&](Scene& scene) {
                                                 copy = scene.duplicate(*entity);
                                                 return copy.valid();
                                             })) {
            return error_response(
                id, "entity does not exist, has a stale handle, or its subtree is too large");
        }
        engine_.logs().write(LogLevel::info,
                             "Duplicated entity " + entity->to_string() + " as " + copy.to_string());
        return response_prefix(id) + "{\"entity\":\"" + copy.to_string() + "\",\"source\":\"" +
               entity->to_string() + "\",\"history\":" + history_json(engine_.scene_history()) +
               "}}";
    }
    if (method == "scene.clear") {
        const auto removed = engine_.scene().entities().size();
        if (!engine_.scene_history().execute("Clear scene", [](Scene& scene) {
                scene.clear();
                return true;
            })) {
            return error_response(id, "could not clear the scene");
        }
        engine_.logs().write(LogLevel::info,
                             "Cleared scene, removing " + std::to_string(removed) + " entities");
        return response_prefix(id) + "{\"removed\":" + std::to_string(removed) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_transform") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid()) return error_response(id, "invalid entity handle");
        const auto* record = engine_.scene().get(*entity);
        if (record == nullptr) return error_response(id, "entity does not exist or has a stale handle");
        auto transform = record->transform;
        if (const auto value = number_field(request, "px")) transform.position.x = *value;
        if (const auto value = number_field(request, "py")) transform.position.y = *value;
        if (const auto value = number_field(request, "pz")) transform.position.z = *value;
        if (const auto value = number_field(request, "rx")) transform.rotation_degrees.x = *value;
        if (const auto value = number_field(request, "ry")) transform.rotation_degrees.y = *value;
        if (const auto value = number_field(request, "rz")) transform.rotation_degrees.z = *value;
        if (const auto value = number_field(request, "sx")) transform.scale.x = *value;
        if (const auto value = number_field(request, "sy")) transform.scale.y = *value;
        if (const auto value = number_field(request, "sz")) transform.scale.z = *value;
        const auto gesture = unsigned_field(request, "gesture", 0U);
        if (!engine_.scene_history().execute(
                "Transform " + entity->to_string(),
                [&](Scene& scene) { return scene.set_transform(*entity, transform); }, gesture)) {
            return error_response(id, "could not update transform");
        }
        engine_.logs().write(LogLevel::info, "Updated transform for " + entity->to_string());
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_camera") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid()) return error_response(id, "invalid entity handle");
        const auto* record = engine_.scene().get(*entity);
        if (record == nullptr) return error_response(id, "entity does not exist or has a stale handle");
        const bool enabled = boolean_field(request, "enabled", true);
        std::optional<Camera> camera;
        if (enabled) {
            camera = record->camera.value_or(Camera{});
            camera->active = boolean_field(request, "active", camera->active);
            if (const auto value = number_field(request, "field_of_view_y_degrees")) {
                camera->field_of_view_y_degrees = *value;
            }
            if (const auto value = number_field(request, "near_plane")) camera->near_plane = *value;
            if (const auto value = number_field(request, "far_plane")) camera->far_plane = *value;
            if (const auto value = number_field(request, "orthographic_height"))
                camera->orthographic_height = *value;
            if (const auto value = number_field(request, "exposure_ev"))
                camera->exposure_ev = *value;
            if (camera->far_plane <= camera->near_plane) {
                return error_response(id, "camera far_plane must be greater than near_plane");
            }
        }
        if (!engine_.scene_history().execute(
                std::string(enabled ? "Configure camera " : "Remove camera ") + entity->to_string(),
                [&](Scene& scene) { return scene.set_camera(*entity, camera); }, unsigned_field(request, "gesture", 0U))) {
            return error_response(id, "could not update camera component");
        }
        engine_.logs().write(LogLevel::info,
                             std::string(enabled ? "Configured camera for " : "Removed camera from ") +
                                 entity->to_string());
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.keyframe.set" || method == "scene.keyframe.delete" ||
        method == "scene.keyframes.playback") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto* record = engine_.scene().get(*entity);
        auto animation = record->transform_animation.value_or(TransformAnimation{});
        if (method == "scene.keyframe.set") {
            const auto time = number_field(request, "time_seconds").value_or(0.0);
            auto found = std::lower_bound(animation.keys.begin(), animation.keys.end(), time,
                [](const TransformKeyframe& key, const double value) {
                    return key.time_seconds < value;
                });
            Transform value = found != animation.keys.end() && found->time_seconds == time
                                  ? found->value : record->transform;
            const auto update = [&](const char* field_name, double& target) {
                if (const auto number = number_field(request, field_name)) target = *number;
            };
            update("px", value.position.x); update("py", value.position.y);
            update("pz", value.position.z); update("rx", value.rotation_degrees.x);
            update("ry", value.rotation_degrees.y); update("rz", value.rotation_degrees.z);
            update("sx", value.scale.x); update("sy", value.scale.y);
            update("sz", value.scale.z);
            if (found != animation.keys.end() && found->time_seconds == time)
                found->value = value;
            else animation.keys.insert(found, TransformKeyframe{time, value});
            animation.duration_seconds = std::max(animation.duration_seconds, time);
        } else if (method == "scene.keyframe.delete") {
            if (!record->transform_animation)
                return error_response(id, "entity has no transform keyframes");
            const auto time = number_field(request, "time_seconds").value_or(0.0);
            const auto found = std::lower_bound(animation.keys.begin(), animation.keys.end(), time,
                [](const TransformKeyframe& key, const double value) {
                    return key.time_seconds < value;
                });
            if (found == animation.keys.end() || found->time_seconds != time)
                return error_response(id, "keyframe does not exist at this time");
            animation.keys.erase(found);
        } else {
            animation.playing = boolean_field(request, "playing", animation.playing);
            animation.loop = boolean_field(request, "loop", animation.loop);
            if (const auto value = number_field(request, "speed")) animation.speed = *value;
            if (const auto value = number_field(request, "duration_seconds"))
                animation.duration_seconds = *value;
            if (const auto value = number_field(request, "time_seconds"))
                animation.time_seconds = *value;
            animation.time_seconds = std::min(animation.time_seconds,
                                              animation.duration_seconds);
        }
        if (!engine_.scene_history().execute(
                "Edit transform keyframes " + entity->to_string(),
                [&](Scene& scene) { return scene.set_transform_animation(*entity, animation); },
                unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid transform animation values or keyframe limit");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_physics_body") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<PhysicsBody> body;
        if (boolean_field(request, "attached", true)) {
            body = engine_.scene().get(*entity)->physics_body.value_or(PhysicsBody{});
            const auto type = string_field(request, "type");
            if (!type.empty())
                body->type = type == "static" ? PhysicsBody::Type::static_body
                                              : PhysicsBody::Type::dynamic;
            if (const auto value = number_field(request, "mass")) body->mass = *value;
            if (const auto value = number_field(request, "gravity_scale"))
                body->gravity_scale = *value;
            if (const auto value = number_field(request, "restitution"))
                body->restitution = *value;
            if (const auto value = number_field(request, "friction"))
                body->friction = *value;
            if (const auto value = number_field(request, "linear_damping"))
                body->linear_damping = *value;
            if (const auto value = number_field(request, "angular_damping"))
                body->angular_damping = *value;
            body->lock_rotation = boolean_field(request, "lock_rotation", body->lock_rotation);
        }
        if (!engine_.scene_history().execute("Configure physics body " + entity->to_string(),
                [&](Scene& scene) { return scene.set_physics_body(*entity, body); }, unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid physics body values");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_joint") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<Joint> joint;
        if (boolean_field(request, "attached", true)) {
            joint = engine_.scene().get(*entity)->joint.value_or(Joint{});
            if (const auto type = optional_string_field(request, "type")) {
                const auto parsed = joint_type_from_name(*type);
                if (!parsed) return error_response(id, "unknown joint type");
                if (*parsed != joint->type) {
                    joint->type = *parsed;
                    set_default_joint_limits(*joint);
                }
            }
            if (const auto connected = optional_string_field(request, "connected")) {
                if (connected->empty()) {
                    joint->connected = {};
                } else {
                    const auto partner = Entity::parse(*connected);
                    if (!partner || !engine_.scene().contains(*partner))
                        return error_response(id, "connected must be a live node, or empty for the world");
                    joint->connected = *partner;
                }
            }
            const auto vector = [&](const char* prefix, Vec3& target) {
                const std::string base{prefix};
                if (const auto value = number_field(request, base + "_x")) target.x = *value;
                if (const auto value = number_field(request, base + "_y")) target.y = *value;
                if (const auto value = number_field(request, base + "_z")) target.z = *value;
            };
            vector("anchor", joint->anchor);
            vector("axis", joint->axis);
            vector("connected_anchor", joint->connected_anchor);
            const auto number = [&](const char* key, double& target) {
                if (const auto value = number_field(request, key)) target = *value;
            };
            number("limit_min", joint->limit_min);
            number("limit_max", joint->limit_max);
            number("motor_speed", joint->motor_speed);
            number("motor_force", joint->motor_force);
            number("spring_frequency", joint->spring_frequency);
            number("spring_damping", joint->spring_damping);
            joint->limits = boolean_field(request, "limits", joint->limits);
            joint->motor = boolean_field(request, "motor", joint->motor);
            joint->collide_connected =
                boolean_field(request, "collide_connected", joint->collide_connected);
            joint->enabled = boolean_field(request, "enabled", joint->enabled);
        }
        if (!engine_.scene_history().execute("Configure joint " + entity->to_string(),
                [&](Scene& scene) { return scene.set_joint(*entity, joint); }, unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid joint values: the connected node must differ from "
                                      "this one, the axis must not be zero, and limits must fit "
                                      "the joint type");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_audio_source") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<AudioSource> source;
        if (boolean_field(request, "attached", true)) {
            source = engine_.scene().get(*entity)->audio_source.value_or(AudioSource{});
            if (const auto clip = optional_string_field(request, "clip")) source->clip = *clip;
            if (const auto bus = optional_string_field(request, "bus")) source->bus = *bus;
            if (const auto rolloff = optional_string_field(request, "rolloff")) {
                const auto parsed = audio_rolloff_from_name(*rolloff);
                if (!parsed) return error_response(id, "unknown rolloff");
                source->rolloff = *parsed;
            }
            const auto number = [&](const char* key, double& target) {
                if (const auto value = number_field(request, key)) target = *value;
            };
            number("volume_db", source->volume_db);
            number("pitch", source->pitch);
            number("pan", source->pan);
            number("min_distance", source->min_distance);
            number("max_distance", source->max_distance);
            number("doppler", source->doppler);
            source->loop = boolean_field(request, "loop", source->loop);
            source->play_on_start = boolean_field(request, "play_on_start", source->play_on_start);
            source->spatial = boolean_field(request, "spatial", source->spatial);
            source->occlusion = boolean_field(request, "occlusion", source->occlusion);
            number("reverb_send", source->reverb_send);
        }
        if (!engine_.scene_history().execute(
                std::string(source ? "Configure audio source " : "Remove audio source ") +
                    entity->to_string(),
                [&](Scene& scene) { return scene.set_audio_source(*entity, source); },
                unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid audio source values: the maximum distance must "
                                      "not be below the minimum distance");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_reverb_zone") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<ReverbZone> zone;
        if (boolean_field(request, "attached", true)) {
            zone = engine_.scene().get(*entity)->reverb_zone.value_or(ReverbZone{});
            if (const auto shape = optional_string_field(request, "shape")) {
                const auto parsed = reverb_shape_from_name(*shape);
                if (!parsed) return error_response(id, "unknown reverb zone shape");
                zone->shape = *parsed;
            }
            const auto number = [&](const char* key, double& target) {
                if (const auto value = number_field(request, key)) target = *value;
            };
            number("radius", zone->radius);
            number("half_x", zone->half_extents.x);
            number("half_y", zone->half_extents.y);
            number("half_z", zone->half_extents.z);
            number("fade", zone->fade);
            // Editing the sound by hand leaves the preset behind.
            bool tuned = false;
            for (const auto& [key, target] :
                 std::initializer_list<std::pair<const char*, double*>>{
                     {"room_size", &zone->room_size}, {"damping", &zone->damping},
                     {"wet_db", &zone->wet_db}, {"pre_delay_ms", &zone->pre_delay_ms}})
                if (const auto value = number_field(request, key)) {
                    *target = *value;
                    tuned = true;
                }
            if (tuned) zone->preset = "custom";
            if (const auto preset = optional_string_field(request, "preset")) {
                if (*preset == "custom") zone->preset = "custom";
                else if (!apply_reverb_preset(*zone, *preset))
                    return error_response(id, "unknown reverb preset " + *preset);
            }
        }
        if (!engine_.scene_history().execute(
                std::string(zone ? "Configure reverb zone " : "Remove reverb zone ") +
                    entity->to_string(),
                [&](Scene& scene) { return scene.set_reverb_zone(*entity, zone); },
                unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid reverb zone values");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_music_player") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<MusicPlayer> player;
        if (boolean_field(request, "attached", true)) {
            player = engine_.scene().get(*entity)->music_player.value_or(MusicPlayer{});
            JsonParser parser(request);
            const auto parsed = parser.parse();
            if (const auto* tracks = parsed && parsed->object() ? field(*parsed->object(), "tracks") : nullptr;
                tracks && tracks->array()) {
                player->tracks.clear();
                for (const auto& track : *tracks->array())
                    if (track.string()) player->tracks.push_back(*track.string());
            }
            if (const auto bus = optional_string_field(request, "bus")) player->bus = *bus;
            if (const auto sync = optional_string_field(request, "sync")) {
                const auto parsed_sync = music_sync_from_name(*sync);
                if (!parsed_sync) return error_response(id, "unknown music sync");
                player->sync = *parsed_sync;
            }
            const auto number = [&](const char* key, double& target) {
                if (const auto value = number_field(request, key)) target = *value;
            };
            number("volume_db", player->volume_db);
            number("crossfade_seconds", player->crossfade_seconds);
            number("bpm", player->bpm);
            number("first_beat_seconds", player->first_beat_seconds);
            if (const auto beats = number_field(request, "beats_per_bar"))
                player->beats_per_bar = static_cast<std::uint32_t>(*beats);
            player->shuffle = boolean_field(request, "shuffle", player->shuffle);
            player->loop_playlist = boolean_field(request, "loop_playlist", player->loop_playlist);
            player->play_on_start = boolean_field(request, "play_on_start", player->play_on_start);
        }
        if (!engine_.scene_history().execute(
                std::string(player ? "Configure music player " : "Remove music player ") +
                    entity->to_string(),
                [&](Scene& scene) { return scene.set_music_player(*entity, player); },
                unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid music player values");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "component.types") {
        std::ostringstream output;
        output << response_prefix(id) << "{\"engine\":[";
        const auto& kinds = engine_components();
        for (std::size_t index = 0; index < kinds.size(); ++index) {
            const auto& kind = kinds[index];
            output << (index ? "," : "") << "{\"id\":\"" << kind.id << "\",\"name\":\""
                   << kind.name << "\",\"category\":\"" << kind.category
                   << "\",\"addable\":" << (kind.addable ? "true" : "false")
                   << ",\"removable\":" << (kind.removable ? "true" : "false")
                   << ",\"multiple\":" << (kind.multiple ? "true" : "false")
                   << ",\"description\":\"" << escape_json(kind.description) << "\"}";
        }
        output << "],\"behaviours\":";
        append_behaviours(output, engine_.scripts().status().behaviours);
        output << "}}";
        return output.str();
    }
    if (method == "component.add" || method == "component.remove" || method == "scene.set_script" ||
        method == "scene.set_script_property") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto component = string_field(request, "component");
        const auto index = static_cast<std::size_t>(unsigned_field(request, "index", 0));
        std::string error;
        std::string label;
        // Outlives `change`, which runs after this block.
        const auto behaviour = string_field(request, "behaviour");
        std::function<bool(Scene&)> change;
        if (method == "component.add") {
            label = "Add " + (component == "script" ? behaviour : component) + " to " +
                    entity->to_string();
            change = [&](Scene& scene) {
                return add_component(scene, *entity, component, behaviour, error);
            };
        } else if (method == "component.remove") {
            label = "Remove " + component + " from " + entity->to_string();
            change = [&](Scene& scene) {
                return remove_component(scene, *entity, component, index, error);
            };
        } else {
            auto scripts = engine_.scene().get(*entity)->scripts;
            if (index >= scripts.size())
                return error_response(id, "the node has no script component at that index");
            auto& script = scripts[index];
            if (method == "scene.set_script") {
                if (const auto replacement = optional_string_field(request, "behaviour")) {
                    if (*replacement != script.behaviour) script.properties.clear();
                    script.behaviour = *replacement;
                }
                script.enabled = boolean_field(request, "enabled", script.enabled);
                label = "Configure script " + script.behaviour + " on " + entity->to_string();
            } else {
                const auto name = string_field(request, "property");
                std::erase_if(script.properties,
                              [&](const ScriptProperty& property) { return property.name == name; });
                if (!boolean_field(request, "reset", false)) {
                    ScriptProperty property;
                    property.name = name;
                    JsonParser parser(request);
                    const auto parsed = parser.parse();
                    const auto& fields = *parsed->object();
                    int given = 0;
                    if (const auto* value = field(fields, "number")) {
                        property.type = ScriptProperty::Type::number;
                        property.number = *value->number();
                        ++given;
                    }
                    if (const auto* value = field(fields, "boolean")) {
                        property.type = ScriptProperty::Type::boolean;
                        property.boolean = *value->boolean();
                        ++given;
                    }
                    if (const auto* value = field(fields, "text")) {
                        property.type = ScriptProperty::Type::text;
                        property.text = *value->string();
                        ++given;
                    }
                    if (const auto* value = field(fields, "vector")) {
                        property.type = ScriptProperty::Type::vector;
                        const auto& items = *value->array();
                        property.vector = {*items[0].number(), *items[1].number(),
                                           *items[2].number()};
                        ++given;
                    }
                    if (given != 1)
                        return error_response(id, "send exactly one of number, boolean, text or vector");
                    script.properties.push_back(std::move(property));
                }
                label = "Set " + script.behaviour + "." + name + " on " + entity->to_string();
            }
            change = [&, scripts](Scene& scene) mutable {
                if (scene.set_scripts(*entity, std::move(scripts))) return true;
                error = "invalid script component values";
                return false;
            };
        }
        if (!engine_.scene_history().execute(label, change, unsigned_field(request, "gesture", 0U)))
            return error_response(id, error.empty() ? "component change failed" : error);
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "nodes.types") {
        std::ostringstream output;
        output << response_prefix(id) << "{\"types\":[";
        const auto& types = node_types();
        for (std::size_t index = 0; index < types.size(); ++index) {
            const auto& type = types[index];
            output << (index ? "," : "") << "{\"id\":\"" << type.id << "\",\"name\":\""
                   << type.name << "\",\"parent\":";
            if (type.parent.empty()) output << "null";
            else output << '"' << type.parent << '"';
            output << ",\"description\":\"" << escape_json(type.description)
                   << "\",\"creatable\":" << (type.creatable ? "true" : "false")
                   << ",\"components\":[";
            const auto components = node_type_components(type.id);
            for (std::size_t item = 0; item < components.size(); ++item)
                output << (item ? "," : "") << '"' << components[item] << '"';
            output << "]}";
        }
        output << "]}}";
        return output.str();
    }
    if (method == "templates.list") {
        std::optional<std::filesystem::path> root;
        if (engine_.project()) root = engine_.project()->root();
        std::ostringstream output;
        output << response_prefix(id) << "{\"templates\":[";
        bool first = true;
        for (const auto& entry : list_templates(root)) {
            output << (first ? "" : ",") << "{\"id\":\"" << escape_json(entry.id)
                   << "\",\"name\":\"" << escape_json(entry.name) << "\",\"type\":\""
                   << entry.type << "\",\"path\":\"" << escape_json(entry.path)
                   << "\",\"components\":[";
            for (std::size_t index = 0; index < entry.components.size(); ++index)
                output << (index ? "," : "") << '"' << entry.components[index] << '"';
            output << "],\"behaviours\":[";
            for (std::size_t index = 0; index < entry.behaviours.size(); ++index)
                output << (index ? "," : "") << '"' << escape_json(entry.behaviours[index]) << '"';
            output << "]}";
            first = false;
        }
        output << "]}}";
        return output.str();
    }
    if (method == "templates.instantiate") {
        const auto parent = entity_field(request, "parent");
        if (!parent) return error_response(id, "invalid parent entity");
        std::optional<std::filesystem::path> root;
        if (engine_.project()) root = engine_.project()->root();
        const auto identifier = string_field(request, "template");
        std::optional<Entity> created;
        std::string error;
        if (!engine_.scene_history().execute("Create " + identifier, [&](Scene& scene) {
                created = instantiate_template(scene, root, identifier, *parent,
                                               string_field(request, "name"), error);
                return created.has_value();
            }))
            return error_response(id, error.empty() ? "template could not be created" : error);
        return response_prefix(id) + "{\"entity\":\"" + created->to_string() + "\",\"node\":" +
               engine_.scene().entity_json(*created) + ",\"history\":" +
               history_json(engine_.scene_history()) + "}}";
    }
    if (method == "templates.save") {
        if (!engine_.project()) return error_response(id, "templates need an open project");
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto name = string_field(request, "name");
        std::string error;
        if (!save_template(engine_.scene(), *entity, engine_.project()->root(), name,
                           boolean_field(request, "replace", false), error))
            return error_response(id, error);
        engine_.logs().write(LogLevel::info, "Saved template " + name);
        return response_prefix(id) + "{\"id\":\"project:" + escape_json(name) + "\",\"path\":\"" +
               std::string{template_directory} + "/" + escape_json(name) +
               std::string{template_suffix} + "\"}}";
    }
    if (method == "scene.set_collider") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        std::optional<BoxCollider> collider;
        if (boolean_field(request, "attached", true)) {
            collider = engine_.scene().get(*entity)->collider.value_or(BoxCollider{});
            const auto type = string_field(request, "type");
            if (!type.empty()) {
                if (type == "box") collider->type = BoxCollider::Type::box;
                else if (type == "sphere") collider->type = BoxCollider::Type::sphere;
                else if (type == "capsule") collider->type = BoxCollider::Type::capsule;
                else if (type == "convex") collider->type = BoxCollider::Type::convex;
                else if (type == "mesh") collider->type = BoxCollider::Type::mesh;
                else return error_response(id, "invalid collider shape");
            }
            if (auto mesh = optional_string_field(request, "mesh")) {
                if (!mesh->empty() && !engine_.assets().find_mesh(*mesh))
                    return error_response(id, "unknown collider mesh");
                collider->mesh = std::move(*mesh);
            }
            collider->enabled = boolean_field(request, "enabled", collider->enabled);
            if (const auto value = number_field(request, "center_x")) collider->center.x = *value;
            if (const auto value = number_field(request, "center_y")) collider->center.y = *value;
            if (const auto value = number_field(request, "center_z")) collider->center.z = *value;
            if (const auto value = number_field(request, "half_x")) collider->half_extents.x = *value;
            if (const auto value = number_field(request, "half_y")) collider->half_extents.y = *value;
            if (const auto value = number_field(request, "half_z")) collider->half_extents.z = *value;
            if (const auto value = number_field(request, "radius")) collider->radius = *value;
            if (const auto value = number_field(request, "half_height")) collider->half_height = *value;
            collider->layer = static_cast<std::uint32_t>(
                unsigned_field(request, "layer", collider->layer));
            collider->mask = static_cast<std::uint32_t>(
                unsigned_field(request, "mask", collider->mask));
        }
        if (!engine_.scene_history().execute("Configure collider " + entity->to_string(),
                [&](Scene& scene) { return scene.set_collider(*entity, collider); }, unsigned_field(request, "gesture", 0U)))
            return error_response(id, "invalid collider values");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_animation" || method == "scene.set_morph" ||
        method == "scene.set_light") {
        const auto entity = Entity::parse(string_field(request, "entity"));
        if (!entity || !engine_.scene().contains(*entity))
            return error_response(id, "invalid or stale entity");
        const auto record = *engine_.scene().get(*entity);
        bool changed = false;
        if (method == "scene.set_animation") {
            if (!record.animator)
                return error_response(id, "entity is not an imported model root");
            auto animator = *record.animator;
            const auto *model = engine_.assets().find_model(animator.model);
            if (!model)
                return error_response(id, "model asset is unavailable");
            if (const auto value = number_field(request, "clip"))
                animator.clip = static_cast<std::uint32_t>(*value);
            if (animator.clip >= model->clips.size())
                return error_response(id, "animation clip is unavailable");
            animator.playing = boolean_field(request, "playing", animator.playing);
            animator.loop = boolean_field(request, "loop", animator.loop);
            if (const auto value = number_field(request, "speed"))
                animator.speed = *value;
            if (const auto value = number_field(request, "time_seconds"))
                animator.time_seconds = *value;
            animator.time_seconds =
                std::min(animator.time_seconds, model->clips[animator.clip].duration_seconds);
            changed = engine_.scene_history().execute(
                "Configure animation " + entity->to_string(),
                [&](Scene &scene) { return scene.set_animator(*entity, animator); },
                unsigned_field(request, "gesture", 0U));
        } else if (method == "scene.set_morph") {
            if (!record.mesh_renderer)
                return error_response(id, "entity has no mesh renderer");
            auto renderer = *record.mesh_renderer;
            const auto *mesh = engine_.assets().find_mesh(renderer.mesh);
            if (!mesh)
                return error_response(id, "mesh asset is unavailable");
            if (boolean_field(request, "reset", false))
                renderer.morph_weights.clear();
            else {
                const auto target =
                    static_cast<std::size_t>(number_field(request, "target").value_or(0.0));
                if (target >= mesh->morph_targets.size())
                    return error_response(id, "morph target is unavailable");
                if (renderer.morph_weights.empty())
                    for (const auto &morph : mesh->morph_targets)
                        renderer.morph_weights.push_back(morph.weight);
                renderer.morph_weights[target] = number_field(request, "weight").value_or(0.0);
            }
            changed = engine_.scene_history().execute("Configure morph weights", [&](Scene &scene) {
                return scene.set_mesh_renderer(*entity, renderer);
            }, unsigned_field(request, "gesture", 0U));
        } else {
            std::optional<Light> light;
            if (boolean_field(request, "enabled", true)) {
                light = record.light.value_or(Light{});
                const auto type = string_field(request, "type");
                if (!type.empty())
                    light->type = type == "directional" ? Light::Type::directional
                                  : type == "spot"      ? Light::Type::spot
                                                        : Light::Type::point;
                const auto assign = [&](std::string_view key, double &target) {
                    if (const auto value = number_field(request, key))
                        target = *value;
                };
                assign("red", light->color.x);
                assign("green", light->color.y);
                assign("blue", light->color.z);
                assign("intensity", light->intensity);
                assign("constant", light->attenuation.x);
                assign("linear", light->attenuation.y);
                assign("quadratic", light->attenuation.z);
                assign("inner_cone", light->inner_cone);
                assign("outer_cone", light->outer_cone);
                assign("range", light->range);
            }
            changed = engine_.scene_history().execute(
                "Configure light", [&](Scene &scene) { return scene.set_light(*entity, light); }, unsigned_field(request, "gesture", 0U));
        }
        if (!changed)
            return error_response(id, "invalid component configuration");
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_renderer") {
        const auto entity = entity_field(request, "entity");
        if (!entity.has_value() || !entity->valid() || engine_.scene().get(*entity) == nullptr) {
            return error_response(id, "entity does not exist or has a stale handle");
        }
        const bool enabled = boolean_field(request, "enabled", true);
        std::optional<MeshRenderer> renderer;
        if (enabled) {
            auto mesh = string_field(request, "mesh");
            auto material = string_field(request, "material");
            if (mesh.empty()) mesh = "builtin.triangle";
            if (material.empty()) material = "builtin.orange";
            if (engine_.assets().find_mesh(mesh) == nullptr ||
                engine_.assets().find_material(material) == nullptr) {
                return error_response(id, "mesh or material asset is not registered");
            }
            renderer = MeshRenderer{std::move(mesh), std::move(material)};
        }
        if (!engine_.scene_history().execute(
                std::string(enabled ? "Configure renderer " : "Remove renderer ") + entity->to_string(),
                [&](Scene& scene) { return scene.set_mesh_renderer(*entity, renderer); })) {
            return error_response(id, "could not update mesh renderer component");
        }
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.set_parent") {
        const auto entity = entity_field(request, "entity");
        const auto parent = entity_field(request, "parent");
        if (!entity.has_value() || !entity->valid() || !parent.has_value()) {
            return error_response(id, "invalid entity or parent handle");
        }
        if (!engine_.scene_history().execute("Reparent " + entity->to_string(), [&](Scene& scene) {
                return scene.set_parent(*entity, *parent);
            })) {
            return error_response(id, "reparenting failed; check handles and hierarchy cycles");
        }
        engine_.logs().write(LogLevel::info, "Reparented entity " + entity->to_string());
        return response_prefix(id) + "{\"entity\":" + engine_.scene().entity_json(*entity) +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "scene.undo") {
        if (!engine_.scene_history().undo()) return error_response(id, "nothing to undo");
        engine_.logs().write(LogLevel::info, "Undid scene transaction");
        return response_prefix(id) + history_json(engine_.scene_history()) + '}';
    }
    if (method == "scene.redo") {
        if (!engine_.scene_history().redo()) return error_response(id, "nothing to redo");
        engine_.logs().write(LogLevel::info, "Redid scene transaction");
        return response_prefix(id) + history_json(engine_.scene_history()) + '}';
    }
    if (method == "scene.snapshot") {
        return response_prefix(id) + engine_.scene().serialize_json() + '}';
    }
    if (method == "scene.save") {
        const auto filename = string_field(request, "filename");
        const auto path = safe_scene_path(engine_, filename);
        if (!path) return error_response(id, "filename must be a safe .relay.json name without directories");
        std::string error;
        if (!save_scene_file_atomic(engine_.scene(), *path, error)) return error_response(id, error);
        engine_.logs().write(LogLevel::info, "Saved scene atomically to " + path->string());
        // Reporting the revision the file holds lets a caller recognise later edits without
        // keeping its own change log, and lets the editor clear its unsaved-work marker.
        return response_prefix(id) + "{\"path\":\"" + escape_json(path->string()) +
               "\",\"version\":" + std::to_string(scene_file_version) +
               ",\"entities\":" + std::to_string(engine_.scene().entities().size()) +
               ",\"revision\":" + std::to_string(engine_.scene_history().revision()) + "}}";
    }
    if (method == "scene.load") {
        const auto filename = string_field(request, "filename");
        const auto path = safe_scene_path(engine_, filename);
        if (!path) return error_response(id, "filename must be a safe .relay.json name without directories");
        auto loaded = load_scene_file(*path);
        if (!loaded) return error_response(id, loaded.error);
        const auto state = std::move(*loaded.state);
        // Imported assets live outside the scene file, so restore them before the scene that
        // references them by id.
        auto reload = reload_imported_assets(engine_.project() ? engine_.project()->root() : std::filesystem::path{"assets"}, engine_.assets());
        if (!engine_.scene_history().execute("Load " + filename, [&](Scene& scene) {
                scene.restore_state(state);
                (void)reload.rebind_scene(scene);
                return true;
            })) {
            return error_response(id, "could not apply loaded scene");
        }
        for (const auto& message : reload.messages) {
            engine_.logs().write(LogLevel::warning, "Imported asset reload: " + message);
        }
        engine_.logs().write(LogLevel::info, "Loaded scene " + path->string());
        return response_prefix(id) + "{\"path\":\"" + escape_json(path->string()) +
               "\",\"source_version\":" + std::to_string(loaded.source_version) +
               ",\"version\":" + std::to_string(scene_file_version) +
               ",\"migrated\":" + (loaded.migrated ? "true" : "false") +
               ",\"entities\":" + std::to_string(loaded.entity_count) +
               ",\"imported_assets\":" + reload.json() +
               ",\"history\":" + history_json(engine_.scene_history()) + "}}";
    }
    if (method == "trace.start") {
        const auto filename = string_field(request, "filename");
        const auto path = safe_trace_path(filename);
        if (!path) return error_response(id, "filename must be a safe .relay-trace.jsonl name without directories");
        std::string error;
        if (!engine_.start_trace(*path, error)) return error_response(id, error);
        return response_prefix(id) + "{\"recording\":true,\"path\":\"" +
               escape_json(path->string()) + "\"}}";
    }
    if (method == "trace.stop") {
        const auto path = engine_.trace().path();
        const auto count = engine_.trace().event_count();
        std::string error;
        if (!engine_.stop_trace(error)) return error_response(id, error);
        return response_prefix(id) + "{\"recording\":false,\"path\":\"" +
               escape_json(path.string()) + "\",\"events\":" + std::to_string(count) + "}}";
    }
    if (method == "trace.status") {
        return response_prefix(id) + "{\"recording\":" +
               (engine_.trace().active() ? "true" : "false") + ",\"events\":" +
               std::to_string(engine_.trace().event_count()) + ",\"path\":\"" +
               escape_json(engine_.trace().path().string()) + "\"}}";
    }
    if (method == "trace.replay") {
        if (replay_active_) return error_response(id, "recursive trace replay is unavailable");
        struct ReplayContext { bool& active; ~ReplayContext() { active = false; } } replay_context{replay_active_};
        replay_active_ = true;
        if (engine_.trace().active()) return error_response(id, "stop the active trace before replaying");
        const auto filename = string_field(request, "filename");
        const auto path = safe_trace_path(filename);
        if (!path) return error_response(id, "filename must be a safe .relay-trace.jsonl name without directories");
        const auto trace = TraceRecorder::load(*path);
        if (!trace) return error_response(id, trace.error);
        if (std::abs(trace.fixed_delta_seconds - engine_.fixed_delta_seconds()) > 1e-12) {
            return error_response(id, "trace fixed timestep does not match this runtime");
        }
        if (trace.random_seed != engine_.random_seed()) {
            return error_response(id, "trace random seed does not match this runtime");
        }
        const auto replay_start = engine_.status().frame_index;
        std::size_t command_count = 0;
        std::size_t input_count = 0;
        for (const auto& event : trace.events) {
            const auto target = replay_start + event.frame;
            const auto current = engine_.status().frame_index;
            if (target > current) engine_.step(static_cast<std::uint32_t>(target - current));
            if (event.kind == "command") {
                if (agent_dispatch_) (void)handle_agent(event.payload);
                else (void)handle(event.payload);
                ++command_count;
            } else {
                engine_.apply_input_event(event.payload);
                ++input_count;
            }
        }
        return response_prefix(id) + "{\"events\":" + std::to_string(trace.events.size()) +
               ",\"commands\":" + std::to_string(command_count) + ",\"inputs\":" +
               std::to_string(input_count) + ",\"final_frame\":" +
               std::to_string(engine_.status().frame_index) + "}}";
    }
    if (method == "logs.read") {
        const auto after = unsigned_field(request, "after", 0);
        const auto entries = engine_.logs().read_after(after);
        std::ostringstream result;
        result << response_prefix(id) << "{\"entries\":[";
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (index != 0) result << ',';
            const auto& entry = entries[index];
            result << "{\"sequence\":" << entry.sequence << ",\"level\":\""
                   << to_string(entry.level) << "\",\"message\":\""
                   << escape_json(entry.message) << "\"}";
        }
        result << "]}}";
        return result.str();
    }

    return error_response(id, "unknown method: " + method);
}

} // namespace relay
