#include "relay/observe/trace.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace relay {
namespace {

constexpr std::size_t maximum_events = 100'000U;
constexpr std::uintmax_t maximum_trace_bytes = 32U * 1024U * 1024U;

std::string hex_encode(const std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2U);
    for (const auto character : input) {
        const auto byte = static_cast<unsigned char>(character);
        output += digits[byte >> 4U];
        output += digits[byte & 0x0FU];
    }
    return output;
}

std::optional<std::string> hex_decode(const std::string_view input) {
    if (input.size() % 2U != 0U) return std::nullopt;
    auto nibble = [](const char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    std::string output;
    output.reserve(input.size() / 2U);
    for (std::size_t index = 0; index < input.size(); index += 2U) {
        const auto high = nibble(input[index]);
        const auto low = nibble(input[index + 1U]);
        if (high < 0 || low < 0) return std::nullopt;
        output += static_cast<char>((high << 4) | low);
    }
    return output;
}

std::optional<std::string_view> quoted_field(const std::string_view line, const std::string_view name) {
    const std::string marker = "\"" + std::string(name) + "\":\"";
    const auto start = line.find(marker);
    if (start == std::string_view::npos) return std::nullopt;
    const auto value_start = start + marker.size();
    const auto end = line.find('"', value_start);
    return end == std::string_view::npos ? std::nullopt
                                         : std::optional{line.substr(value_start, end - value_start)};
}

bool unsigned_field(const std::string_view line, const std::string_view name, std::uint64_t& output) {
    const std::string marker = "\"" + std::string(name) + "\":";
    const auto found = line.find(marker);
    if (found == std::string_view::npos) return false;
    const auto start = found + marker.size();
    auto end = start;
    while (end < line.size() && line[end] >= '0' && line[end] <= '9') ++end;
    const auto parsed = std::from_chars(line.data() + start, line.data() + end, output);
    return end > start && parsed.ec == std::errc{} && parsed.ptr == line.data() + end;
}

bool replace_file(const std::filesystem::path& temporary, const std::filesystem::path& destination,
                  std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(temporary.c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        error = "could not atomically replace trace file";
        return false;
    }
#else
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = "could not atomically replace trace file";
        return false;
    }
#endif
    return true;
}

} // namespace

bool TraceRecorder::start(std::filesystem::path path, const std::uint64_t current_frame,
                          const double fixed_delta_seconds, const std::uint64_t random_seed,
                          std::string& error) {
    if (active_) {
        error = "a trace is already recording";
        return false;
    }
    path_ = std::move(path);
    starting_frame_ = current_frame;
    fixed_delta_seconds_ = fixed_delta_seconds;
    random_seed_ = random_seed;
    events_.clear();
    active_ = true;
    error.clear();
    return true;
}

bool TraceRecorder::stop(std::string& error) {
    if (!active_) {
        error = "no trace is recording";
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(path_.parent_path(), filesystem_error);
    if (filesystem_error) {
        error = "could not create trace directory: " + filesystem_error.message();
        return false;
    }
    const auto temporary = path_.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"format\":\"relay.trace\",\"version\":1,\"fixed_delta_seconds\":"
           << fixed_delta_seconds_ << ",\"random_seed\":" << random_seed_ << "}\n";
    for (const auto& event : events_) {
        output << "{\"frame\":" << event.frame << ",\"kind\":\"" << event.kind
               << "\",\"payload_hex\":\"" << hex_encode(event.payload) << "\"}\n";
    }
    output.flush();
    if (!output) {
        error = "could not write trace file";
        output.close();
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    output.close();
    if (!replace_file(temporary, path_, error)) {
        std::filesystem::remove(temporary, filesystem_error);
        return false;
    }
    active_ = false;
    return true;
}

void TraceRecorder::record(const std::uint64_t current_frame, std::string kind, std::string payload) {
    if (!active_ || events_.size() >= maximum_events) return;
    events_.push_back({current_frame - starting_frame_, std::move(kind), std::move(payload)});
}

bool TraceRecorder::active() const { return active_; }
std::size_t TraceRecorder::event_count() const { return events_.size(); }
const std::filesystem::path& TraceRecorder::path() const { return path_; }

TraceLoadResult TraceRecorder::load(const std::filesystem::path& path) {
    TraceLoadResult result;
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error || size > maximum_trace_bytes) {
        result.error = filesystem_error ? "could not inspect trace file" : "trace exceeds the 32 MiB limit";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!std::getline(input, line) || line.find(R"("format":"relay.trace")") == std::string::npos ||
        line.find(R"("version":1)") == std::string::npos) {
        result.error = "trace header is invalid or unsupported";
        return result;
    }
    const std::string delta_marker = R"("fixed_delta_seconds":)";
    const auto delta_start = line.find(delta_marker);
    if (delta_start == std::string::npos) {
        result.error = "trace is missing its fixed timestep";
        return result;
    }
    const auto begin = line.data() + delta_start + delta_marker.size();
    const auto parsed_delta = std::from_chars(begin, line.data() + line.size(), result.fixed_delta_seconds);
    if (parsed_delta.ec != std::errc{} || result.fixed_delta_seconds <= 0.0) {
        result.error = "trace fixed timestep is invalid";
        return result;
    }
    if (!unsigned_field(line, "random_seed", result.random_seed)) {
        result.error = "trace random seed is invalid";
        return result;
    }
    std::uint64_t previous_frame = 0;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::uint64_t frame = 0;
        const auto kind = quoted_field(line, "kind");
        const auto encoded = quoted_field(line, "payload_hex");
        if (!unsigned_field(line, "frame", frame) || frame > 10'000'000U || !kind || !encoded ||
            (*kind != "command" && *kind != "input") || frame < previous_frame) {
            result.error = "trace event is malformed or out of order";
            return result;
        }
        auto payload = hex_decode(*encoded);
        if (!payload) {
            result.error = "trace event payload is not valid hexadecimal";
            return result;
        }
        result.events.push_back({frame, std::string(*kind), std::move(*payload)});
        previous_frame = frame;
        if (result.events.size() > maximum_events) {
            result.error = "trace exceeds the 100000 event limit";
            return result;
        }
    }
    if (!input.eof()) result.error = "trace read failed";
    return result;
}

} // namespace relay
