#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace relay {

struct ProcessResult {
    bool started{};
    bool timed_out{};
    int exit_code{-1};
    std::string output; // Combined stdout and stderr, truncated at the caller's bound.
};

// Runs an executable directly (no shell) with stdin closed, killing its process group at the
// deadline. The first argument is resolved through PATH.
[[nodiscard]] ProcessResult run_process(const std::vector<std::string>& arguments,
                                        std::chrono::seconds timeout,
                                        std::size_t maximum_output);

} // namespace relay
