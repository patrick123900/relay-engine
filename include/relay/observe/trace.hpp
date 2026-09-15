#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

struct TraceEvent {
    std::uint64_t frame{};
    std::string kind;
    std::string payload;
};

struct TraceLoadResult {
    std::vector<TraceEvent> events;
    double fixed_delta_seconds{};
    std::uint64_t random_seed{};
    std::string error;
    [[nodiscard]] explicit operator bool() const { return error.empty(); }
};

class TraceRecorder {
public:
    [[nodiscard]] bool start(std::filesystem::path path, std::uint64_t current_frame,
                             double fixed_delta_seconds, std::uint64_t random_seed,
                             std::string& error);
    [[nodiscard]] bool stop(std::string& error);
    void record(std::uint64_t current_frame, std::string kind, std::string payload);
    [[nodiscard]] bool active() const;
    [[nodiscard]] std::size_t event_count() const;
    [[nodiscard]] const std::filesystem::path& path() const;

    [[nodiscard]] static TraceLoadResult load(const std::filesystem::path& path);

private:
    std::filesystem::path path_;
    std::uint64_t starting_frame_{};
    double fixed_delta_seconds_{};
    std::uint64_t random_seed_{};
    std::vector<TraceEvent> events_;
    bool active_{false};
};

} // namespace relay
