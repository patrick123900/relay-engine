#include "relay/platform/sdl_input.hpp"

#include <cctype>
#include <string_view>

namespace relay {
namespace {

std::string key_name(const SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_RETURN: return "enter";
    case SDL_SCANCODE_MINUS: return "minus";
    case SDL_SCANCODE_EQUALS: return "equals";
    case SDL_SCANCODE_LEFTBRACKET: return "left_bracket";
    case SDL_SCANCODE_RIGHTBRACKET: return "right_bracket";
    case SDL_SCANCODE_BACKSLASH: return "backslash";
    case SDL_SCANCODE_SEMICOLON: return "semicolon";
    case SDL_SCANCODE_APOSTROPHE: return "apostrophe";
    case SDL_SCANCODE_GRAVE: return "grave";
    case SDL_SCANCODE_COMMA: return "comma";
    case SDL_SCANCODE_PERIOD: return "period";
    case SDL_SCANCODE_SLASH: return "slash";
    case SDL_SCANCODE_LSHIFT: return "left_shift";
    case SDL_SCANCODE_RSHIFT: return "right_shift";
    case SDL_SCANCODE_LCTRL: return "left_ctrl";
    case SDL_SCANCODE_RCTRL: return "right_ctrl";
    case SDL_SCANCODE_LALT: return "left_alt";
    case SDL_SCANCODE_RALT: return "right_alt";
    default: break;
    }
    // SDL's names are stable English labels: "W", "Space", "Keypad 4", "F1".
    std::string name;
    for (const char character : std::string_view{SDL_GetScancodeName(scancode)}) {
        if (std::isalnum(static_cast<unsigned char>(character)))
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        else if (character == ' ' && !name.empty() && name.back() != '_')
            name += '_';
    }
    if (!name.empty() && name.back() == '_') name.pop_back();
    return name.empty() ? "scancode_" + std::to_string(static_cast<int>(scancode)) : name;
}

std::string_view mouse_button_name(const Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return "left";
    case SDL_BUTTON_MIDDLE: return "middle";
    case SDL_BUTTON_RIGHT: return "right";
    case SDL_BUTTON_X1: return "x1";
    case SDL_BUTTON_X2: return "x2";
    default: return {};
    }
}

std::string_view gamepad_button_name(const Uint8 button) {
    switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "a";
    case SDL_GAMEPAD_BUTTON_EAST: return "b";
    case SDL_GAMEPAD_BUTTON_WEST: return "x";
    case SDL_GAMEPAD_BUTTON_NORTH: return "y";
    case SDL_GAMEPAD_BUTTON_BACK: return "back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "guide";
    case SDL_GAMEPAD_BUTTON_START: return "start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "left_stick";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "right_stick";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "left_shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "right_shoulder";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "dpad_up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "dpad_down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "dpad_left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "dpad_right";
    default: return {};
    }
}

std::string_view gamepad_axis_name(const Uint8 axis) {
    switch (static_cast<SDL_GamepadAxis>(axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX: return "leftx";
    case SDL_GAMEPAD_AXIS_LEFTY: return "lefty";
    case SDL_GAMEPAD_AXIS_RIGHTX: return "rightx";
    case SDL_GAMEPAD_AXIS_RIGHTY: return "righty";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "left_trigger";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "right_trigger";
    default: return {};
    }
}

} // namespace

std::optional<std::string> sdl_input_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.repeat) return std::nullopt;
        return std::string{"key:"} + (event.type == SDL_EVENT_KEY_DOWN ? "down:" : "up:") +
               key_name(event.key.scancode);
    case SDL_EVENT_MOUSE_MOTION:
        return "mouse_motion:" + std::to_string(event.motion.x) + ':' + std::to_string(event.motion.y) +
               ':' + std::to_string(event.motion.xrel) + ':' + std::to_string(event.motion.yrel);
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto name = mouse_button_name(event.button.button);
        if (name.empty()) return std::nullopt;
        return std::string{"mouse_button:"} +
               (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down:" : "up:") + std::string{name};
    }
    case SDL_EVENT_MOUSE_WHEEL:
        return "mouse_wheel:" + std::to_string(event.wheel.x) + ':' + std::to_string(event.wheel.y);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto name = gamepad_axis_name(event.gaxis.axis);
        if (name.empty()) return std::nullopt;
        return "gamepad_axis:" + std::to_string(event.gaxis.which) + ':' + std::string{name} + ':' +
               std::to_string(event.gaxis.value);
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const auto name = gamepad_button_name(event.gbutton.button);
        if (name.empty()) return std::nullopt;
        return std::string{"gamepad_button:"} +
               (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? "down:" : "up:") +
               std::to_string(event.gbutton.which) + ':' + std::string{name};
    }
    default:
        return std::nullopt;
    }
}

std::optional<std::string> sdl_binding_control(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.repeat) return std::nullopt;
        return "key:" + key_name(event.key.scancode);
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const auto name = mouse_button_name(event.button.button);
        if (name.empty()) return std::nullopt;
        return "mouse:" + std::string{name};
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const auto name = gamepad_button_name(event.gbutton.button);
        if (name.empty()) return std::nullopt;
        return "gamepad:" + std::string{name};
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto name = gamepad_axis_name(event.gaxis.axis);
        if (name.empty() || (event.gaxis.value < 16384 && event.gaxis.value > -16384))
            return std::nullopt;
        return "gamepad:" + std::string{name};
    }
    default:
        return std::nullopt;
    }
}

void sdl_track_gamepads(const SDL_Event& event) {
    if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
        (void)SDL_OpenGamepad(event.gdevice.which);
    } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
        if (auto* gamepad = SDL_GetGamepadFromID(event.gdevice.which)) SDL_CloseGamepad(gamepad);
    }
}

} // namespace relay
