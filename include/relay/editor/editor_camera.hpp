#pragma once

#include "relay/editor/editor_math.hpp"

namespace relay {

// View-only navigation state. Neither navigation nor framing changes scene transforms.
struct EditorCamera {
    Vec3 target{};
    double yaw{0.6}, pitch{0.35}, distance{6.0}, fly_speed{4.0};

    [[nodiscard]] Vec3 offset() const {
        return {distance * std::cos(pitch) * std::sin(yaw), distance * std::sin(pitch),
                distance * std::cos(pitch) * std::cos(yaw)};
    }
    [[nodiscard]] Vec3 position() const {
        const auto value = offset();
        return {target.x + value.x, target.y + value.y, target.z + value.z};
    }
    void turn(double dx, double dy, bool freelook) {
        const auto eye = position();
        yaw -= dx * 0.005;
        pitch = std::clamp(pitch + dy * 0.005, -1.55, 1.55);
        if (freelook) {
            const auto value = offset();
            target = {eye.x - value.x, eye.y - value.y, eye.z - value.z};
        }
    }
    void pan(double dx, double dy, double viewport_height, double fov) {
        const auto view = editor_view(position(), target);
        const auto scale = 2.0 * distance * std::tan(fov * 3.14159265358979323846 / 360.0) /
                           std::max(viewport_height, 1.0);
        target.x += (-view[0] * dx + view[1] * dy) * scale;
        target.y += (-view[4] * dx + view[5] * dy) * scale;
        target.z += (-view[8] * dx + view[9] * dy) * scale;
    }
    void fly(double forward, double right, double up, double dt, double modifier) {
        const auto view = editor_view(position(), target);
        Vec3 direction{-view[2] * forward + view[0] * right,
                       -view[6] * forward + view[4] * right + up,
                       -view[10] * forward + view[8] * right};
        const auto length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                      direction.z * direction.z);
        if (length <= 1e-12) return;
        const auto step = fly_speed * modifier * std::clamp(dt, 0.0, 0.1) / length;
        target.x += direction.x * step;
        target.y += direction.y * step;
        target.z += direction.z * step;
    }
    void wheel(double steps, bool freelook) {
        if (freelook)
            fly_speed = std::clamp(fly_speed * std::pow(1.2, steps), 0.05, 1000.0);
        else
            distance = std::clamp(distance * std::pow(0.9, steps), 0.25, 5000.0);
    }
    void frame(Vec3 minimum, Vec3 maximum, bool has_geometry, double fov, double aspect) {
        target = {(minimum.x + maximum.x) * 0.5, (minimum.y + maximum.y) * 0.5,
                  (minimum.z + maximum.z) * 0.5};
        // Lights/cameras/empty nodes have a position, not a renderable extent. Keep a useful
        // standoff rather than deriving a near-zero distance from numerical bounds noise.
        if (!has_geometry) {
            distance = 3.0;
            return;
        }
        const Vec3 extent{maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z};
        const auto radius =
            0.5 * std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
        const auto half_vertical = fov * 3.14159265358979323846 / 360.0;
        const auto half_horizontal = std::atan(std::tan(half_vertical) * std::max(aspect, 0.001));
        distance = std::clamp(radius * 1.2 / std::sin(std::min(half_vertical, half_horizontal)),
                              0.5, 5000.0);
    }
};

} // namespace relay
