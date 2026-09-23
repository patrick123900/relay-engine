#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace relay {

// Controls are named "<device>:<name>": key:w, key:left_shift, mouse:left, gamepad:a,
// gamepad:leftx. Keys name physical positions (US layout names), so bindings survive keyboard
// layouts. Gamepad sticks and triggers are analog controls with values in [-1, 1].
[[nodiscard]] bool valid_input_control(std::string_view control);
[[nodiscard]] bool valid_input_name(std::string_view name);

struct InputAction {
    std::string name;
    std::vector<std::string> bindings; // Any bound control held holds the action.
};

// One contribution to an axis: two buttons pulling towards -1 and +1, or an analog control.
struct InputAxisBinding {
    std::string negative, positive; // Button pair; both empty for analog.
    std::string analog;             // Gamepad axis; empty for a button pair.
    double scale{1.0};              // Analog only; -1 inverts.
};

struct InputAxis {
    std::string name;
    double deadzone{0.2}; // Analog magnitudes below it read as zero.
    std::vector<InputAxisBinding> bindings; // The strongest binding wins, clamped to [-1, 1].
};

struct InputMap {
    std::vector<InputAction> actions;
    std::vector<InputAxis> axes;
    bool lock_mouse{}; // The editor hides and locks the cursor while the game has input focus.
};

inline constexpr std::string_view input_map_filename = "input.relay-input.json";
inline constexpr std::size_t maximum_input_entries = 128U;
inline constexpr std::size_t maximum_input_bindings = 16U;

[[nodiscard]] InputMap default_input_map();
[[nodiscard]] std::optional<InputMap> parse_input_map(std::string_view json, std::string& error);
[[nodiscard]] std::string input_map_json(const InputMap& map);
// A project without input.relay-input.json uses the defaults.
[[nodiscard]] InputMap load_input_map(const std::filesystem::path& project_root, std::string& error);
[[nodiscard]] bool save_input_map(const std::filesystem::path& project_root, const InputMap& map,
                                  std::string& error);

// Live devices plus the per-step view scripts read. Platform events update the live state at any
// time; begin_step() latches it once per fixed game step, so a tap shorter than a step still
// reads as pressed exactly once, and replaying recorded events reproduces the same reads.
class InputState {
public:
    // Events: key:down:<name>, key:up:<name>, mouse_button:down|up:<name>,
    // mouse_motion:<x>:<y>:<dx>:<dy>, mouse_wheel:<x>:<y>, gamepad_axis:<pad>:<name>:<-32768..32767>,
    // gamepad_button:down|up:<pad>:<name>, and input:reset, which releases everything.
    void apply(std::string_view event);
    void begin_step();
    // Clears per-step edges and pending motion, for example when a game session starts.
    void clear_edges();
    void set_map(InputMap map) { map_ = std::move(map); }
    [[nodiscard]] const InputMap& map() const { return map_; }

    enum class Query { held, pressed, released };
    [[nodiscard]] bool control(std::string_view id, Query query) const;
    [[nodiscard]] double control_value(std::string_view id) const;
    [[nodiscard]] bool action(std::string_view name, Query query) const;
    [[nodiscard]] double axis(std::string_view name) const;
    [[nodiscard]] double mouse_x() const { return mouse_x_; }
    [[nodiscard]] double mouse_y() const { return mouse_y_; }
    [[nodiscard]] double mouse_dx() const { return step_dx_; }
    [[nodiscard]] double mouse_dy() const { return step_dy_; }
    [[nodiscard]] double mouse_wheel() const { return step_wheel_; }

    // Holds an action or sets an axis for a number of steps, for automated play-testing.
    [[nodiscard]] bool simulate(std::string_view name, double value, std::uint32_t steps);
    [[nodiscard]] std::string state_json() const;

private:
    struct Control {
        bool down{};
        std::uint32_t presses{}, releases{};
        double value{};
        bool held{}, pressed{}, released{}, was_active{};
        bool still_down{}; // Down when the step began, unlike a tap that ended within it.
    };
    struct Simulation {
        double value{};
        std::uint32_t steps{};
        bool was_held{}, held{}, pressed{}, released{};
    };
    [[nodiscard]] const Control* find(std::string_view id) const;

    InputMap map_{default_input_map()};
    std::map<std::string, Control, std::less<>> controls_;
    std::map<std::string, Simulation, std::less<>> simulations_;
    double mouse_x_{}, mouse_y_{}, pending_dx_{}, pending_dy_{}, pending_wheel_{};
    double step_dx_{}, step_dy_{}, step_wheel_{};
};

} // namespace relay
