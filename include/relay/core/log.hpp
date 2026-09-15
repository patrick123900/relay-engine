#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

enum class LogLevel { debug, info, warning, error };

struct LogEntry {
    std::uint64_t sequence{};
    LogLevel level{LogLevel::info};
    std::string message;
};

class LogBuffer {
public:
    explicit LogBuffer(std::size_t capacity = 2048);

    void write(LogLevel level, std::string_view message);
    [[nodiscard]] std::vector<LogEntry> read_after(std::uint64_t sequence) const;
    [[nodiscard]] std::uint64_t latest_sequence() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<LogEntry> entries_;
    std::uint64_t next_sequence_{1};
};

[[nodiscard]] std::string_view to_string(LogLevel level);

} // namespace relay

