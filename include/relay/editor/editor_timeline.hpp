#pragma once

#include <algorithm>
#include <cmath>

namespace relay {

// Timeline display FPS is a view preference; animation data and engine timestep remain seconds.
inline double timeline_time(double fraction, double duration, int fps, bool snap) {
    if (!std::isfinite(fraction) || !std::isfinite(duration) || duration <= 0) return 0;
    auto time = std::clamp(fraction, 0.0, 1.0) * duration;
    if (snap && fps > 0) time = std::round(time * fps) / fps;
    return std::clamp(time, 0.0, duration);
}

inline double timeline_step(double time, double duration, int fps, int direction) {
    if (fps <= 0 || duration <= 0 || !std::isfinite(time) || !std::isfinite(duration)) return 0;
    const double frame = time * fps;
    const double next = direction > 0 ? std::floor(frame + 1e-8) + 1
                                      : std::ceil(frame - 1e-8) - 1;
    return std::clamp(next / fps, 0.0, duration);
}

} // namespace relay
