#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace relay {

struct FramePerformance {
    std::uint64_t frame{};
    double cpu_frame_ms{};
    double gpu_frame_ms{};
    std::uint32_t draw_calls{};
    std::uint32_t render_resources{};
    std::size_t entity_count{};
};

struct PerformanceSnapshot {
    double average_cpu_ms{};
    double maximum_cpu_ms{};
    double latest_gpu_ms{};
    std::uint64_t resident_memory_bytes{};
    std::vector<FramePerformance> samples;
};

class PerformanceTracker {
public:
    explicit PerformanceTracker(std::size_t capacity = 240);
    void record_cpu(std::uint64_t frame, double milliseconds, std::size_t entity_count);
    void record_render(std::uint64_t frame, double gpu_milliseconds, std::uint32_t draw_calls,
                       std::uint32_t resource_count);
    [[nodiscard]] PerformanceSnapshot snapshot(std::uint64_t after_frame = 0) const;

private:
    std::size_t capacity_;
    std::deque<FramePerformance> samples_;
};

} // namespace relay
