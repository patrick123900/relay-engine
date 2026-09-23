#include "relay/core/input.hpp"

#include "relay/core/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <system_error>

namespace relay {
namespace {

namespace fs = std::filesystem;

std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const auto colon = text.find(':', start);
        parts.push_back(text.substr(start, colon == std::string_view::npos ? colon : colon - start));
        if (colon == std::string_view::npos) return parts;
        start = colon + 1U;
    }
}

std::optional<double> number(std::string_view text) {
    double value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

bool analog_control(std::string_view id) {
    return id == "gamepad:leftx" || id == "gamepad:lefty" || id == "gamepad:rightx" ||
           id == "gamepad:righty" || id == "gamepad:left_trigger" || id == "gamepad:right_trigger";
}

std::string control_id(std::string_view device, std::string_view name) {
    return std::string{device} + ":" + std::string{name};
}

} // namespace

bool valid_input_name(const std::string_view name) {
    if (name.empty() || name.size() > 64U || (name.front() >= '0' && name.front() <= '9'))
        return false;
    return std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

bool valid_input_control(const std::string_view control) {
    const auto colon = control.find(':');
    if (colon == std::string_view::npos || control.size() > 48U) return false;
    const auto device = control.substr(0, colon);
    const auto name = control.substr(colon + 1U);
    if (device != "key" && device != "mouse" && device != "gamepad") return false;
    return !name.empty() && std::all_of(name.begin(), name.end(), [](const char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

InputMap default_input_map() {
    InputMap map;
    map.actions = {
        {"jump", {"key:space", "gamepad:a"}},
        {"interact", {"key:e", "gamepad:x"}},
        {"fire", {"mouse:left", "gamepad:right_trigger"}},
        {"sprint", {"key:left_shift", "gamepad:left_stick"}},
    };
    map.axes = {
        {"move_x", 0.2, {{"key:a", "key:d", "", 1.0}, {"key:left", "key:right", "", 1.0},
                         {"", "", "gamepad:leftx", 1.0}}},
        // SDL reports stick Y growing downwards; forward is positive here.
        {"move_y", 0.2, {{"key:s", "key:w", "", 1.0}, {"key:down", "key:up", "", 1.0},
                         {"", "", "gamepad:lefty", -1.0}}},
        {"look_x", 0.15, {{"", "", "gamepad:rightx", 1.0}}},
        {"look_y", 0.15, {{"", "", "gamepad:righty", -1.0}}},
    };
    return map;
}

std::optional<InputMap> parse_input_map(const std::string_view json, std::string& error) {
    JsonParser parser(json);
    const auto parsed = parser.parse();
    const auto* root = parsed ? parsed->object() : nullptr;
    const auto* format = root ? field(*root, "format") : nullptr;
    const auto* version = root ? field(*root, "version") : nullptr;
    const auto* actions = root ? field(*root, "actions") : nullptr;
    const auto* axes = root ? field(*root, "axes") : nullptr;
    const auto* lock = root ? field(*root, "lock_mouse") : nullptr;
    if (!format || !format->string() || *format->string() != "relay.input" || !version ||
        !version->number() || *version->number() != 1.0 || !actions || !actions->array() ||
        !axes || !axes->array() || (lock && !lock->boolean())) {
        error = "input map must be a relay.input version 1 object with actions and axes arrays";
        return std::nullopt;
    }
    if (actions->array()->size() > maximum_input_entries || axes->array()->size() > maximum_input_entries) {
        error = "input map exceeds 128 actions or 128 axes";
        return std::nullopt;
    }
    InputMap map;
    map.lock_mouse = lock && *lock->boolean();
    std::set<std::string> names;
    const auto name_of = [&](const JsonValue::Object& object, std::string& target) {
        const auto* value = field(object, "name");
        if (!value || !value->string() || !valid_input_name(*value->string())) {
            error = "input action and axis names must be identifiers of at most 64 characters";
            return false;
        }
        if (!names.insert(*value->string()).second) {
            error = "input name " + *value->string() + " is used twice";
            return false;
        }
        target = *value->string();
        return true;
    };
    const auto control_of = [&](const JsonValue* value, std::string& target) {
        if (!value || !value->string() || !valid_input_control(*value->string())) {
            error = "input controls look like key:space, mouse:left or gamepad:a";
            return false;
        }
        target = *value->string();
        return true;
    };
    for (const auto& item : *actions->array()) {
        const auto* object = item.object();
        const auto* bindings = object ? field(*object, "bindings") : nullptr;
        InputAction action;
        if (!object || !name_of(*object, action.name)) {
            if (error.empty()) error = "invalid input action";
            return std::nullopt;
        }
        if (!bindings || !bindings->array() || bindings->array()->size() > maximum_input_bindings) {
            error = "action " + action.name + " needs a bindings array of at most 16 controls";
            return std::nullopt;
        }
        for (const auto& binding : *bindings->array()) {
            action.bindings.emplace_back();
            if (!control_of(&binding, action.bindings.back())) return std::nullopt;
        }
        map.actions.push_back(std::move(action));
    }
    for (const auto& item : *axes->array()) {
        const auto* object = item.object();
        const auto* bindings = object ? field(*object, "bindings") : nullptr;
        const auto* deadzone = object ? field(*object, "deadzone") : nullptr;
        InputAxis axis;
        if (!object || !name_of(*object, axis.name)) {
            if (error.empty()) error = "invalid input axis";
            return std::nullopt;
        }
        if (deadzone) {
            if (!deadzone->number() || *deadzone->number() < 0.0 || *deadzone->number() > 0.95) {
                error = "axis " + axis.name + " deadzone must be between 0 and 0.95";
                return std::nullopt;
            }
            axis.deadzone = *deadzone->number();
        }
        if (!bindings || !bindings->array() || bindings->array()->size() > maximum_input_bindings) {
            error = "axis " + axis.name + " needs a bindings array of at most 16 bindings";
            return std::nullopt;
        }
        for (const auto& value : *bindings->array()) {
            const auto* binding = value.object();
            if (!binding) {
                error = "axis bindings are objects";
                return std::nullopt;
            }
            InputAxisBinding parsed_binding;
            if (const auto* analog = field(*binding, "analog")) {
                const auto* scale = field(*binding, "scale");
                if (!control_of(analog, parsed_binding.analog)) return std::nullopt;
                if (!analog_control(parsed_binding.analog)) {
                    error = parsed_binding.analog + " is not an analog control";
                    return std::nullopt;
                }
                if (scale) {
                    if (!scale->number() || std::abs(*scale->number()) > 100.0) {
                        error = "analog scale must be a number between -100 and 100";
                        return std::nullopt;
                    }
                    parsed_binding.scale = *scale->number();
                }
            } else if (!control_of(field(*binding, "negative"), parsed_binding.negative) ||
                       !control_of(field(*binding, "positive"), parsed_binding.positive)) {
                error = "axis bindings have negative and positive controls, or an analog control";
                return std::nullopt;
            }
            axis.bindings.push_back(std::move(parsed_binding));
        }
        map.axes.push_back(std::move(axis));
    }
    return map;
}

std::string input_map_json(const InputMap& map) {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"format\": \"relay.input\",\n  \"version\": 1,\n  \"lock_mouse\": "
           << (map.lock_mouse ? "true" : "false") << ",\n  \"actions\": [";
    for (std::size_t index = 0; index < map.actions.size(); ++index) {
        const auto& action = map.actions[index];
        output << (index ? "," : "") << "\n    {\"name\": \"" << json_escape(action.name)
               << "\", \"bindings\": [";
        for (std::size_t item = 0; item < action.bindings.size(); ++item)
            output << (item ? ", " : "") << '"' << json_escape(action.bindings[item]) << '"';
        output << "]}";
    }
    output << (map.actions.empty() ? "" : "\n  ") << "],\n  \"axes\": [";
    for (std::size_t index = 0; index < map.axes.size(); ++index) {
        const auto& axis = map.axes[index];
        output << (index ? "," : "") << "\n    {\"name\": \"" << json_escape(axis.name)
               << "\", \"deadzone\": " << axis.deadzone << ", \"bindings\": [";
        for (std::size_t item = 0; item < axis.bindings.size(); ++item) {
            const auto& binding = axis.bindings[item];
            output << (item ? ", " : "");
            if (!binding.analog.empty())
                output << "{\"analog\": \"" << json_escape(binding.analog)
                       << "\", \"scale\": " << binding.scale << '}';
            else
                output << "{\"negative\": \"" << json_escape(binding.negative)
                       << "\", \"positive\": \"" << json_escape(binding.positive) << "\"}";
        }
        output << "]}";
    }
    output << (map.axes.empty() ? "" : "\n  ") << "]\n}\n";
    return output.str();
}

InputMap load_input_map(const fs::path& project_root, std::string& error) {
    const auto path = project_root / input_map_filename;
    std::error_code code;
    if (fs::is_symlink(path, code)) {
        error = "input map must not be a symbolic link; using defaults";
        return default_input_map();
    }
    if (!fs::is_regular_file(path, code)) return default_input_map();
    if (fs::file_size(path, code) > 256U * 1024U) {
        error = "input map exceeds 256 KiB; using defaults";
        return default_input_map();
    }
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    auto map = parse_input_map(text.str(), error);
    if (!map) {
        error = "invalid input map: " + error + "; using defaults";
        return default_input_map();
    }
    return *map;
}

bool save_input_map(const fs::path& project_root, const InputMap& map, std::string& error) {
    const auto path = project_root / input_map_filename;
    const auto temporary = project_root / ".relay-input.tmp";
    std::error_code code;
    if (fs::is_symlink(path, code)) {
        error = "input map must not be a symbolic link";
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const auto text = input_map_json(map);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.flush()) {
            error = "could not write the input map";
            fs::remove(temporary, code);
            return false;
        }
    }
    fs::rename(temporary, path, code);
    if (code) {
        error = "could not replace the input map: " + code.message();
        fs::remove(temporary, code);
        return false;
    }
    return true;
}

void InputState::apply(const std::string_view event) {
    const auto parts = split(event);
    const auto press = [&](const std::string& id, const bool down) {
        auto& control = controls_[id];
        if (down && !control.down) ++control.presses;
        if (!down && control.down) ++control.releases;
        control.down = down;
    };
    const auto kind = parts[0];
    if ((kind == "key" || kind == "mouse_button") && parts.size() == 3U &&
        (parts[1] == "down" || parts[1] == "up")) {
        const auto id = control_id(kind == "key" ? "key" : "mouse", parts[2]);
        if (valid_input_control(id)) press(id, parts[1] == "down");
    } else if (kind == "gamepad_button" && parts.size() == 4U &&
               (parts[1] == "down" || parts[1] == "up")) {
        const auto id = control_id("gamepad", parts[3]);
        if (valid_input_control(id)) press(id, parts[1] == "down");
    } else if (kind == "gamepad_axis" && parts.size() == 4U) {
        const auto id = control_id("gamepad", parts[2]);
        const auto value = number(parts[3]);
        if (value && valid_input_control(id))
            controls_[id].value = std::clamp(*value / 32767.0, -1.0, 1.0);
    } else if (kind == "mouse_motion" && parts.size() == 5U) {
        const auto x = number(parts[1]), y = number(parts[2]), dx = number(parts[3]),
                   dy = number(parts[4]);
        if (x && y && dx && dy) {
            mouse_x_ = *x;
            mouse_y_ = *y;
            pending_dx_ += *dx;
            pending_dy_ += *dy;
        }
    } else if (kind == "mouse_wheel" && parts.size() == 3U) {
        if (const auto y = number(parts[2])) pending_wheel_ += *y;
    } else if (event == "input:reset") {
        for (auto& [id, control] : controls_) {
            if (control.down) ++control.releases;
            control.down = false;
            control.value = 0.0;
        }
        pending_dx_ = pending_dy_ = pending_wheel_ = 0.0;
    }
    // Older traces recorded layout-dependent keycodes such as key:down:32; no binding names those.
}

void InputState::begin_step() {
    for (auto& [id, control] : controls_) {
        if (analog_control(id)) {
            // Analog controls act as buttons past half travel, for actions bound to triggers.
            const bool active = control.value > 0.5;
            control.held = control.still_down = active;
            control.pressed = active && !control.was_active;
            control.released = !active && control.was_active;
            control.was_active = active;
        } else {
            control.pressed = control.presses > 0U;
            control.released = control.releases > 0U;
            control.held = control.down || control.pressed;
            control.still_down = control.down;
        }
        control.presses = control.releases = 0U;
    }
    step_dx_ = pending_dx_;
    step_dy_ = pending_dy_;
    step_wheel_ = pending_wheel_;
    pending_dx_ = pending_dy_ = pending_wheel_ = 0.0;
    for (auto it = simulations_.begin(); it != simulations_.end();) {
        auto& simulation = it->second;
        simulation.held = simulation.steps > 0U;
        simulation.pressed = simulation.held && !simulation.was_held;
        simulation.released = !simulation.held && simulation.was_held;
        simulation.was_held = simulation.held;
        if (simulation.steps > 0U) --simulation.steps;
        if (!simulation.held && !simulation.released) it = simulations_.erase(it);
        else ++it;
    }
}

void InputState::clear_edges() {
    for (auto& [id, control] : controls_) {
        control.presses = control.releases = 0U;
        control.pressed = control.released = false;
        control.held = control.still_down = control.down;
    }
    pending_dx_ = pending_dy_ = pending_wheel_ = step_dx_ = step_dy_ = step_wheel_ = 0.0;
    simulations_.clear();
}

const InputState::Control* InputState::find(const std::string_view id) const {
    const auto found = controls_.find(id);
    return found == controls_.end() ? nullptr : &found->second;
}

bool InputState::control(const std::string_view id, const Query query) const {
    const auto* found = find(id);
    if (!found) return false;
    return query == Query::held ? found->held : query == Query::pressed ? found->pressed
                                                                        : found->released;
}

double InputState::control_value(const std::string_view id) const {
    const auto* found = find(id);
    if (!found) return 0.0;
    return analog_control(id) ? found->value : (found->held ? 1.0 : 0.0);
}

bool InputState::action(const std::string_view name, const Query query) const {
    if (const auto found = simulations_.find(name); found != simulations_.end()) {
        const auto& simulation = found->second;
        return query == Query::held ? simulation.held : query == Query::pressed ? simulation.pressed
                                                                                : simulation.released;
    }
    const auto action = std::find_if(map_.actions.begin(), map_.actions.end(),
                                     [&](const InputAction& entry) { return entry.name == name; });
    if (action == map_.actions.end()) return false;
    bool held = false, pressed = false, released = false, still_down = false;
    for (const auto& binding : action->bindings) {
        held |= control(binding, Query::held);
        pressed |= control(binding, Query::pressed);
        released |= control(binding, Query::released);
        if (const auto* found = find(binding)) still_down |= found->still_down;
    }
    // With two keys on one action, releasing one while the other stays down is not a release.
    if (query == Query::held) return held;
    if (query == Query::pressed) return pressed;
    return released && !still_down;
}

double InputState::axis(const std::string_view name) const {
    if (const auto found = simulations_.find(name); found != simulations_.end())
        return found->second.held ? std::clamp(found->second.value, -1.0, 1.0) : 0.0;
    const auto axis = std::find_if(map_.axes.begin(), map_.axes.end(),
                                   [&](const InputAxis& entry) { return entry.name == name; });
    if (axis == map_.axes.end()) return 0.0;
    double strongest = 0.0;
    for (const auto& binding : axis->bindings) {
        double value = 0.0;
        if (!binding.analog.empty()) {
            const auto raw = control_value(binding.analog) * binding.scale;
            const auto magnitude = std::abs(raw);
            value = magnitude <= axis->deadzone
                        ? 0.0
                        : std::copysign((magnitude - axis->deadzone) / (1.0 - axis->deadzone), raw);
        } else {
            value = (control(binding.positive, Query::held) ? 1.0 : 0.0) -
                    (control(binding.negative, Query::held) ? 1.0 : 0.0);
        }
        if (std::abs(value) > std::abs(strongest)) strongest = value;
    }
    return std::clamp(strongest, -1.0, 1.0);
}

bool InputState::simulate(const std::string_view name, const double value, const std::uint32_t steps) {
    const bool known =
        std::any_of(map_.actions.begin(), map_.actions.end(),
                    [&](const InputAction& action) { return action.name == name; }) ||
        std::any_of(map_.axes.begin(), map_.axes.end(),
                    [&](const InputAxis& axis) { return axis.name == name; });
    if (!known || !std::isfinite(value)) return false;
    auto& simulation = simulations_[std::string{name}];
    simulation.value = value;
    simulation.steps = steps;
    return true;
}

std::string InputState::state_json() const {
    std::ostringstream output;
    output << std::setprecision(6) << "{\"actions\":[";
    for (std::size_t index = 0; index < map_.actions.size(); ++index) {
        const auto& name = map_.actions[index].name;
        output << (index ? "," : "") << "{\"name\":\"" << json_escape(name) << "\",\"held\":"
               << (action(name, Query::held) ? "true" : "false") << ",\"pressed\":"
               << (action(name, Query::pressed) ? "true" : "false") << '}';
    }
    output << "],\"axes\":[";
    for (std::size_t index = 0; index < map_.axes.size(); ++index) {
        const auto& name = map_.axes[index].name;
        output << (index ? "," : "") << "{\"name\":\"" << json_escape(name) << "\",\"value\":"
               << axis(name) << '}';
    }
    output << "],\"held_controls\":[";
    bool first = true;
    for (const auto& [id, control] : controls_)
        if (control.held || (analog_control(id) && std::abs(control.value) > 0.05)) {
            output << (first ? "" : ",") << '"' << json_escape(id) << '"';
            first = false;
        }
    output << "],\"mouse\":{\"x\":" << mouse_x_ << ",\"y\":" << mouse_y_ << ",\"dx\":" << step_dx_
           << ",\"dy\":" << step_dy_ << ",\"wheel\":" << step_wheel_ << "}}";
    return output.str();
}

} // namespace relay
