#include "relay/scene/ui.hpp"

#include "relay/core/json.hpp"
#include "relay/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <memory>

namespace relay {
namespace {

// Builds one component's field list with typed accessors, so the generic table never needs a
// switch over components.
template <class C>
struct Table {
    std::vector<UiFieldInfo> fields;
    std::vector<std::function<UiValue(const C&)>> get;
    std::vector<std::function<void(C&, const UiValue&)>> set;

    Table& boolean(std::string_view name, bool C::*member, std::string_view description) {
        fields.push_back({name, UiFieldType::boolean, 0.0, 1.0, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<bool>(value); });
        return *this;
    }
    Table& number(std::string_view name, double C::*member, double minimum, double maximum,
                  std::string_view description) {
        fields.push_back({name, UiFieldType::number, minimum, maximum, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<double>(value); });
        return *this;
    }
    Table& integer(std::string_view name, std::int32_t C::*member, double minimum, double maximum,
                   std::string_view description) {
        fields.push_back({name, UiFieldType::integer, minimum, maximum, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{static_cast<double>(c.*member)}; });
        set.emplace_back([member](C& c, const UiValue& value) {
            c.*member = static_cast<std::int32_t>(std::llround(std::get<double>(value)));
        });
        return *this;
    }
    Table& vec2(std::string_view name, Vec2 C::*member, double minimum, double maximum,
                std::string_view description) {
        fields.push_back({name, UiFieldType::vec2, minimum, maximum, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<Vec2>(value); });
        return *this;
    }
    Table& color(std::string_view name, UiColor C::*member, std::string_view description) {
        fields.push_back({name, UiFieldType::color, 0.0, 1.0, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<UiColor>(value); });
        return *this;
    }
    Table& margins(std::string_view name, UiMargins C::*member, double minimum, double maximum,
                   std::string_view description) {
        fields.push_back({name, UiFieldType::margins, minimum, maximum, {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<UiMargins>(value); });
        return *this;
    }
    Table& text(std::string_view name, std::string C::*member, std::size_t maximum_bytes,
                std::string_view description) {
        fields.push_back({name, UiFieldType::text, 0.0, static_cast<double>(maximum_bytes), {}, {}, description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<std::string>(value); });
        return *this;
    }
    Table& asset(std::string_view name, std::string C::*member, std::vector<std::string_view> kinds,
                 std::string_view description) {
        fields.push_back({name, UiFieldType::asset, 0.0, 128.0, {}, std::move(kinds), description});
        get.emplace_back([member](const C& c) { return UiValue{c.*member}; });
        set.emplace_back([member](C& c, const UiValue& value) { c.*member = std::get<std::string>(value); });
        return *this;
    }
    template <class E>
    Table& choice(std::string_view name, E C::*member, std::vector<std::string_view> names,
                  std::string_view description) {
        fields.push_back({name, UiFieldType::choice, 0.0, 0.0, names, {}, description});
        get.emplace_back([member, names](const C& c) {
            const auto index = static_cast<std::size_t>(c.*member);
            return UiValue{std::string(index < names.size() ? names[index] : names.front())};
        });
        set.emplace_back([member, names](C& c, const UiValue& value) {
            const auto& wanted = std::get<std::string>(value);
            const auto found = std::find(names.begin(), names.end(), wanted);
            if (found != names.end()) c.*member = static_cast<E>(found - names.begin());
        });
        return *this;
    }
};

template <class C>
UiComponentInfo make_component(std::string_view id, std::string_view key, std::string_view name,
                               std::string_view description, std::uint32_t stable_id,
                               std::optional<C> UiComponents::*slot, Table<C> table) {
    UiComponentInfo info;
    info.id = id;
    info.key = key;
    info.name = name;
    info.description = description;
    info.stable_id = stable_id;
    info.fields = table.fields;
    info.present = [slot](const UiComponents& ui) { return (ui.*slot).has_value(); };
    info.attach = [slot](UiComponents& ui, const bool on) {
        if (!on) (ui.*slot).reset();
        else if (!(ui.*slot)) ui.*slot = C{};
    };
    auto getters = std::make_shared<std::vector<std::function<UiValue(const C&)>>>(std::move(table.get));
    auto setters = std::make_shared<std::vector<std::function<void(C&, const UiValue&)>>>(std::move(table.set));
    info.get = [slot, getters](const UiComponents& ui, const std::size_t index) {
        return (*getters)[index](*(ui.*slot));
    };
    info.set = [slot, setters](UiComponents& ui, const std::size_t index, const UiValue& value) {
        (*setters)[index](*(ui.*slot), value);
    };
    return info;
}

std::string number_json(const double value) {
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string("0");
}

bool finite_within(const double value, const double minimum, const double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}

bool extension_suits(std::string_view path, std::string_view kind) {
    const auto extension = lowercase(std::filesystem::path(std::string(path)).extension().string());
    if (kind == "image") return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
    if (kind == "font") return extension == ".ttf" || extension == ".otf";
    if (kind == "audio") return valid_audio_clip_path(path);
    return false;
}

const std::vector<std::string_view> placements{"fill", "start", "center", "end"};

} // namespace

const std::vector<UiComponentInfo>& ui_components() {
    static const std::vector<UiComponentInfo> components = [] {
        std::vector<UiComponentInfo> list;
        list.push_back(make_component<UiCanvas>(
            "ui_canvas", "canvas", "Canvas",
            "A layer for game interface controls, shown during Run Game over the game's view. It "
            "scales the controls below it to the screen and orders them against other canvases.",
            ui_first_stable_id, &UiComponents::canvas,
            Table<UiCanvas>{}
                .choice("scale_mode", &UiCanvas::scale_mode, {"constant_pixel_size", "scale_with_screen_size"},
                        "constant_pixel_size keeps canvas units at `scale` screen pixels; "
                        "scale_with_screen_size scales a layout made for reference_size to the screen")
                .vec2("reference_size", &UiCanvas::reference_size, 16.0, 16384.0,
                      "The screen size layouts are made for, in canvas units")
                .number("match", &UiCanvas::match, 0.0, 1.0,
                        "Scale by the screen's width (0), height (1) or a blend of the two")
                .number("scale", &UiCanvas::scale, 0.05, 20.0, "Screen pixels per canvas unit, for constant_pixel_size")
                .integer("sort_order", &UiCanvas::sort_order, -1000.0, 1000.0,
                         "Higher canvases draw on top and receive the pointer first")
                .boolean("visible", &UiCanvas::visible, "Whether the canvas and everything on it shows")));
        list.push_back(make_component<UiControl>(
            "ui_control", "control", "Control",
            "Places a rectangle on the screen for game interface widgets: anchors on the parent "
            "control (or the screen) plus offsets, with a pivot for rotation and scale.",
            ui_first_stable_id + 1U, &UiComponents::control,
            Table<UiControl>{}
                .vec2("anchor_min", &UiControl::anchor_min, 0.0, 1.0,
                      "Where the left and top edges attach, as fractions of the parent's width and height")
                .vec2("anchor_max", &UiControl::anchor_max, 0.0, 1.0,
                      "Where the right and bottom edges attach, as fractions of the parent's width and height")
                .vec2("offset_min", &UiControl::offset_min, -maximum_ui_coordinate, maximum_ui_coordinate,
                      "Left and top edges, in canvas units from their anchors")
                .vec2("offset_max", &UiControl::offset_max, -maximum_ui_coordinate, maximum_ui_coordinate,
                      "Right and bottom edges, in canvas units from their anchors")
                .vec2("pivot", &UiControl::pivot, -10.0, 10.0,
                      "The point rotation and scale turn about, as fractions of the control's size")
                .number("rotation", &UiControl::rotation, -3600.0, 3600.0, "Degrees, clockwise on screen")
                .vec2("scale", &UiControl::scale, -100.0, 100.0, "Drawn size multiplier, about the pivot")
                .boolean("visible", &UiControl::visible, "Hidden controls, and everything below them, are not drawn")
                .number("opacity", &UiControl::opacity, 0.0, 1.0, "Multiplies the opacity of the control and its children")
                .boolean("clip_contents", &UiControl::clip_contents, "Cut children off at the control's edges")
                .choice("mouse_filter", &UiControl::mouse_filter, {"stop", "pass", "ignore"},
                        "stop blocks the pointer from controls behind and from the game; pass lets clicks "
                        "through; ignore makes the control invisible to the pointer")
                .integer("order", &UiControl::order, -1e6, 1e6,
                         "Position among siblings: lower draws first (behind) and comes first in containers")
                .vec2("min_size", &UiControl::min_size, 0.0, maximum_ui_coordinate,
                      "Smallest size a container gives the control")
                .boolean("expand_x", &UiControl::expand_x, "In a row or grid, take a share of spare width")
                .boolean("expand_y", &UiControl::expand_y, "In a column or grid, take a share of spare height")
                .choice("size_x", &UiControl::size_x, placements,
                        "In a container: fill its cell's width, or sit at its start, center or end")
                .choice("size_y", &UiControl::size_y, placements,
                        "In a container: fill its cell's height, or sit at its start, center or end")));
        list.push_back(make_component<UiPanel>(
            "ui_panel", "panel", "Panel",
            "Fills the control with a color, with rounded corners, a border and a soft shadow.",
            ui_first_stable_id + 2U, &UiComponents::panel,
            Table<UiPanel>{}
                .color("color", &UiPanel::color, "Fill color")
                .color("border_color", &UiPanel::border_color, "Border color")
                .number("border_width", &UiPanel::border_width, 0.0, 256.0, "Border thickness, inside the edge")
                .number("corner_radius", &UiPanel::corner_radius, 0.0, 4096.0, "Rounding of the corners")
                .color("shadow_color", &UiPanel::shadow_color, "Shadow color; fully transparent draws none")
                .number("shadow_size", &UiPanel::shadow_size, 0.0, 256.0, "How far the shadow blurs out")
                .vec2("shadow_offset", &UiPanel::shadow_offset, -1024.0, 1024.0, "Shadow shift")));
        list.push_back(make_component<UiLabel>(
            "ui_label", "label", "Label", "Draws text in the control, aligned and optionally wrapped.",
            ui_first_stable_id + 3U, &UiComponents::label,
            Table<UiLabel>{}
                .text("text", &UiLabel::text, maximum_ui_text_bytes, "The text; new lines break lines")
                .asset("font", &UiLabel::font, {"font"}, "A .ttf or .otf file; empty uses the built-in Inter")
                .boolean("bold", &UiLabel::bold, "Use the built-in font's bold weight")
                .number("size", &UiLabel::size, 1.0, 1024.0, "Font size in canvas units")
                .color("color", &UiLabel::color, "Text color")
                .choice("horizontal_align", &UiLabel::horizontal_align, {"left", "center", "right"},
                        "Where lines sit across the control")
                .choice("vertical_align", &UiLabel::vertical_align, {"top", "center", "bottom"},
                        "Where the text sits down the control")
                .boolean("wrap", &UiLabel::wrap, "Break lines at the control's width")
                .number("line_spacing", &UiLabel::line_spacing, 0.25, 4.0, "Line height, times the font's own")
                .number("outline_size", &UiLabel::outline_size, 0.0, 8.0, "Outline thickness; 0 draws none")
                .color("outline_color", &UiLabel::outline_color, "Outline color")
                .color("shadow_color", &UiLabel::shadow_color, "Drop shadow color; fully transparent draws none")
                .vec2("shadow_offset", &UiLabel::shadow_offset, -64.0, 64.0, "Drop shadow shift")));
        list.push_back(make_component<UiImage>(
            "ui_image", "image", "Image", "Draws a PNG or JPEG image in the control.",
            ui_first_stable_id + 4U, &UiComponents::image,
            Table<UiImage>{}
                .asset("image", &UiImage::image, {"image"}, "A project PNG or JPEG file")
                .color("color", &UiImage::color, "Tint multiplied into the image")
                .choice("mode", &UiImage::mode, {"stretch", "fit", "fill", "center", "tile", "sliced"},
                        "stretch fills the control; fit and fill keep the aspect (fill crops); center "
                        "uses the image's size; tile repeats it; sliced keeps its edges (nine-slice)")
                .margins("slice", &UiImage::slice, 0.0, 16384.0,
                         "Sliced: image pixels kept unstretched at each edge")
                .number("slice_scale", &UiImage::slice_scale, 0.01, 100.0, "Sliced: scale of the kept edges")
                .boolean("flip_x", &UiImage::flip_x, "Mirror left to right")
                .boolean("flip_y", &UiImage::flip_y, "Mirror top to bottom")));
        list.push_back(make_component<UiButton>(
            "ui_button", "button", "Button",
            "Makes the control clickable. Scripts hear each click through on_ui; the panel's color "
            "is the normal look, replaced while hovered, pressed or disabled.",
            ui_first_stable_id + 5U, &UiComponents::button,
            Table<UiButton>{}
                .boolean("disabled", &UiButton::disabled, "Ignore the pointer and look disabled")
                .boolean("toggle", &UiButton::toggle, "Each click flips pressed, like a switch")
                .boolean("pressed", &UiButton::pressed, "Toggle buttons: whether it is on")
                .color("hover_color", &UiButton::hover_color, "Look while the pointer is over it")
                .color("pressed_color", &UiButton::pressed_color, "Look while held down, or on")
                .color("disabled_color", &UiButton::disabled_color, "Look while disabled")
                .margins("padding", &UiButton::padding, 0.0, 4096.0, "Room around the label")
                .asset("click_sound", &UiButton::click_sound, {"audio"}, "Played on each click")));
        list.push_back(make_component<UiToggle>(
            "ui_toggle", "toggle", "Check box",
            "A check box or switch that flips on each click, with the node's label beside it.",
            ui_first_stable_id + 6U, &UiComponents::toggle,
            Table<UiToggle>{}
                .boolean("checked", &UiToggle::checked, "Whether it is on")
                .boolean("disabled", &UiToggle::disabled, "Ignore the pointer and look disabled")
                .choice("style", &UiToggle::style, {"check", "switch"}, "A check box or a sliding switch")
                .number("box_size", &UiToggle::box_size, 4.0, 1024.0, "Height of the box or switch")
                .number("spacing", &UiToggle::spacing, 0.0, 1024.0, "Gap between the box and the label")
                .color("box_color", &UiToggle::box_color, "Box, or the switch's track while off")
                .color("check_color", &UiToggle::check_color, "Check mark, or the switch's track while on")
                .asset("click_sound", &UiToggle::click_sound, {"audio"}, "Played on each click")));
        list.push_back(make_component<UiSlider>(
            "ui_slider", "slider", "Slider", "A value chosen by dragging a handle along a track.",
            ui_first_stable_id + 7U, &UiComponents::slider,
            Table<UiSlider>{}
                .number("value", &UiSlider::value, -1e9, 1e9, "The current value")
                .number("minimum", &UiSlider::minimum, -1e9, 1e9, "Value at the start of the track")
                .number("maximum", &UiSlider::maximum, -1e9, 1e9, "Value at the end of the track")
                .number("step", &UiSlider::step, 0.0, 1e9, "Values snap to multiples of this; 0 is smooth")
                .boolean("vertical", &UiSlider::vertical, "Run bottom to top instead of left to right")
                .boolean("disabled", &UiSlider::disabled, "Ignore the pointer and look disabled")
                .number("track_thickness", &UiSlider::track_thickness, 0.0, 1024.0, "Thickness of the track")
                .number("handle_size", &UiSlider::handle_size, 0.0, 1024.0, "Diameter of the handle")
                .color("track_color", &UiSlider::track_color, "The track")
                .color("fill_color", &UiSlider::fill_color, "The track up to the handle")
                .color("handle_color", &UiSlider::handle_color, "The handle")));
        list.push_back(make_component<UiProgressBar>(
            "ui_progress_bar", "progress_bar", "Progress bar",
            "Fills part of the control from one edge, for health, loading or timers. The node's "
            "panel is the empty track.",
            ui_first_stable_id + 8U, &UiComponents::progress_bar,
            Table<UiProgressBar>{}
                .number("value", &UiProgressBar::value, -1e9, 1e9, "The current value")
                .number("minimum", &UiProgressBar::minimum, -1e9, 1e9, "Value when empty")
                .number("maximum", &UiProgressBar::maximum, -1e9, 1e9, "Value when full")
                .choice("direction", &UiProgressBar::direction,
                        {"left_to_right", "right_to_left", "bottom_to_top", "top_to_bottom"},
                        "The edge it fills from, and which way")
                .color("fill_color", &UiProgressBar::fill_color, "The filled part")
                .number("fill_inset", &UiProgressBar::fill_inset, 0.0, 1024.0, "Gap between the fill and the edges")));
        list.push_back(make_component<UiContainer>(
            "ui_container", "container", "Container",
            "Arranges the child controls in a column, a row or a grid, sizing them from their "
            "minimum sizes and expand settings.",
            ui_first_stable_id + 9U, &UiComponents::container,
            Table<UiContainer>{}
                .choice("layout", &UiContainer::layout, {"vertical", "horizontal", "grid"},
                        "A column, a row, or a grid filled row by row")
                .number("spacing", &UiContainer::spacing, 0.0, 4096.0, "Gap between children")
                .margins("padding", &UiContainer::padding, 0.0, 4096.0, "Room inside the edges")
                .choice("align", &UiContainer::align, {"start", "center", "end"},
                        "Where children gather along the layout when none expands")
                .integer("columns", &UiContainer::columns, 1.0, 64.0, "Grid: children per row")));
        return list;
    }();
    return components;
}

const UiComponentInfo* find_ui_component(const std::string_view name) {
    for (const auto& component : ui_components())
        if (component.id == name || component.key == name) return &component;
    return nullptr;
}

std::optional<std::size_t> ui_field_index(const UiComponentInfo& component, const std::string_view name) {
    for (std::size_t index = 0; index < component.fields.size(); ++index)
        if (component.fields[index].name == name) return index;
    return std::nullopt;
}

bool valid_ui_asset_path(const std::string_view path, const std::vector<std::string_view>& kinds) {
    if (path.empty() || path.size() > 128U) return false;
    const std::filesystem::path relative{std::string(path)};
    if (relative.is_absolute()) return false;
    for (const auto& part : relative) {
        const auto name = part.string();
        if (name.empty() || name.front() == '.') return false;
        for (const char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                  c == '_' || c == '.' || c == ' '))
                return false;
    }
    return std::any_of(kinds.begin(), kinds.end(), [&](std::string_view kind) { return extension_suits(path, kind); });
}

bool check_ui_value(const UiFieldInfo& field, const UiValue& value, std::string& error) {
    const auto fail = [&](std::string message) {
        error = std::string(field.name) + ' ' + message;
        return false;
    };
    const auto range = [&] {
        return "must be from " + number_json(field.minimum) + " to " + number_json(field.maximum);
    };
    switch (field.type) {
    case UiFieldType::boolean:
        return std::holds_alternative<bool>(value) || fail("must be true or false");
    case UiFieldType::number:
    case UiFieldType::integer: {
        const auto* number = std::get_if<double>(&value);
        if (!number) return fail("must be a number");
        if (!finite_within(*number, field.minimum, field.maximum)) return fail(range());
        if (field.type == UiFieldType::integer && std::floor(*number) != *number) return fail("must be a whole number");
        return true;
    }
    case UiFieldType::vec2: {
        const auto* vector = std::get_if<Vec2>(&value);
        if (!vector) return fail("must be two numbers");
        return (finite_within(vector->x, field.minimum, field.maximum) &&
                finite_within(vector->y, field.minimum, field.maximum)) ||
               fail("values " + range());
    }
    case UiFieldType::margins: {
        const auto* margins = std::get_if<UiMargins>(&value);
        if (!margins) return fail("must be four numbers: left, top, right, bottom");
        for (const double side : {margins->left, margins->top, margins->right, margins->bottom})
            if (!finite_within(side, field.minimum, field.maximum)) return fail("values " + range());
        return true;
    }
    case UiFieldType::color: {
        const auto* color = std::get_if<UiColor>(&value);
        if (!color) return fail("must be a color: red, green, blue and alpha");
        for (const double channel : {color->r, color->g, color->b, color->a})
            if (!finite_within(channel, 0.0, 1.0)) return fail("channels must be from 0 to 1");
        return true;
    }
    case UiFieldType::text: {
        const auto* text = std::get_if<std::string>(&value);
        if (!text) return fail("must be text");
        return static_cast<double>(text->size()) <= field.maximum ||
               fail("must be at most " + number_json(field.maximum) + " bytes");
    }
    case UiFieldType::asset: {
        const auto* path = std::get_if<std::string>(&value);
        if (!path) return fail("must be a project file path");
        if (path->empty() || valid_ui_asset_path(*path, field.kinds)) return true;
        std::string wanted;
        for (const auto kind : field.kinds) wanted += (wanted.empty() ? "" : " or ") + std::string(kind);
        return fail("must be empty or a project-relative " + wanted + " file");
    }
    case UiFieldType::choice: {
        const auto* name = std::get_if<std::string>(&value);
        if (name && std::find(field.choices.begin(), field.choices.end(), *name) != field.choices.end())
            return true;
        std::string names;
        for (const auto choice : field.choices) names += (names.empty() ? "" : ", ") + std::string(choice);
        return fail("must be one of " + names);
    }
    }
    return fail("has an unknown type");
}

std::optional<UiValue> ui_value_from_json(const UiFieldInfo& field, const JsonValue& value, std::string& error) {
    const auto numbers = [&](std::size_t count) -> std::optional<std::vector<double>> {
        const auto* array = value.array();
        if (!array || array->size() != count) return std::nullopt;
        std::vector<double> result;
        for (const auto& item : *array) {
            if (!item.number()) return std::nullopt;
            result.push_back(*item.number());
        }
        return result;
    };
    std::optional<UiValue> result;
    switch (field.type) {
    case UiFieldType::boolean:
        if (value.boolean()) result = UiValue{*value.boolean()};
        break;
    case UiFieldType::number:
    case UiFieldType::integer:
        if (value.number()) result = UiValue{*value.number()};
        break;
    case UiFieldType::vec2:
        if (const auto list = numbers(2U)) result = UiValue{Vec2{(*list)[0], (*list)[1]}};
        break;
    case UiFieldType::color:
        if (const auto list = numbers(4U)) result = UiValue{UiColor{(*list)[0], (*list)[1], (*list)[2], (*list)[3]}};
        else if (const auto rgb = numbers(3U)) result = UiValue{UiColor{(*rgb)[0], (*rgb)[1], (*rgb)[2], 1.0}};
        break;
    case UiFieldType::margins:
        if (const auto list = numbers(4U)) result = UiValue{UiMargins{(*list)[0], (*list)[1], (*list)[2], (*list)[3]}};
        else if (value.number()) result = UiValue{UiMargins{*value.number(), *value.number(), *value.number(), *value.number()}};
        break;
    case UiFieldType::text:
    case UiFieldType::asset:
    case UiFieldType::choice:
        if (value.string()) result = UiValue{*value.string()};
        break;
    }
    if (!result) {
        static constexpr std::array<std::string_view, 9> expected{
            "true or false", "a number", "a whole number", "[x, y]", "[red, green, blue, alpha]",
            "[left, top, right, bottom]", "a string", "a file path string", "a name string"};
        error = std::string(field.name) + " must be " + std::string(expected[static_cast<std::size_t>(field.type)]);
        return std::nullopt;
    }
    if (!check_ui_value(field, *result, error)) return std::nullopt;
    return result;
}

std::string ui_value_json(const UiValue& value) {
    if (const auto* flag = std::get_if<bool>(&value)) return *flag ? "true" : "false";
    if (const auto* number = std::get_if<double>(&value)) return number_json(*number);
    if (const auto* vector = std::get_if<Vec2>(&value))
        return '[' + number_json(vector->x) + ',' + number_json(vector->y) + ']';
    if (const auto* color = std::get_if<UiColor>(&value))
        return '[' + number_json(color->r) + ',' + number_json(color->g) + ',' + number_json(color->b) + ',' +
               number_json(color->a) + ']';
    if (const auto* margins = std::get_if<UiMargins>(&value))
        return '[' + number_json(margins->left) + ',' + number_json(margins->top) + ',' +
               number_json(margins->right) + ',' + number_json(margins->bottom) + ']';
    return '"' + json_escape(std::get<std::string>(value)) + '"';
}

std::string ui_components_json(const UiComponents& ui) {
    std::string output;
    for (const auto& component : ui_components()) {
        if (!component.present(ui)) continue;
        output += output.empty() ? "{" : ",";
        output += '"' + std::string(component.key) + "\":{";
        for (std::size_t index = 0; index < component.fields.size(); ++index) {
            if (index) output += ',';
            output += '"' + std::string(component.fields[index].name) + "\":" + ui_value_json(component.get(ui, index));
        }
        output += '}';
    }
    return output.empty() ? "null" : output + '}';
}

bool read_ui_components(const JsonValue& value, UiComponents& ui, std::string& error) {
    ui = {};
    if (value.is_null()) return true;
    const auto* object = value.object();
    if (!object) {
        error = "ui must be an object or null";
        return false;
    }
    for (const auto& [key, fields] : *object) {
        const auto* component = find_ui_component(key);
        if (!component || component->key != key) {
            error = "unknown UI component " + key;
            return false;
        }
        if (!fields.object()) {
            error = "UI component " + key + " must be an object";
            return false;
        }
        component->attach(ui, true);
        for (const auto& [name, item] : *fields.object()) {
            const auto index = ui_field_index(*component, name);
            if (!index) {
                error = "unknown field " + name + " in UI component " + key;
                return false;
            }
            const auto parsed = ui_value_from_json(component->fields[*index], item, error);
            if (!parsed) return false;
            component->set(ui, *index, *parsed);
        }
    }
    return valid_ui(ui, &error);
}

bool apply_ui_values(UiComponents& ui, const UiComponentInfo& component, const JsonValue& values,
                     std::string& error) {
    const auto* object = values.object();
    if (!values.is_null() && !object) {
        error = "values must be an object of field names";
        return false;
    }
    auto changed = ui;
    component.attach(changed, true);
    if (object)
        for (const auto& [name, item] : *object) {
            const auto index = ui_field_index(component, name);
            if (!index) {
                std::string names;
                for (const auto& field : component.fields)
                    names += (names.empty() ? "" : ", ") + std::string(field.name);
                error = std::string(component.name) + " has no field " + name + "; its fields are " + names;
                return false;
            }
            const auto parsed = ui_value_from_json(component.fields[*index], item, error);
            if (!parsed) return false;
            component.set(changed, *index, *parsed);
        }
    normalize_ui(changed);
    if (!valid_ui(changed, &error)) return false;
    ui = std::move(changed);
    return true;
}

void normalize_ui(UiComponents& ui) {
    if (auto& slider = ui.slider; slider && slider->minimum < slider->maximum) {
        auto value = std::clamp(slider->value, slider->minimum, slider->maximum);
        if (slider->step > 0.0)
            value = std::min(slider->minimum + std::round((value - slider->minimum) / slider->step) * slider->step,
                             slider->maximum);
        slider->value = value;
    }
    if (auto& bar = ui.progress_bar; bar && bar->minimum < bar->maximum)
        bar->value = std::clamp(bar->value, bar->minimum, bar->maximum);
}

bool valid_ui(const UiComponents& ui, std::string* error) {
    std::string message;
    const auto fail = [&](std::string text) {
        if (error) *error = std::move(text);
        return false;
    };
    for (const auto& component : ui_components()) {
        if (!component.present(ui)) continue;
        for (std::size_t index = 0; index < component.fields.size(); ++index)
            if (!check_ui_value(component.fields[index], component.get(ui, index), message))
                return fail(std::string(component.name) + ": " + message);
    }
    const bool widget = ui.panel || ui.label || ui.image || ui.button || ui.toggle || ui.slider ||
                        ui.progress_bar || ui.container;
    if (widget && !ui.control) return fail("UI widgets need the node's Control component");
    if (ui.canvas && ui.control) return fail("a Canvas node cannot also be a Control; put controls under it");
    const int interactive = (ui.button ? 1 : 0) + (ui.toggle ? 1 : 0) + (ui.slider ? 1 : 0) + (ui.progress_bar ? 1 : 0);
    if (interactive > 1) return fail("a node carries at most one of Button, Check box, Slider and Progress bar");
    if (ui.control && (ui.control->anchor_min.x > ui.control->anchor_max.x ||
                       ui.control->anchor_min.y > ui.control->anchor_max.y))
        return fail("Control: anchor_min must not exceed anchor_max");
    if (ui.slider) {
        const auto& slider = *ui.slider;
        if (!(slider.minimum < slider.maximum)) return fail("Slider: minimum must be below maximum");
        if (slider.value < slider.minimum || slider.value > slider.maximum)
            return fail("Slider: value must lie between minimum and maximum");
        if (slider.step > slider.maximum - slider.minimum) return fail("Slider: step must not exceed its range");
    }
    if (ui.progress_bar) {
        const auto& bar = *ui.progress_bar;
        if (!(bar.minimum < bar.maximum)) return fail("Progress bar: minimum must be below maximum");
        if (bar.value < bar.minimum || bar.value > bar.maximum)
            return fail("Progress bar: value must lie between minimum and maximum");
    }
    return true;
}

namespace {
struct AnchorPreset {
    std::string_view name;
    double left, top, right, bottom;
};
const std::array<AnchorPreset, 16> anchor_presets{{
    {"top_left", 0.0, 0.0, 0.0, 0.0},       {"top", 0.5, 0.0, 0.5, 0.0},
    {"top_right", 1.0, 0.0, 1.0, 0.0},      {"left", 0.0, 0.5, 0.0, 0.5},
    {"center", 0.5, 0.5, 0.5, 0.5},         {"right", 1.0, 0.5, 1.0, 0.5},
    {"bottom_left", 0.0, 1.0, 0.0, 1.0},    {"bottom", 0.5, 1.0, 0.5, 1.0},
    {"bottom_right", 1.0, 1.0, 1.0, 1.0},   {"full_rect", 0.0, 0.0, 1.0, 1.0},
    {"top_wide", 0.0, 0.0, 1.0, 0.0},       {"bottom_wide", 0.0, 1.0, 1.0, 1.0},
    {"left_wide", 0.0, 0.0, 0.0, 1.0},      {"right_wide", 1.0, 0.0, 1.0, 1.0},
    {"vcenter_wide", 0.5, 0.0, 0.5, 1.0},   {"hcenter_wide", 0.0, 0.5, 1.0, 0.5},
}};
} // namespace

const std::vector<std::string_view>& ui_anchor_presets() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> result;
        for (const auto& preset : anchor_presets) result.push_back(preset.name);
        return result;
    }();
    return names;
}

bool apply_ui_anchor_preset(UiControl& control, const std::string_view preset, std::optional<Vec2> size,
                            const double margin) {
    const auto found = std::find_if(anchor_presets.begin(), anchor_presets.end(),
                                    [&](const AnchorPreset& entry) { return entry.name == preset; });
    if (found == anchor_presets.end()) return false;
    if (!size) {
        const auto extent = [](double low, double high, double anchor_low, double anchor_high) {
            return anchor_low == anchor_high ? std::max(high - low, 0.0) : 100.0;
        };
        size = Vec2{extent(control.offset_min.x, control.offset_max.x, control.anchor_min.x, control.anchor_max.x),
                    extent(control.offset_min.y, control.offset_max.y, control.anchor_min.y, control.anchor_max.y)};
    }
    // Each axis: stretched between two anchors, or a fixed length hung from one.
    const auto place = [margin](double low, double high, double length, double& offset_low, double& offset_high) {
        if (low != high) {
            offset_low = margin;
            offset_high = -margin;
        } else if (low == 0.0) {
            offset_low = margin;
            offset_high = margin + length;
        } else if (low == 1.0) {
            offset_low = -margin - length;
            offset_high = -margin;
        } else {
            offset_low = -length * 0.5;
            offset_high = length * 0.5;
        }
    };
    control.anchor_min = {found->left, found->top};
    control.anchor_max = {found->right, found->bottom};
    place(found->left, found->right, size->x, control.offset_min.x, control.offset_max.x);
    place(found->top, found->bottom, size->y, control.offset_min.y, control.offset_max.y);
    return true;
}

} // namespace relay
