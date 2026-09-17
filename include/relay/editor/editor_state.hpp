#pragma once

#include "relay/core/json.hpp"

#include <array>
#include <string_view>

namespace relay {

// Protocol vectors in scene components are objects; bounds responses use arrays.
inline std::array<double, 3> editor_vector(const JsonValue::Object& object, std::string_view name,
                                           std::array<double, 3> fallback) {
    const auto* value = field(object, name);
    if (!value) return fallback;
    std::array<double, 3> result{};
    constexpr std::array<std::string_view, 3> axes{"x", "y", "z"};
    for (std::size_t axis = 0; axis < result.size(); ++axis) {
        const JsonValue* coordinate = nullptr;
        if (const auto* vector = value->object())
            coordinate = field(*vector, axes[axis]);
        else if (const auto* array = value->array(); array && array->size() == 3)
            coordinate = &(*array)[axis];
        if (!coordinate || !coordinate->number()) return fallback;
        result[axis] = *coordinate->number();
    }
    return result;
}

// Hold a draft throughout activation and the release frame. When idle, follow the latest runtime
// state, including undo/redo and agent edits. Widgets commit only their own changed channels.
struct EditorScalarDraft {
    double value{};
    bool active{};
    double& begin(double current) {
        if (!active) value = current;
        return value;
    }
    void finish(bool is_active) { active = is_active; }
};

} // namespace relay
