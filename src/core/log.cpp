#include "relay/core/log.hpp"

#include <algorithm>

namespace relay {

LogBuffer::LogBuffer(const std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {
    entries_.reserve(capacity_);
}

void LogBuffer::write(const LogLevel level, const std::string_view message) {
    std::scoped_lock lock(mutex_);
    if (entries_.size() == capacity_) {
        entries_.erase(entries_.begin());
    }
    entries_.push_back(LogEntry{next_sequence_++, level, std::string(message)});
}

std::vector<LogEntry> LogBuffer::read_after(const std::uint64_t sequence) const {
    std::scoped_lock lock(mutex_);
    std::vector<LogEntry> result;
    for (const auto& entry : entries_) {
        if (entry.sequence > sequence) {
            result.push_back(entry);
        }
    }
    return result;
}

std::uint64_t LogBuffer::latest_sequence() const {
    std::scoped_lock lock(mutex_);
    return next_sequence_ - 1;
}

std::string_view to_string(const LogLevel level) {
    switch (level) {
    case LogLevel::debug: return "debug";
    case LogLevel::info: return "info";
    case LogLevel::warning: return "warning";
    case LogLevel::error: return "error";
    }
    return "unknown";
}

} // namespace relay

