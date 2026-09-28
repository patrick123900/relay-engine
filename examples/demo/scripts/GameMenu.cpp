#include "relay_script.hpp"

#include <cmath>
#include <string>

// The showcase's interface: a crosshair, control hints and a count of balls fired while playing,
// and a menu on Tab that frees the cursor for its button, switch and slider.
//
// Put it on the HUD canvas. It finds its controls by name: "Menu" (hidden until the menu opens),
// "Crosshair", "Hints" and "Shots", and hears the menu's "Resume" button, "Show hints" switch and
// "Music volume" slider through on_ui, which reaches this script from every control below it.
// The First Person Controller stands still while the menu has the cursor.
class GameMenu : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("menu_key", menu_key);
        p.add("music_bus", music_bus);
    }

    void on_start() override {
        menu = relay::world::find("Menu");
        crosshair = relay::world::find("Crosshair");
        hints = relay::world::find("Hints");
        shots_label = relay::world::find("Shots");
        menu.set_visible(false);
    }

    void on_update(double) override {
        if (relay::input::key_pressed(menu_key)) show_menu(!open);
        // Clicks on the menu never reach "fire": the interface takes them.
        if (!open && relay::input::pressed("fire")) shots_label.set_text("Balls " + std::to_string(++shots));
    }

    void on_ui(const relay::ui::Event& event) override {
        const auto name = event.control.name();
        if (event.clicked() && name == "Resume") show_menu(false);
        if (event.toggled() && name == "Show hints") hints.set_visible(event.value != 0.0);
        // The slider runs from silent to the mixer's own level, following loudness rather than amplitude.
        if (event.value_changed() && name == "Music volume")
            relay::audio::set_bus_volume(music_bus, event.value > 0.001 ? 40.0 * std::log10(event.value) : -80.0);
    }

private:
    void show_menu(bool show) {
        open = show;
        menu.set_visible(show);
        crosshair.set_visible(!show);
        relay::input::set_mouse_locked(!show);
    }

    std::string menu_key = "tab"; // A key name, as relay::input::key_pressed reads them.
    std::string music_bus = "Music";
    relay::Entity menu, crosshair, hints, shots_label;
    bool open = false;
    int shots = 0;
};
RELAY_BEHAVIOUR(GameMenu)
