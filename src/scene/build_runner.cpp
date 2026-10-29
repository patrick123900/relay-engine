#include "relay/scene/build_runner.hpp"

#include <algorithm>
#include <chrono>

namespace relay {

std::string_view build_state_name(const BuildStatus::State state) {
    switch (state) {
    case BuildStatus::State::idle: return "idle";
    case BuildStatus::State::running: return "running";
    case BuildStatus::State::succeeded: return "succeeded";
    case BuildStatus::State::failed: return "failed";
    case BuildStatus::State::cancelled: return "cancelled";
    }
    return "idle";
}

BuildRunner::~BuildRunner() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool BuildRunner::start(const Project& project, const BuildSettings& settings, const BuildProfile& profile,
                        std::string& error) {
    if (running()) {
        error = "a build is already running";
        return false;
    }
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    {
        std::lock_guard lock(mutex_);
        const auto serial = status_.serial + 1U;
        status_ = {};
        status_.state = BuildStatus::State::running;
        status_.serial = serial;
        status_.profile = profile.name;
        status_.stage = "Starting";
        started_ = std::chrono::steady_clock::now();
    }
    worker_ = std::thread([this, project, settings, profile] {
        BuildControl control;
        control.cancel = &cancel_;
        control.progress = [this](const std::string_view stage, const double fraction, const std::string_view detail) {
            BuildStatus snapshot;
            std::function<void(const BuildStatus&)> observer;
            {
                std::lock_guard lock(mutex_);
                // Parallel compilers report out of order; the bar never goes back.
                status_.fraction = std::max(status_.fraction, std::clamp(fraction, 0.0, 1.0));
                status_.stage = std::string{stage};
                status_.detail = std::string{detail};
                status_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
                snapshot = status_;
                observer = observer_;
            }
            if (observer) observer(snapshot);
        };
        std::string build_error;
        BuildReport report;
        const bool built = build_project(project, settings, profile, build_error, report, &control);
        std::lock_guard lock(mutex_);
        status_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
        if (built) {
            status_.state = BuildStatus::State::succeeded;
            status_.fraction = 1.0;
            status_.stage = "Done";
            status_.detail.clear();
            status_.report = std::move(report);
        } else {
            status_.state = report.cancelled ? BuildStatus::State::cancelled : BuildStatus::State::failed;
            status_.error = std::move(build_error);
        }
    });
    return true;
}

BuildStatus BuildRunner::status() const {
    std::lock_guard lock(mutex_);
    auto copy = status_;
    if (copy.state == BuildStatus::State::running)
        copy.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return copy;
}

void BuildRunner::cancel() { cancel_ = true; }

bool BuildRunner::running() const {
    std::lock_guard lock(mutex_);
    return status_.state == BuildStatus::State::running;
}

void BuildRunner::wait() {
    if (worker_.joinable()) worker_.join();
}

void BuildRunner::set_observer(std::function<void(const BuildStatus&)> observer) {
    std::lock_guard lock(mutex_);
    observer_ = std::move(observer);
}

} // namespace relay
