#include "relay/core/engine.hpp"
#include <algorithm>
#include <cmath>

#include <chrono>
#include <sstream>

namespace relay {

Engine::Engine(EngineConfig config)
    : config_(config), renderer_(config.width, config.height), scene_history_(scene_) {
    std::ostringstream message;
    message << "Relay runtime initialized at " << config_.width << 'x' << config_.height;
    logs_.write(LogLevel::info, message.str());
}

Engine::~Engine() {
    if (video_.status().recording) {
        std::string ignored_error;
        (void)video_.stop(ignored_error);
    }
}

void Engine::tick() {
    if (running_ && !paused_) {
        advance_one_frame();
    }
}

void Engine::step(const std::uint32_t frame_count) {
    if (!running_) {
        return;
    }
    for (std::uint32_t index = 0; index < frame_count; ++index) {
        advance_one_frame();
    }
}

void Engine::pause() {
    paused_ = true;
    logs_.write(LogLevel::info, "Runtime paused");
}

void Engine::resume() {
    paused_ = false;
    logs_.write(LogLevel::info, "Runtime resumed");
}

void Engine::request_shutdown() {
    if (video_.status().recording) {
        std::string error;
        if (!video_.stop(error)) logs_.write(LogLevel::error, "Video finalization failed: " + error);
    }
    running_ = false;
    logs_.write(LogLevel::info, "Runtime shutdown requested");
}

bool Engine::capture(const std::filesystem::path& path, std::string& error) {
    const bool captured = write_frame_image(renderer_.frame(), path, error);
    if (captured) {
        logs_.write(LogLevel::info, "Captured frame to " + path.string());
    } else {
        logs_.write(LogLevel::error, "Frame capture failed: " + error);
    }
    return captured;
}

std::uint64_t Engine::capture_async(const std::filesystem::path& path, std::string& error) {
    const auto id = capture_queue_.submit(renderer_.frame(), path, error);
    if (id != 0U) logs_.write(LogLevel::info, "Queued frame capture " + std::to_string(id));
    return id;
}

CaptureJobStatus Engine::capture_status(const std::uint64_t id) const {
    return capture_queue_.status(id);
}

bool Engine::start_video(const std::filesystem::path& path, const std::uint32_t fps,
                         const std::uint32_t maximum_frames, std::string& error) {
    return video_.start(path, fps, maximum_frames, config_.fixed_delta_seconds, error);
}

bool Engine::stop_video(std::string& error) { return video_.stop(error); }

VideoStatus Engine::video_status() const { return video_.status(); }

void Engine::record_render_performance(const double gpu_milliseconds, const std::uint32_t draw_calls,
                                       const std::uint32_t resource_count) {
    performance_.record_render(frame_index_, gpu_milliseconds, draw_calls, resource_count);
}

PerformanceSnapshot Engine::performance(const std::uint64_t after_frame) const {
    return performance_.snapshot(after_frame);
}

bool Engine::start_trace(const std::filesystem::path& path, std::string& error) {
    return trace_.start(path, frame_index_, config_.fixed_delta_seconds, config_.random_seed, error);
}

bool Engine::stop_trace(std::string& error) { return trace_.stop(error); }

void Engine::record_trace_event(std::string kind, std::string payload) {
    trace_.record(frame_index_, std::move(kind), std::move(payload));
}

TraceRecorder& Engine::trace() { return trace_; }

EngineStatus Engine::status() const {
    return EngineStatus{
        running_, paused_, frame_index_, elapsed_seconds_, config_.width, config_.height,
    };
}

double Engine::fixed_delta_seconds() const { return config_.fixed_delta_seconds; }
std::uint64_t Engine::random_seed() const { return config_.random_seed; }

void Engine::apply_input_event(std::string payload) {
    trace_.record(frame_index_, "input", payload);
    if (recent_input_events_.size() == 64U) recent_input_events_.pop_front();
    recent_input_events_.push_back(std::move(payload));
}

const std::deque<std::string>& Engine::recent_input_events() const { return recent_input_events_; }

FrameView Engine::frame() const {
    return renderer_.frame();
}

LogBuffer& Engine::logs() {
    return logs_;
}

const LogBuffer& Engine::logs() const {
    return logs_;
}

Scene& Engine::scene() { return scene_; }
const Scene& Engine::scene() const { return scene_; }
SceneHistory& Engine::scene_history() { return scene_history_; }
AssetRegistry& Engine::assets() { return assets_; }
const AssetRegistry& Engine::assets() const { return assets_; }

void Engine::advance_one_frame() {
    const auto start = std::chrono::steady_clock::now();
    ++frame_index_;
    elapsed_seconds_ += config_.fixed_delta_seconds;
    for (const auto entity : scene_.entities()) {
        auto *record = scene_.get(entity);
        if (!record->animator || !record->animator->playing)
            continue;
        auto &animator = *record->animator;
        const auto *model = assets_.find_model(animator.model);
        if (!model || animator.clip >= model->clips.size())
            continue;
        const auto duration = model->clips[animator.clip].duration_seconds;
        animator.time_seconds += config_.fixed_delta_seconds * animator.speed;
        if (duration <= 0.0) {
            animator.time_seconds = 0.0;
            animator.playing = false;
        } else if (animator.loop) {
            animator.time_seconds = std::fmod(animator.time_seconds, duration);
            if (animator.time_seconds < 0.0)
                animator.time_seconds += duration;
        } else {
            if (animator.time_seconds <= 0.0 || animator.time_seconds >= duration)
                animator.playing = false;
            animator.time_seconds = std::clamp(animator.time_seconds, 0.0, duration);
        }
    }
    renderer_.render(frame_index_, elapsed_seconds_);
    video_.record(renderer_.frame());
    const auto end = std::chrono::steady_clock::now();
    const auto milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    performance_.record_cpu(frame_index_, milliseconds, scene_.entities().size());
}

} // namespace relay
