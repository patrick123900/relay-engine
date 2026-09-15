#include "relay/observe/performance.hpp"

#include <algorithm>
#include <fstream>
#include <numeric>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

namespace relay {
namespace {

std::uint64_t resident_memory_bytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0
               ? static_cast<std::uint64_t>(counters.WorkingSetSize) : 0U;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                     reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS
               ? static_cast<std::uint64_t>(info.resident_size) : 0U;
#else
    std::ifstream input("/proc/self/statm");
    std::uint64_t pages = 0;
    std::uint64_t resident = 0;
    input >> pages >> resident;
    const auto page_size = sysconf(_SC_PAGESIZE);
    return input && page_size > 0 ? resident * static_cast<std::uint64_t>(page_size) : 0U;
#endif
}

} // namespace

PerformanceTracker::PerformanceTracker(const std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1U)) {}

void PerformanceTracker::record_cpu(const std::uint64_t frame, const double milliseconds,
                                    const std::size_t entity_count) {
    if (samples_.size() == capacity_) samples_.pop_front();
    samples_.push_back(FramePerformance{frame, milliseconds, 0.0, 0U, 0U, entity_count});
}

void PerformanceTracker::record_render(const std::uint64_t frame, const double gpu_milliseconds,
                                       const std::uint32_t draw_calls,
                                       const std::uint32_t resource_count) {
    const auto sample = std::find_if(samples_.rbegin(), samples_.rend(), [frame](const auto& value) {
        return value.frame == frame;
    });
    if (sample == samples_.rend()) return;
    sample->gpu_frame_ms = gpu_milliseconds;
    sample->draw_calls = draw_calls;
    sample->render_resources = resource_count;
}

PerformanceSnapshot PerformanceTracker::snapshot(const std::uint64_t after_frame) const {
    PerformanceSnapshot result;
    result.resident_memory_bytes = resident_memory_bytes();
    double total = 0.0;
    for (const auto& sample : samples_) {
        total += sample.cpu_frame_ms;
        result.maximum_cpu_ms = std::max(result.maximum_cpu_ms, sample.cpu_frame_ms);
        if (sample.gpu_frame_ms > 0.0) result.latest_gpu_ms = sample.gpu_frame_ms;
        if (sample.frame > after_frame) result.samples.push_back(sample);
    }
    if (!samples_.empty()) result.average_cpu_ms = total / static_cast<double>(samples_.size());
    return result;
}

} // namespace relay
