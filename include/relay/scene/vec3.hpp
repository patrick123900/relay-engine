#pragma once

#include <compare>

namespace relay {

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};

    auto operator<=>(const Vec3&) const = default;
};

} // namespace relay
