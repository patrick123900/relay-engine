#pragma once

#include "relay/core/log.hpp"
#include "relay/observe/capture.hpp"
#include "relay/observe/performance.hpp"
#include "relay/observe/trace.hpp"
#include "relay/render/assets.hpp"
#include "relay/render/renderer.hpp"
#include "relay/scene/scene.hpp"
#include "relay/scene/scene_history.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <string>

namespace relay {

struct EngineConfig {
    std::uint32_t width{1280};
    std::uint32_t height{720};
    double fixed_delta_seconds{1.0 / 60.0};
    std::uint64_t random_seed{0x52454C4159ULL};
};

struct EngineStatus {
    bool running{true};
    bool paused{false};
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
    void step(std::uint32_t frame_count = 1);
    void pause();
    void resume();
    void request_shutdown();

    [[nodiscard]] bool capture(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] std::uint64_t capture_async(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] CaptureJobStatus capture_status(std::uint64_t id) const;
    [[nodiscard]] bool start_video(const std::filesystem::path& path, std::uint32_t fps,
                                   std::uint32_t maximum_frames, std::string& error);
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
    [[nodiscard]] SceneHistory& scene_history();
    [[nodiscard]] AssetRegistry& assets();
    [[nodiscard]] const AssetRegistry& assets() const;

private:
    void advance_one_frame();

    EngineConfig config_;
    SoftwareRenderer renderer_;
    LogBuffer logs_;
    Scene scene_;
    SceneHistory scene_history_;
    AssetRegistry assets_;
    CaptureQueue capture_queue_;
    VideoRecorder video_{capture_queue_};
    PerformanceTracker performance_;
    TraceRecorder trace_;
    std::deque<std::string> recent_input_events_;
    bool running_{true};
    bool paused_{false};
    std::uint64_t frame_index_{0};
    double elapsed_seconds_{0.0};
};

} // namespace relay
