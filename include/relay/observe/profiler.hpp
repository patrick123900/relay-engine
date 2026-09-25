#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace relay {

// Work is time spent computing. Wait is time spent blocked on the GPU, the display or frame
// pacing; it tells a slow frame caused by the CPU apart from one that is waiting for something.
enum class ProfileKind : std::uint8_t { work, wait };

// One scope in one frame. Repeated scopes with the same name under the same parent merge into a
// single node that counts its calls, so a frame's nodes form a call tree in creation order.
struct ProfileNode {
    std::uint32_t name{};
    std::int32_t parent{-1};
    std::int32_t first_child{-1};
    std::int32_t next_sibling{-1};
    std::uint16_t depth{};
    std::uint32_t calls{};
    double milliseconds{};
};

struct ProfileGpuPass {
    std::uint32_t name{};
    double milliseconds{};
};

struct ProfileFrame {
    std::uint64_t index{};
    std::uint64_t engine_frame{};
    bool game{};
    double milliseconds{};
    // Negative until the GPU timings for this frame have been read back.
    double gpu_milliseconds{-1.0};
    // Node 0 is the whole frame; its self time is the time no scope accounted for.
    std::vector<ProfileNode> nodes;
    std::vector<ProfileGpuPass> gpu_passes;
};

struct ProfileReportScope {
    std::string name;
    std::int32_t parent{-1};
    std::uint16_t depth{};
    ProfileKind kind{ProfileKind::work};
    // Per-frame averages over the frames that were reported.
    double total_ms{};
    double self_ms{};
    double calls{};
    // The largest total in any single reported frame.
    double max_ms{};
};

struct ProfileReportHotspot {
    std::string name;
    ProfileKind kind{ProfileKind::work};
    double self_ms{};
    double total_ms{};
    double calls{};
};

struct ProfileReportPass {
    std::string name;
    double average_ms{};
    double max_ms{};
};

struct ProfileReportFrame {
    std::uint64_t index{};
    double milliseconds{};
    double gpu_milliseconds{-1.0};
    bool game{};
};

struct ProfileReport {
    bool paused{};
    std::size_t capacity{};
    std::size_t frames{};
    std::uint64_t first_frame{};
    std::uint64_t last_frame{};
    double average_ms{};
    double minimum_ms{};
    double maximum_ms{};
    double p95_ms{};
    double fps{};
    // Average GPU time over the reported frames that have GPU timings; negative when none do.
    double gpu_ms{-1.0};
    double wait_ms{};
    double cpu_ms{};
    // "cpu", "gpu", "display" or "unknown" when no frames were reported.
    std::string bottleneck{"unknown"};
    std::vector<ProfileReportScope> scopes;       // Depth first, children by total time.
    std::vector<ProfileReportHotspot> hotspots;   // By self time, merged across call paths.
    std::vector<ProfileReportPass> gpu_passes;    // In submission order.
    std::vector<ProfileReportFrame> history;      // Oldest first.
};

struct ProfileQuery {
    std::size_t frames{120};      // The most recent frames to aggregate.
    std::uint64_t frame{};        // A single frame to report instead, or 0.
    bool game_only{};             // Aggregate only frames recorded while the game was running.
    std::size_t history{};        // Recent frame times to include, regardless of the filter.
};

// A frame profiler for the main loop. Scopes are recorded only on the thread that opened the
// frame and only between begin_frame and end_frame; elsewhere they cost a branch. Reads are safe
// from any thread.
class Profiler {
public:
    static constexpr std::size_t capacity = 1200;
    static constexpr std::size_t maximum_nodes = 4096;

    Profiler();

    // Returns a stable id for a scope name. Ids are cheap to record; call sites cache them.
    [[nodiscard]] std::uint32_t intern(std::string_view name, ProfileKind kind = ProfileKind::work);
    [[nodiscard]] std::string name(std::uint32_t id) const;
    [[nodiscard]] ProfileKind kind(std::uint32_t id) const;

    void begin_frame();
    void end_frame(std::uint64_t engine_frame, bool game);
    [[nodiscard]] bool begin_scope(std::uint32_t name);
    void end_scope();
    // The frame currently being recorded, or 0 outside a frame. GPU timings are reported against it.
    [[nodiscard]] std::uint64_t current_frame() const;
    void record_gpu(std::uint64_t frame, const std::vector<ProfileGpuPass>& passes,
                    double total_milliseconds);

    // A paused profiler keeps the frames it has and records no new ones, so they can be inspected.
    void set_paused(bool paused);
    [[nodiscard]] bool paused() const;
    void clear();

    [[nodiscard]] ProfileReport report(const ProfileQuery& query) const;
    [[nodiscard]] std::optional<ProfileFrame> frame(std::uint64_t index) const;

private:
    using Clock = std::chrono::steady_clock;
    struct OpenScope {
        std::int32_t node{};
        Clock::time_point start;
    };

    [[nodiscard]] const ProfileFrame* find_frame(std::uint64_t index) const;

    mutable std::mutex names_mutex_;
    std::vector<std::string> names_;
    std::vector<ProfileKind> kinds_;
    std::unordered_map<std::string, std::uint32_t> ids_;

    // Recording state, owned by the thread that opened the frame.
    std::thread::id owner_;
    bool recording_{};
    std::uint64_t next_index_{1};
    ProfileFrame building_;
    std::vector<OpenScope> open_;
    std::uint32_t frame_name_{};

    mutable std::mutex frames_mutex_;
    std::vector<ProfileFrame> ring_;
    std::size_t ring_head_{};
    std::size_t ring_size_{};
    bool paused_{};
};

// The process-wide profiler used by the engine, the renderer and the editor loop.
[[nodiscard]] Profiler& profiler();

class ProfileScope {
public:
    explicit ProfileScope(const std::uint32_t name) : active_(profiler().begin_scope(name)) {}
    ~ProfileScope() {
        if (active_) profiler().end_scope();
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    bool active_;
};

} // namespace relay

#define RELAY_PROFILE_CONCAT_INNER(a, b) a##b
#define RELAY_PROFILE_CONCAT(a, b) RELAY_PROFILE_CONCAT_INNER(a, b)
// Times the rest of the enclosing block under a fixed name.
#define RELAY_PROFILE_SCOPE(label)                                                               \
    static const std::uint32_t RELAY_PROFILE_CONCAT(relay_profile_id_, __LINE__) =              \
        ::relay::profiler().intern(label);                                                       \
    const ::relay::ProfileScope RELAY_PROFILE_CONCAT(relay_profile_scope_, __LINE__)(            \
        RELAY_PROFILE_CONCAT(relay_profile_id_, __LINE__))
// Times the rest of the enclosing block as waiting rather than work.
#define RELAY_PROFILE_WAIT(label)                                                                \
    static const std::uint32_t RELAY_PROFILE_CONCAT(relay_profile_id_, __LINE__) =              \
        ::relay::profiler().intern(label, ::relay::ProfileKind::wait);                           \
    const ::relay::ProfileScope RELAY_PROFILE_CONCAT(relay_profile_scope_, __LINE__)(            \
        RELAY_PROFILE_CONCAT(relay_profile_id_, __LINE__))
