#pragma once

// The game interface during Run Game: lays out the scene's controls for the game view, moves the
// pointer over them each game step, turns presses into clicks, toggles and slider drags, and
// reports those as events for scripts. It also builds the draw list the window draws over the
// game's view. In Editor mode nothing here runs, so the interface never shows in the editor's view.

#include "relay/core/input.hpp"
#include "relay/ui/ui_render.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace relay {

struct UiEvent {
    // clicked: a button, check box or slider was pressed and released over it. toggled: a check
    // box or toggle button flipped (value 1 on, 0 off). value_changed: a slider moved (value).
    // pressed and released: the pointer went down on, or came up from, an interactive control.
    enum class Type : std::uint8_t { clicked, toggled, value_changed, pressed, released };
    Type type{};
    Entity control;
    double value{};
};
[[nodiscard]] std::string_view ui_event_name(UiEvent::Type type);

class UiSystem {
public:
    // Plays a click sound: a project sound file, flat, on the SFX bus.
    using SoundPlayer = std::function<void(const std::string& clip)>;

    void set_root(const std::filesystem::path& root) { painter_.set_root(root); }
    void set_sound_player(SoundPlayer player) { sound_ = std::move(player); }
    // The game view in pixels. Hosts set it as the view changes; headless engines keep the
    // engine's configured size.
    void set_view_size(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] std::uint32_t view_width() const { return width_; }
    [[nodiscard]] std::uint32_t view_height() const { return height_; }

    // One game step, after input is latched and before scripts update: hover, presses, clicks and
    // drags change the scene's controls (a check box's checked, a slider's value) and are
    // reported by events() until the next step. Presses the interface takes are consumed, so the
    // game does not also see them.
    void update(Scene& scene, InputState& input);
    [[nodiscard]] const std::vector<UiEvent>& events() const { return events_; }
    // Forgets pointer state and events, at Run Game and Stop Game.
    void reset();

    // The interface as laid out for the view, and its triangles with the pointer's hover and
    // press looks. The draw list lives until the next call.
    [[nodiscard]] UiLayout layout(const Scene& scene, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] const UiDrawList& draw(const Scene& scene, std::uint32_t width, std::uint32_t height);
    [[nodiscard]] UiVisualState visual_state() const { return {hovered_, pressed_}; }

    // Presses and releases the pointer at a control's center over the next two steps, as a
    // player would, even while the cursor is locked. False when it is not a visible,
    // interactive control.
    [[nodiscard]] bool simulate_click(const Scene& scene, Entity control, std::string& error);
    [[nodiscard]] UiPainter& painter() { return painter_; }

private:
    void emit(UiEvent::Type type, Entity control, double value) { events_.push_back({type, control, value}); }

    UiPainter painter_;
    UiDrawList draw_list_;
    SoundPlayer sound_;
    std::uint32_t width_{1280U};
    std::uint32_t height_{720U};
    std::vector<UiEvent> events_;
    Entity hovered_;
    Entity pressed_;
    struct Simulated {
        Entity control;
        bool released{};
    };
    std::optional<Simulated> simulated_;
    // The pointer position the latest step used.
    Vec2 pointer_{};
};

} // namespace relay
