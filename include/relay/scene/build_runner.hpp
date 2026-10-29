#pragma once

#include "relay/scene/project.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace relay {

struct BuildStatus {
    enum class State : std::uint8_t { idle, running, succeeded, failed, cancelled };
    State state{State::idle};
    std::uint64_t serial{}; // Counts builds started; a new one begins a new serial.
    std::string profile;
    std::string stage;      // "Compiling scripts"
    std::string detail;     // "3 of 12 files"
    double fraction{};      // How far through, 0 to 1.
    double seconds{};       // Elapsed while running, total once finished.
    std::string error;      // Why it failed or stopped.
    BuildReport report;     // What it made, once it succeeded.
};
[[nodiscard]] std::string_view build_state_name(BuildStatus::State state);

// Builds a game on a worker thread, so an editor keeps drawing while scripts compile. One build at
// a time: it works from copies of the project and its settings taken when it starts, and reads the
// project's saved files as it goes.
class BuildRunner {
public:
    BuildRunner() = default;
    ~BuildRunner();
    BuildRunner(const BuildRunner&) = delete;
    BuildRunner& operator=(const BuildRunner&) = delete;

    // False, with `error`, when a build is already running.
    [[nodiscard]] bool start(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                             std::string& error);
    [[nodiscard]] BuildStatus status() const;
    // Asks the running build to stop; it does so at its next check and leaves the earlier build as it was.
    void cancel();
    // Blocks until the running build is over (returns at once when none is).
    void wait();
    [[nodiscard]] bool running() const;
    // Tests: called on the worker thread each time the build reports progress, before the status
    // changes hands, so a test can hold or cancel the build at a known stage.
    void set_observer(std::function<void(const BuildStatus&)> observer);

private:
    void finish();

    mutable std::mutex mutex_;
    BuildStatus status_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    std::function<void(const BuildStatus&)> observer_;
    std::chrono::steady_clock::time_point started_{};
};

} // namespace relay
