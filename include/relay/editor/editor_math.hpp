#pragma once

// Transform maths shared by the editor's gizmo and viewport camera.
//
// These deliberately do not use the gizmo library's own compose/decompose helpers. Relay stores
// rotations as Euler degrees with a specific order, and a library using a different order would
// silently write back rotations that do not match the value the renderer uses. Composing here
// against Relay's own convention keeps the editor, the scene format and the renderer in agreement,
// and lets the native tests check that agreement directly.

#include "relay/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace relay {

// Column-major 4x4, matching RenderMatrix and GLSL's default mat4 layout.
using EditorMatrix = std::array<float, 16>;

inline EditorMatrix editor_identity() {
    EditorMatrix result{};
    result[0] = result[5] = result[10] = result[15] = 1.0F;
    return result;
}

inline EditorMatrix editor_multiply(const EditorMatrix& left, const EditorMatrix& right) {
    EditorMatrix result{};
    for (std::size_t column = 0; column < 4U; ++column) {
        for (std::size_t row = 0; row < 4U; ++row) {
            float value = 0.0F;
            for (std::size_t inner = 0; inner < 4U; ++inner) {
                value += left[inner * 4U + row] * right[column * 4U + inner];
            }
            result[column * 4U + row] = value;
        }
    }
    return result;
}

// Invert the complete affine parent matrix, including rotated, nonuniform and mirrored scales.
// A collapsed parent has no inverse, so its children cannot be manipulated safely in world space.
inline std::optional<EditorMatrix> editor_inverse_affine(const EditorMatrix& matrix) {
    const double a = matrix[0], b = matrix[4], c = matrix[8];
    const double d = matrix[1], e = matrix[5], f = matrix[9];
    const double g = matrix[2], h = matrix[6], i = matrix[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || determinant == 0.0) return std::nullopt;
    const std::array<double, 9> inverse{
        (e * i - f * h) / determinant, (f * g - d * i) / determinant,
        (d * h - e * g) / determinant, (c * h - b * i) / determinant,
        (a * i - c * g) / determinant, (b * g - a * h) / determinant,
        (b * f - c * e) / determinant, (c * d - a * f) / determinant,
        (a * e - b * d) / determinant};
    auto result = editor_identity();
    for (std::size_t column = 0; column < 3; ++column)
        for (std::size_t row = 0; row < 3; ++row)
            result[column * 4 + row] = static_cast<float>(inverse[column * 3 + row]);
    for (std::size_t row = 0; row < 3; ++row)
        result[12 + row] = -(result[row] * matrix[12] + result[4 + row] * matrix[13] +
                             result[8 + row] * matrix[14]);
    for (const auto value : result)
        if (!std::isfinite(value)) return std::nullopt;
    return result;
}

// Mirrors Scene's local transform: translation * Rz * Ry * Rx * scale, rotations in degrees.
inline EditorMatrix editor_compose(const Vec3& position, const Vec3& rotation_degrees,
                                   const Vec3& scale) {
    constexpr double to_radians = 3.14159265358979323846 / 180.0;
    const auto x = rotation_degrees.x * to_radians;
    const auto y = rotation_degrees.y * to_radians;
    const auto z = rotation_degrees.z * to_radians;
    const auto sx = std::sin(x), cx = std::cos(x);
    const auto sy = std::sin(y), cy = std::cos(y);
    const auto sz = std::sin(z), cz = std::cos(z);

    // Rotation rows of Rz * Ry * Rx.
    const std::array<std::array<double, 3>, 3> rotation{{
        {cy * cz, cz * sy * sx - cx * sz, cz * sy * cx + sx * sz},
        {cy * sz, sz * sy * sx + cx * cz, sz * sy * cx - sx * cz},
        {-sy, cy * sx, cy * cx},
    }};
    const std::array<double, 3> scales{scale.x, scale.y, scale.z};

    EditorMatrix result{};
    for (std::size_t column = 0; column < 3U; ++column) {
        for (std::size_t row = 0; row < 3U; ++row) {
            result[column * 4U + row] = static_cast<float>(rotation[row][column] * scales[column]);
        }
    }
    result[12] = static_cast<float>(position.x);
    result[13] = static_cast<float>(position.y);
    result[14] = static_cast<float>(position.z);
    result[15] = 1.0F;
    return result;
}

// Inverse of editor_compose. Recovers the Euler triple Relay would have used to build the matrix.
inline void editor_decompose(const EditorMatrix& matrix, Vec3& position, Vec3& rotation_degrees,
                             Vec3& scale) {
    constexpr double to_degrees = 180.0 / 3.14159265358979323846;
    const auto column_length = [&matrix](const std::size_t column) {
        const auto x = static_cast<double>(matrix[column * 4U + 0U]);
        const auto y = static_cast<double>(matrix[column * 4U + 1U]);
        const auto z = static_cast<double>(matrix[column * 4U + 2U]);
        return std::sqrt(x * x + y * y + z * z);
    };
    scale = {column_length(0U), column_length(1U), column_length(2U)};
    // A reflected basis needs one negative scale; otherwise decomposition changes handedness.
    const double determinant =
        static_cast<double>(matrix[0]) * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
        static_cast<double>(matrix[4]) * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
        static_cast<double>(matrix[8]) * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    if (determinant < 0) scale.x = -scale.x;
    position = {matrix[12], matrix[13], matrix[14]};

    // Normalize the basis so the rotation can be read back without the scale baked in.
    std::array<double, 9> r{};
    for (std::size_t column = 0; column < 3U; ++column) {
        const auto length = column == 0 ? scale.x : column == 1 ? scale.y : scale.z;
        const auto divisor = std::abs(length) > 1e-12 ? length : 1.0;
        for (std::size_t row = 0; row < 3U; ++row) {
            r[column * 3U + row] = static_cast<double>(matrix[column * 4U + row]) / divisor;
        }
    }
    const auto element = [&r](const std::size_t row, const std::size_t column) {
        return r[column * 3U + row];
    };

    const auto pitch = std::asin(std::clamp(-element(2U, 0U), -1.0, 1.0));
    double roll = 0.0;
    double yaw = 0.0;
    if (std::abs(element(2U, 0U)) < 0.99999) {
        roll = std::atan2(element(2U, 1U), element(2U, 2U));
        yaw = std::atan2(element(1U, 0U), element(0U, 0U));
    } else {
        // Gimbal lock: the X and Z rotations become the same axis, so attribute all of it to X.
        roll = std::atan2(-element(1U, 2U), element(1U, 1U));
        yaw = 0.0;
    }
    rotation_degrees = {roll * to_degrees, pitch * to_degrees, yaw * to_degrees};
}

// Right-handed look-at view matrix, the convention the gizmo library expects.
inline EditorMatrix editor_view(const Vec3& eye, const Vec3& target) {
    const auto normalize = [](const Vec3& value) {
        const auto length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        if (length <= 1e-12) return value;
        return Vec3{value.x / length, value.y / length, value.z / length};
    };
    const auto cross = [](const Vec3& a, const Vec3& b) {
        return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const auto dot = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };

    const auto forward = normalize({target.x - eye.x, target.y - eye.y, target.z - eye.z});
    Vec3 hint{0.0, 1.0, 0.0};
    if (std::abs(forward.y) > 0.9999) hint = {0.0, 0.0, forward.y > 0.0 ? -1.0 : 1.0};
    const auto right = normalize(cross(forward, hint));
    const auto up = cross(right, forward);

    EditorMatrix result = editor_identity();
    result[0] = static_cast<float>(right.x);
    result[4] = static_cast<float>(right.y);
    result[8] = static_cast<float>(right.z);
    result[1] = static_cast<float>(up.x);
    result[5] = static_cast<float>(up.y);
    result[9] = static_cast<float>(up.z);
    result[2] = static_cast<float>(-forward.x);
    result[6] = static_cast<float>(-forward.y);
    result[10] = static_cast<float>(-forward.z);
    result[12] = static_cast<float>(-dot(right, eye));
    result[13] = static_cast<float>(-dot(up, eye));
    result[14] = static_cast<float>(dot(forward, eye));
    return result;
}

// Projection for the gizmo only. The Vulkan backend draws with its own flipped-Y, zero-to-one
// projection; this one has to agree with the gizmo library's screen mapping, which places world
// +Y at the top of the viewport exactly as the rendered image does.
inline EditorMatrix editor_projection(const double field_of_view_y_degrees, const double aspect,
                                      const double near_plane, const double far_plane) {
    constexpr double to_radians = 3.14159265358979323846 / 180.0;
    const auto focal = 1.0 / std::tan(field_of_view_y_degrees * 0.5 * to_radians);
    EditorMatrix result{};
    result[0] = static_cast<float>(focal / std::max(aspect, 0.001));
    result[5] = static_cast<float>(focal);
    result[10] = static_cast<float>((far_plane + near_plane) / (near_plane - far_plane));
    result[11] = -1.0F;
    result[14] = static_cast<float>(2.0 * far_plane * near_plane / (near_plane - far_plane));
    return result;
}

// Ray through normalized viewport coordinates, shared with picking tests. The same camera basis
// drives rendering and gizmos; horizontal/vertical range from -1 to +1, with +Y at screen top.
inline Vec3 editor_screen_ray(const Vec3& eye, const Vec3& target, double field_of_view,
                              double aspect, double horizontal, double vertical) {
    const auto view = editor_view(eye, target);
    const auto tangent = std::tan(field_of_view * 0.5 * 3.14159265358979323846 / 180.0);
    const double x = horizontal * tangent * aspect;
    const double y = vertical * tangent;
    Vec3 direction{view[0] * x + view[1] * y - view[2], view[4] * x + view[5] * y - view[6],
                   view[8] * x + view[9] * y - view[10]};
    const auto length = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                                  direction.z * direction.z);
    if (length > 1e-12) {
        direction.x /= length;
        direction.y /= length;
        direction.z /= length;
    }
    return direction;
}

} // namespace relay
