#include "relay/control/control_protocol.hpp"
#include "relay/control/generated_protocol.hpp"

#include "relay/core/engine.hpp"
#include "relay/core/json.hpp"
#include "relay/editor/editor_math.hpp"
#include "relay/scene/scene_edit.hpp"
#include "relay/scene/project.hpp"
#include "relay/render/vulkan_device.hpp"
#include "relay/render/asset_manifest.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/render_graph.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/scene/scene_io.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
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
    const auto* const specification = find_protocol_method(method);
    const bool read_only = specification != nullptr && specification->read_only;
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
               << ",\"frame\":" << status.frame_index
               << ",\"elapsed_seconds\":" << status.elapsed_seconds
               << ",\"fixed_delta_seconds\":" << engine_.fixed_delta_seconds()
               << ",\"random_seed\":" << engine_.random_seed()
               << ",\"width\":" << status.width << ",\"height\":" << status.height << "}}";
        return result.str();
    }
    if (method == "runtime.pause") {
        engine_.pause();
        return response_prefix(id) + "{\"paused\":true}}";
    }
    if (method == "runtime.resume") {
        engine_.resume();
        return response_prefix(id) + "{\"paused\":false}}";
    }
    if (method == "runtime.step") {
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
        auto name = string_field(request, "name");
        if (name.empty()) name = "Entity";
        const auto parent = entity_field(request, "parent");
        if (!parent.has_value() || (parent->valid() && !engine_.scene().contains(*parent))) {
            return error_response(id, "parent does not exist or has a stale handle");
        }
        Entity created{};
        const bool changed = engine_.scene_history().execute("Create " + name, [&](Scene& scene) {
            created = scene.create(name, *parent);
            return created.valid();
        });
        if (!changed) return error_response(id, "could not create entity");
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
               (engine_.project() ? engine_.project()->json() : "null") + "}}";
    }
    if (method == "project.list") {
        std::string files = "[";
        for (const auto& file : available_projects()) {
            if (files.size() > 1) files += ',';
            files += '\"' + escape_json(file) + '\"';
        }
        return response_prefix(id) + "{\"projects\":" + files + "]}}";
    }
    if (method == "project.close") {
        engine_.project().reset();
        return response_prefix(id) + "{\"project\":null}}";
    }
    if (method == "project.create" || method == "project.open") {
        std::string error;
        const auto filename = string_field(request, "filename");
        auto project = method == "project.open" ? load_project(filename, error)
                                                : std::optional{Project{filename, string_field(request, "name"), {}, {}}};
        if (!project) return error_response(id, error);
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
        return response_prefix(id) + "{\"project\":" + engine_.project()->json() +
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
        return response_prefix(id) + "{\"project\":" + engine_.project()->json() + "}}";
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
            if (camera->far_plane <= camera->near_plane) {
                return error_response(id, "camera far_plane must be greater than near_plane");
            }
        }
        if (!engine_.scene_history().execute(
                std::string(enabled ? "Configure camera " : "Remove camera ") + entity->to_string(),
                [&](Scene& scene) { return scene.set_camera(*entity, camera); })) {
            return error_response(id, "could not update camera component");
        }
        engine_.logs().write(LogLevel::info,
                             std::string(enabled ? "Configured camera for " : "Removed camera from ") +
                                 entity->to_string());
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
            });
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
                "Configure light", [&](Scene &scene) { return scene.set_light(*entity, light); });
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
