#include "relay/core/engine.hpp"
#include <algorithm>
#include <cmath>

#include <chrono>
#include <sstream>

namespace relay {

Engine::Engine(EngineConfig config)
    : config_(config), renderer_(config.width, config.height), scene_history_(scene_) {
    mode_ = config.editor_mode ? RuntimeMode::editor : RuntimeMode::game;
    physics_.set_assets(&assets_);
    std::ostringstream message;
    message << "Relay runtime initialized at " << config_.width << 'x' << config_.height;
    logs_.write(LogLevel::info, message.str());
}

Engine::~Engine() {
    if (gpu_flush_) gpu_flush_();
    if (video_.status().recording) {
        std::string ignored_error;
        (void)stop_video(ignored_error);
    }
}

void Engine::tick() {
    scripts_.poll();
    sync_input_map();
    if (running_ && mode_ == RuntimeMode::game && !paused_) {
        advance_one_frame();
    }
}

bool Engine::run_game() {
    run_game_error_.clear();
    if (!running_ || mode_ != RuntimeMode::editor) {
        run_game_error_ = "game can only start from editor mode";
        return false;
    }
    if (!scripts_.can_start(scene_, run_game_error_)) return false;
    sync_input_map();
    input_.clear_edges();
    authored_scene_ = scene_.capture_state();
    physics_.reset();
    mode_ = RuntimeMode::game;
    paused_ = false;
    frame_index_ = 0;
    elapsed_seconds_ = 0.0;
    logs_.write(LogLevel::info, "Game started");
    scripts_.start();
    return true;
}

bool Engine::stop_game() {
    if (!authored_scene_ || mode_ != RuntimeMode::game) return false;
    if (video_.status().recording) {
        std::string error;
        if (!stop_video(error)) return false;
    }
    scripts_.stop();
    scene_.restore_state(std::move(*authored_scene_));
    physics_.reset();
    authored_scene_.reset();
    mode_ = RuntimeMode::editor;
    paused_ = false;
    frame_index_ = 0;
    elapsed_seconds_ = 0.0;
    renderer_.render(frame_index_, elapsed_seconds_);
    logs_.write(LogLevel::info, "Game stopped; authored scene restored");
    return true;
}

void Engine::step(const std::uint32_t frame_count) {
    if (!running_ || mode_ != RuntimeMode::game) {
        return;
    }
    for (std::uint32_t index = 0; index < frame_count; ++index) {
        advance_one_frame();
    }
}

void Engine::pause() {
    if (mode_ != RuntimeMode::game) return;
    paused_ = true;
    logs_.write(LogLevel::info, "Runtime paused");
}

void Engine::resume() {
    if (mode_ != RuntimeMode::game) return;
    paused_ = false;
    logs_.write(LogLevel::info, "Runtime resumed");
}

void Engine::request_shutdown() {
    if (video_.status().recording) {
        std::string error;
        if (!stop_video(error)) logs_.write(LogLevel::error, "Video finalization failed: " + error);
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

void Engine::set_gpu_capture_source(GpuFrameSource source, std::function<void()> flush) {
    if (gpu_flush_) gpu_flush_();
    gpu_source_ = std::move(source);
    gpu_flush_ = std::move(flush);
}

std::uint64_t Engine::capture_async(const std::filesystem::path& path, std::string& error, std::string source) {
    if (source != "vulkan" && source != "deterministic") { error = "unknown capture source"; return 0; }
    if (source == "vulkan") {
        if (!gpu_source_) { error = "Vulkan capture requires a live GPU renderer"; return 0; }
        const auto id = capture_queue_.reserve(path, source, error);
        if (!id) return 0;
        if (!gpu_source_([this, id](OwnedFrame frame, std::string failure) {
            capture_queue_.deliver(id, std::move(frame), std::move(failure));
        }, error)) capture_queue_.deliver(id, {}, error);
        return id;
    }
    const auto id = capture_queue_.submit(renderer_.frame(), path, error);
    if (id != 0U) logs_.write(LogLevel::info, "Queued frame capture " + std::to_string(id));
    return id;
}

CaptureJobStatus Engine::capture_status(const std::uint64_t id) const {
    return capture_queue_.status(id);
}

bool Engine::start_video(const std::filesystem::path& path, const std::uint32_t fps,
                         const std::uint32_t maximum_frames, std::string& error, std::string source) {
    if (source != "vulkan" && source != "deterministic") { error = "unknown video source"; return false; }
    if (source == "vulkan" && !gpu_source_) { error = "Vulkan video requires a live GPU renderer"; return false; }
    if (!video_.start(path, fps, maximum_frames, config_.fixed_delta_seconds, error)) return false;
    video_.set_source(std::move(source));
    return true;
}

bool Engine::stop_video(std::string& error) { if (gpu_flush_) gpu_flush_(); return video_.stop(error); }

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
        running_, paused_, mode_, frame_index_, elapsed_seconds_, config_.width, config_.height,
    };
}

double Engine::fixed_delta_seconds() const { return config_.fixed_delta_seconds; }
std::uint64_t Engine::random_seed() const { return config_.random_seed; }

void Engine::apply_input_event(std::string payload) {
    trace_.record(frame_index_, "input", payload);
    input_.apply(payload);
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

InputState& Engine::input() {
    sync_input_map();
    return input_;
}

// Applies the open project's input map when the project changes; standalone sessions and projects
// that never saved one use the defaults.
void Engine::sync_input_map() {
    std::optional<std::filesystem::path> root;
    if (project_) root = std::filesystem::absolute(project_->root()).lexically_normal();
    if (input_loaded_ && root == input_root_) return;
    input_loaded_ = true;
    input_root_ = root;
    input_.set_map(project_ && project_->input ? *project_->input : default_input_map());
}

bool Engine::set_input_map(InputMap map, std::string& error) {
    sync_input_map();
    if (!project_) {
        error = "the input map is saved with a project; open one first";
        return false;
    }
    auto updated = *project_;
    updated.input = map;
    if (!save_project(updated, error)) return false;
    *project_ = std::move(updated);
    input_.set_map(std::move(map));
    return true;
}
const AssetRegistry& Engine::assets() const { return assets_; }

void Engine::advance_one_frame() {
    const auto start = std::chrono::steady_clock::now();
    ++frame_index_;
    elapsed_seconds_ += config_.fixed_delta_seconds;
    input_.begin_step();
    scripts_.update(config_.fixed_delta_seconds);
    for (const auto entity : scene_.entities()) {
        auto *record = scene_.get(entity);
        if (record->transform_animation && record->transform_animation->playing) {
            auto& animation = *record->transform_animation;
            animation.time_seconds += config_.fixed_delta_seconds * animation.speed;
            if (animation.loop) {
                animation.time_seconds = std::fmod(animation.time_seconds,
                                                   animation.duration_seconds);
                if (animation.time_seconds < 0.0)
                    animation.time_seconds += animation.duration_seconds;
            } else {
                if (animation.time_seconds <= 0.0 ||
                    animation.time_seconds >= animation.duration_seconds)
                    animation.playing = false;
                animation.time_seconds = std::clamp(animation.time_seconds, 0.0,
                                                    animation.duration_seconds);
            }
        }
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
    physics_.step(scene_, config_.fixed_delta_seconds);
    scripts_.dispatch_contacts();
    renderer_.render(frame_index_, elapsed_seconds_);
    if (video_.status().recording && video_.status().source == "vulkan") {
        if (video_.sample_due()) {
            std::string error;
            if (!gpu_source_([this](OwnedFrame frame, std::string failure) {
                if (failure.empty()) video_.record_sample(frame.view()); else video_.drop();
            }, error)) video_.drop();
        }
    } else video_.record(renderer_.frame());
    const auto end = std::chrono::steady_clock::now();
    const auto milliseconds = std::chrono::duration<double, std::milli>(end - start).count();
    performance_.record_cpu(frame_index_, milliseconds, scene_.entities().size());
}

} // namespace relay
