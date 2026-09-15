#pragma once

#include "relay/render/renderer.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace relay {

struct OwnedFrame {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] FrameView view() const { return {width, height, rgba}; }
};

[[nodiscard]] bool write_frame_image(FrameView frame, const std::filesystem::path& path,
                                     std::string& error);
[[nodiscard]] bool video_encoder_available();

enum class CaptureJobState { queued, writing, complete, failed };

struct CaptureJobStatus {
    std::uint64_t id{};
    CaptureJobState state{CaptureJobState::queued};
    std::filesystem::path path;
    std::string error;
};

class CaptureQueue {
public:
    explicit CaptureQueue(std::size_t capacity = 8);
    ~CaptureQueue();
    CaptureQueue(const CaptureQueue&) = delete;
    CaptureQueue& operator=(const CaptureQueue&) = delete;

    [[nodiscard]] std::uint64_t submit(FrameView frame, std::filesystem::path path,
                                       std::string& error);
    [[nodiscard]] CaptureJobStatus status(std::uint64_t id) const;
    void wait_idle();

private:
    struct Job {
        CaptureJobStatus status;
        OwnedFrame frame;
    };
    void run();

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable idle_;
    std::deque<std::uint64_t> pending_;
    std::map<std::uint64_t, Job> jobs_;
    std::thread worker_;
    std::uint64_t next_id_{1};
    bool stopping_{false};
    bool writing_{false};
};

struct VideoStatus {
    bool recording{false};
    std::filesystem::path path;
    std::uint32_t fps{30};
    std::uint32_t submitted_frames{0};
    std::uint32_t dropped_frames{0};
};

class VideoRecorder {
public:
    explicit VideoRecorder(CaptureQueue& captures);
    [[nodiscard]] bool start(std::filesystem::path path, std::uint32_t fps,
                             std::uint32_t maximum_frames, double fixed_delta_seconds,
                             std::string& error);
    void record(FrameView frame);
    [[nodiscard]] bool stop(std::string& error);
    [[nodiscard]] VideoStatus status() const;

private:
    CaptureQueue& captures_;
    VideoStatus status_;
    std::filesystem::path temporary_directory_;
    std::vector<std::uint64_t> jobs_;
    std::uint32_t maximum_frames_{};
    double source_fps_{};
    double sampling_accumulator_{};
};

} // namespace relay
