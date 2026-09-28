#pragma once

// Game interface components: screen-space controls laid out like Godot's Control nodes (anchors
// and offsets inside the parent's rectangle) and drawn over the game's view during Run Game only.
//
// A node becomes a control with the ui_control component, which places a rectangle; widgets such
// as a panel, label, image or button draw into it or react to the pointer. A canvas (ui_canvas) on
// an ancestor scales everything below it for the screen size and orders it against other canvases.
//
// Every UI field is described once in ui_components(): scene files, the control protocol, scripts
// and the editor's Inspector all read and write components through that table.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace relay {

struct JsonValue;

struct Vec2 {
    double x{};
    double y{};
    auto operator<=>(const Vec2&) const = default;
};

// A color as people pick it for an interface: sRGB channels with straight alpha, each 0 to 1.
struct UiColor {
    double r{1.0};
    double g{1.0};
    double b{1.0};
    double a{1.0};
    auto operator<=>(const UiColor&) const = default;
};

// Distances inward from each edge.
struct UiMargins {
    double left{};
    double top{};
    double right{};
    double bottom{};
    auto operator<=>(const UiMargins&) const = default;
};

// Scales and orders the controls below it. Without a canvas, controls use screen pixels.
struct UiCanvas {
    enum class ScaleMode : std::uint8_t { constant_pixel_size, scale_with_screen_size };
    ScaleMode scale_mode{ScaleMode::scale_with_screen_size};
    // scale_with_screen_size: layouts are made for this size and scaled to fit the screen,
    // following its width (match 0), height (1) or a blend of the two.
    Vec2 reference_size{1920.0, 1080.0};
    double match{0.5};
    // constant_pixel_size: one canvas unit is this many screen pixels.
    double scale{1.0};
    // Canvases draw in increasing order; the pointer reaches higher ones first.
    std::int32_t sort_order{};
    bool visible{true};
    auto operator<=>(const UiCanvas&) const = default;
};

// A control's rectangle: each edge sits at an anchor (a fraction of the parent's rectangle) plus an
// offset in canvas units, so anchors 0,0 to 1,1 with zero offsets fill the parent and equal
// anchors keep a fixed size. Rotation and scale turn the drawn control about its pivot.
struct UiControl {
    Vec2 anchor_min{0.5, 0.5};
    Vec2 anchor_max{0.5, 0.5};
    Vec2 offset_min{-60.0, -20.0};
    Vec2 offset_max{60.0, 20.0};
    Vec2 pivot{0.5, 0.5};
    double rotation{};
    Vec2 scale{1.0, 1.0};
    bool visible{true};
    double opacity{1.0};
    bool clip_contents{false};
    // What the pointer does over this control: stop keeps it from controls behind and from the
    // game, pass lets clicks through, ignore makes the control invisible to the pointer.
    enum class MouseFilter : std::uint8_t { stop, pass, ignore };
    MouseFilter mouse_filter{MouseFilter::pass};
    // Siblings draw, and sit in containers, in increasing order.
    std::int32_t order{};
    // Inside a container: the smallest size, whether the control takes a share of spare room along
    // each axis, and how it sits in its cell.
    Vec2 min_size{};
    bool expand_x{};
    bool expand_y{};
    enum class Placement : std::uint8_t { fill, start, center, end };
    Placement size_x{Placement::fill};
    Placement size_y{Placement::fill};
    auto operator<=>(const UiControl&) const = default;
};

// A filled, optionally rounded and outlined rectangle behind everything else on the node.
struct UiPanel {
    UiColor color{0.09, 0.1, 0.12, 0.86};
    UiColor border_color{1.0, 1.0, 1.0, 0.12};
    double border_width{};
    double corner_radius{8.0};
    UiColor shadow_color{0.0, 0.0, 0.0, 0.0};
    double shadow_size{12.0};
    Vec2 shadow_offset{0.0, 4.0};
    auto operator<=>(const UiPanel&) const = default;
};

struct UiLabel {
    std::string text{"Label"};
    std::string font; // A project .ttf or .otf file; empty uses the built-in Inter.
    bool bold{};      // The built-in font's bold weight.
    double size{24.0};
    UiColor color{};
    enum class Align : std::uint8_t { start, center, end };
    Align horizontal_align{Align::start}; // left, center, right
    Align vertical_align{Align::center};  // top, center, bottom
    bool wrap{};
    double line_spacing{1.0};
    double outline_size{};
    UiColor outline_color{0.0, 0.0, 0.0, 1.0};
    UiColor shadow_color{0.0, 0.0, 0.0, 0.0};
    Vec2 shadow_offset{2.0, 2.0};
    auto operator<=>(const UiLabel&) const = default;
};

struct UiImage {
    std::string image; // A project PNG or JPEG file.
    UiColor color{};
    // stretch fills the rectangle; fit keeps the aspect inside it; fill keeps the aspect and crops;
    // center draws at the image's own size; tile repeats it; sliced stretches only the middle,
    // keeping `slice` pixels at each edge (nine-slice).
    enum class Mode : std::uint8_t { stretch, fit, fill, center, tile, sliced };
    Mode mode{Mode::stretch};
    UiMargins slice{};
    double slice_scale{1.0};
    bool flip_x{};
    bool flip_y{};
    auto operator<=>(const UiImage&) const = default;
};

// Makes the control clickable. The node's panel color is its normal look; these replace it (or
// tint its image) while the pointer hovers, while held or pressed, and while disabled.
struct UiButton {
    bool disabled{};
    bool toggle{};  // Each click flips `pressed` instead of springing back.
    bool pressed{};
    UiColor hover_color{0.2, 0.22, 0.27, 0.95};
    UiColor pressed_color{0.06, 0.07, 0.09, 0.95};
    UiColor disabled_color{0.09, 0.1, 0.12, 0.5};
    UiMargins padding{18.0, 10.0, 18.0, 10.0}; // Around the label, for containers.
    std::string click_sound;                    // A project sound played flat on each click.
    auto operator<=>(const UiButton&) const = default;
};

// A check box or switch, with the node's label beside it.
struct UiToggle {
    bool checked{};
    bool disabled{};
    enum class Style : std::uint8_t { check, switch_ };
    Style style{Style::check};
    double box_size{24.0};
    double spacing{10.0};
    UiColor box_color{0.2, 0.22, 0.27, 1.0};
    UiColor check_color{0.35, 0.7, 1.0, 1.0};
    std::string click_sound;
    auto operator<=>(const UiToggle&) const = default;
};

struct UiSlider {
    double value{0.5};
    double minimum{};
    double maximum{1.0};
    double step{}; // Zero moves smoothly.
    bool vertical{};
    bool disabled{};
    double track_thickness{6.0};
    double handle_size{20.0};
    UiColor track_color{1.0, 1.0, 1.0, 0.16};
    UiColor fill_color{0.35, 0.7, 1.0, 1.0};
    UiColor handle_color{0.95, 0.96, 0.98, 1.0};
    auto operator<=>(const UiSlider&) const = default;
};

// Fills part of the control from one edge. The node's panel, if any, is the empty track.
struct UiProgressBar {
    double value{0.5};
    double minimum{};
    double maximum{1.0};
    enum class Direction : std::uint8_t { left_to_right, right_to_left, bottom_to_top, top_to_bottom };
    Direction direction{Direction::left_to_right};
    UiColor fill_color{0.35, 0.7, 1.0, 1.0};
    double fill_inset{2.0};
    auto operator<=>(const UiProgressBar&) const = default;
};

// Places the child controls itself, in a column, a row or a grid, replacing their anchors.
struct UiContainer {
    enum class Layout : std::uint8_t { vertical, horizontal, grid };
    Layout layout{Layout::vertical};
    double spacing{8.0};
    UiMargins padding{};
    enum class Align : std::uint8_t { start, center, end };
    Align align{Align::start}; // Where children sit along the layout when there is room left.
    std::int32_t columns{2};   // Grid only.
    auto operator<=>(const UiContainer&) const = default;
};

// A node's UI components. Widgets (everything but the canvas) need the control; a canvas node is
// not itself a control.
struct UiComponents {
    std::optional<UiCanvas> canvas;
    std::optional<UiControl> control;
    std::optional<UiPanel> panel;
    std::optional<UiLabel> label;
    std::optional<UiImage> image;
    std::optional<UiButton> button;
    std::optional<UiToggle> toggle;
    std::optional<UiSlider> slider;
    std::optional<UiProgressBar> progress_bar;
    std::optional<UiContainer> container;
    [[nodiscard]] bool empty() const { return !canvas && !control; }
    auto operator<=>(const UiComponents&) const = default;
};

inline constexpr std::size_t maximum_ui_text_bytes = 4096U;
inline constexpr double maximum_ui_coordinate = 1e6;

enum class UiFieldType : std::uint8_t { boolean, number, integer, vec2, color, margins, text, asset, choice };

// One value of any field type. Choices are their names; text and asset paths are strings.
using UiValue = std::variant<bool, double, Vec2, UiColor, UiMargins, std::string>;

struct UiFieldInfo {
    std::string_view name;          // Wire name, also the Inspector label with spaces.
    UiFieldType type{};
    double minimum{};               // Numbers and every component of vectors and margins.
    double maximum{};               // Also the byte limit of text.
    std::vector<std::string_view> choices; // Choice names, in the enum's order.
    std::vector<std::string_view> kinds;   // Assets: "image", "font" or "audio".
    std::string_view description;
};

struct UiComponentInfo {
    std::string_view id;          // "ui_label", as component.add and scene.set_ui name it.
    std::string_view key;         // "label", its key in an entity's "ui" object.
    std::string_view name;        // "Label"
    std::string_view description;
    std::uint32_t stable_id{};
    std::vector<UiFieldInfo> fields;
    std::function<bool(const UiComponents&)> present;
    // Adds the component with its defaults, or removes it.
    std::function<void(UiComponents&, bool)> attach;
    // Unchecked access by field index; the component must be present and values valid.
    std::function<UiValue(const UiComponents&, std::size_t)> get;
    std::function<void(UiComponents&, std::size_t, const UiValue&)> set;
};

// In the order the Inspector shows them: canvas, control, then widgets.
[[nodiscard]] const std::vector<UiComponentInfo>& ui_components();
// By component id ("ui_label") or entity key ("label").
[[nodiscard]] const UiComponentInfo* find_ui_component(std::string_view name);
[[nodiscard]] std::optional<std::size_t> ui_field_index(const UiComponentInfo& component,
                                                        std::string_view name);

// Whether a value suits a field: its type, range, choice, length or asset extension.
[[nodiscard]] bool check_ui_value(const UiFieldInfo& field, const UiValue& value, std::string& error);
// A field's value from JSON: booleans, numbers, strings (text, asset paths, choice names) and
// arrays for vectors [x, y], colors [r, g, b, a] and margins [left, top, right, bottom].
[[nodiscard]] std::optional<UiValue> ui_value_from_json(const UiFieldInfo& field, const JsonValue& value,
                                                        std::string& error);
[[nodiscard]] std::string ui_value_json(const UiValue& value);

// "null" without UI components, otherwise an object keyed by component key with every field.
[[nodiscard]] std::string ui_components_json(const UiComponents& ui);
// Reads what ui_components_json writes. Fields left out keep their defaults; unknown components
// and fields are errors.
[[nodiscard]] bool read_ui_components(const JsonValue& value, UiComponents& ui, std::string& error);
// Applies a partial update to one component, adding it first when it is missing: `values` holds
// field names and JSON values. Slider and progress values are clamped into their new range.
[[nodiscard]] bool apply_ui_values(UiComponents& ui, const UiComponentInfo& component,
                                   const JsonValue& values, std::string& error);
// Clamps slider and progress values into their ranges and rounds slider values to their step.
void normalize_ui(UiComponents& ui);
[[nodiscard]] bool valid_ui(const UiComponents& ui, std::string* error = nullptr);
// A project-relative path with a suitable extension for one of `kinds`.
[[nodiscard]] bool valid_ui_asset_path(std::string_view path, const std::vector<std::string_view>& kinds);

// Anchor presets as Godot names them: top_left, top, top_right, left, center, right, bottom_left,
// bottom, bottom_right, full_rect, top_wide, bottom_wide, left_wide, right_wide, vcenter_wide,
// hcenter_wide. Setting one keeps `size` (the control's laid-out size; by default its offsets'
// extent, where both anchors on an axis agree) and fills along wide axes, `margin` in from the
// anchored edges.
[[nodiscard]] const std::vector<std::string_view>& ui_anchor_presets();
[[nodiscard]] bool apply_ui_anchor_preset(UiControl& control, std::string_view preset,
                                          std::optional<Vec2> size = std::nullopt, double margin = 0.0);

// Stable ids for scene file component descriptors.
inline constexpr std::uint32_t ui_first_stable_id = 0x15U;

} // namespace relay
