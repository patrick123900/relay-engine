#pragma once
#include <functional>

#include "relay/audio/audio_system.hpp"
#include "relay/core/input.hpp"
#include "relay/core/log.hpp"
#include "relay/observe/capture.hpp"
#include "relay/observe/performance.hpp"
#include "relay/observe/trace.hpp"
#include "relay/physics/collision.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/renderer.hpp"
#include "relay/render/scene_render.hpp"
#include "relay/scene/scene.hpp"
#include "relay/scene/scene_edit.hpp"
#include "relay/scene/project.hpp"
#include "relay/scene/scene_history.hpp"
#include "relay/script/script_system.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>

namespace relay {

struct EngineConfig {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    double fixed_delta_seconds{1.0 / 60.0};
    std::uint64_t random_seed{0x52454C4159ULL};
    bool editor_mode{false};
    // Play sound through the default device. Off for tests and headless sessions, where audio is
    // mixed by the game steps instead and nothing reaches the speakers.
    bool audio_output{false};
};

enum class RuntimeMode { editor, game };

struct EngineStatus {
    bool running{true};
    bool paused{false};
    RuntimeMode mode{RuntimeMode::game};
    std::uint64_t frame_index{0};
    double elapsed_seconds{0.0};
    std::uint32_t width{};
    std::uint32_t height{};
};

class Engine {
public:
    explicit Engine(EngineConfig config = {});
    ~Engine();

    void tick();
    [[nodiscard]] bool run_game();
    [[nodiscard]] bool stop_game();
    [[nodiscard]] bool game_session_active() const { return authored_scene_.has_value(); }
    void step(std::uint32_t frame_count = 1);
    void pause();
    void resume();
    void request_shutdown();

    [[nodiscard]] bool capture(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] std::uint64_t capture_async(const std::filesystem::path& path, std::string& error, std::string source = "deterministic");
    using FrameReceiver = std::function<void(OwnedFrame, std::string)>;
    using GpuFrameSource = std::function<bool(FrameReceiver, std::string&)>;
    void set_gpu_capture_source(GpuFrameSource source, std::function<void()> flush);
    [[nodiscard]] std::string capture_source() const { return gpu_source_ ? "vulkan" : "deterministic"; }
    bool cancel_capture(std::uint64_t id) { return capture_queue_.cancel(id); }
    [[nodiscard]] CaptureJobStatus capture_status(std::uint64_t id) const;
    [[nodiscard]] bool start_video(const std::filesystem::path& path, std::uint32_t fps,
                                   std::uint32_t maximum_frames, std::string& error, std::string source = "deterministic");
    [[nodiscard]] bool stop_video(std::string& error);
    [[nodiscard]] VideoStatus video_status() const;
    void record_render_performance(double gpu_milliseconds, std::uint32_t draw_calls,
                                   std::uint32_t resource_count);
    [[nodiscard]] PerformanceSnapshot performance(std::uint64_t after_frame = 0) const;
    [[nodiscard]] bool start_trace(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] bool stop_trace(std::string& error);
    void record_trace_event(std::string kind, std::string payload);
    [[nodiscard]] TraceRecorder& trace();
    [[nodiscard]] EngineStatus status() const;
    [[nodiscard]] double fixed_delta_seconds() const;
    [[nodiscard]] std::uint64_t random_seed() const;
    void apply_input_event(std::string payload);
    [[nodiscard]] const std::deque<std::string>& recent_input_events() const;
    [[nodiscard]] FrameView frame() const;
    [[nodiscard]] LogBuffer& logs();
    [[nodiscard]] const LogBuffer& logs() const;
    [[nodiscard]] Scene& scene();
    [[nodiscard]] const Scene& scene() const;
    [[nodiscard]] PhysicsWorld& physics() { return physics_; }
    [[nodiscard]] const PhysicsWorld& physics() const { return physics_; }
    [[nodiscard]] SceneHistory& scene_history();
    [[nodiscard]] SceneClipboard& clipboard() { return clipboard_; }
    [[nodiscard]] std::optional<Project>& project() { return project_; }
    [[nodiscard]] const std::optional<Project>& project() const { return project_; }
    [[nodiscard]] AssetRegistry& assets();
    [[nodiscard]] const AssetRegistry& assets() const;
    [[nodiscard]] ScriptSystem& scripts() { return scripts_; }
    [[nodiscard]] AudioSystem& audio();
    // The open project's mixer buses, or the defaults without a project or saved settings.
    [[nodiscard]] AudioSettings audio_settings() const;
    // Validates and saves mixer buses in the open project; the mixer follows immediately.
    [[nodiscard]] bool set_audio_settings(AudioSettings settings, std::string& error);
    // Applies mixer settings without saving, for live fader and effect drags; the next save (or
    // opening another project) ends the preview. audio_settings() reports the previewed values.
    [[nodiscard]] bool preview_audio_settings(AudioSettings settings, std::string& error);
    [[nodiscard]] bool previewing_audio_settings() const { return audio_preview_.has_value(); }
    // Game input: live devices latched once per game step, read through the project's input map.
    [[nodiscard]] InputState& input();
    [[nodiscard]] const InputState& input() const { return input_; }
    // Validates, saves to the open project and applies a new input map.
    [[nodiscard]] bool set_input_map(InputMap map, std::string& error);
    // The open project's graphics settings, or the defaults without a project or saved settings.
    [[nodiscard]] GraphicsSettings graphics_settings() const;
    // Saves graphics settings in the open project. The renderer picks them up on its next frame.
    [[nodiscard]] bool set_graphics_settings(const GraphicsSettings& settings, std::string& error);
    // The state before the latest game step, blended `alpha` of the way to the current one, for
    // drawing between steps. Null outside a running, unpaused game or before its first step.
    [[nodiscard]] const RenderInterpolation* render_interpolation(double alpha);
    // Why the last run_game() refused to start, or empty.
    [[nodiscard]] const std::string& run_game_error() const { return run_game_error_; }

private:
    void advance_one_frame();
    void advance_animations();

    EngineConfig config_;
    SoftwareRenderer renderer_;
    LogBuffer logs_;
    Scene scene_;
    PhysicsWorld physics_;
    SceneHistory scene_history_;
    SceneClipboard clipboard_;
    std::optional<Project> project_;
    AssetRegistry assets_;
    GpuFrameSource gpu_source_;
    std::function<void()> gpu_flush_;
    CaptureQueue capture_queue_;
    VideoRecorder video_{capture_queue_};
    PerformanceTracker performance_;
    TraceRecorder trace_;
    std::deque<std::string> recent_input_events_;
    bool running_{true};
    bool paused_{false};
    RuntimeMode mode_{RuntimeMode::game};
    std::optional<SceneState> authored_scene_;
    std::uint64_t frame_index_{0};
    double elapsed_seconds_{0.0};
    std::string run_game_error_;
    InputState input_;
    RenderInterpolation interpolation_;
    std::optional<std::filesystem::path> input_root_;
    bool input_loaded_{};
    void sync_input_map();
    // Points audio at the open project's files and buses.
    void sync_audio();
    AudioSystem audio_;
    std::optional<AudioSettings> audio_preview_;
    std::optional<std::filesystem::path> audio_root_;
    // Declared last: script callbacks reach every other member, so it is destroyed first.
    ScriptSystem scripts_{*this};
};

} // namespace relay
