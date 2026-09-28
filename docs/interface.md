# Game interface

Relay games draw their interface (HUDs, menus, buttons, health bars) with interface nodes, much as
Godot does with Control nodes and Unity with its canvas UI. Interface nodes draw over the game's
view **during Run Game only**. The editor's 3D viewport never shows them while you fly its camera;
the **Interface** panel shows them instead, at any screen size, and lets you place them by hand.

## Building an interface

Add a **Canvas** (Add Node → Canvas) and put controls under it. Adding a control somewhere without
a canvas above it creates one for you.

| Node type | What it is |
| --- | --- |
| Canvas | A layer for the interface. It scales everything below it to the screen and orders it against other canvases. |
| Control | An empty rectangle that holds and places other controls. It fills its parent when created. |
| Panel | A filled rectangle with rounded corners, a border and a soft shadow: windows, backdrops, bars. |
| Label | Text. |
| Image | A PNG or JPEG picture. |
| Button | A clickable button with a panel and a label. |
| Check Box | A check box or switch with a label. |
| Slider | A value chosen by dragging a handle. |
| Progress Bar | A bar filled to a value, for health, loading or timers. |
| Vertical Box, Horizontal Box, Grid | Containers that arrange their child controls in a column, a row or a grid. |

Node types are made of components, so they combine: give a Panel a Label for a titled box, or give
a Button an Image and remove its Panel for a picture button. **+ Add Component** lists them under
**UI**; widgets bring the Control that places them. A canvas node is never a control itself.

## Placing controls

A control's rectangle is set the Godot way. Each edge hangs from an **anchor**, a fraction of the
parent control's rectangle (or the screen, for controls directly under a canvas), and sits an
**offset** in canvas units from it. With both anchors of an axis equal, the control keeps its size
and moves with the anchor: anchors at the bottom right keep a minimap in the bottom-right corner on
every screen. With anchors at 0 and 1, the control stretches with its parent and the offsets are
margins.

The Inspector's **Anchors** menu has Godot's presets: corners, edge centers, the center, and wide
presets that stretch along an edge or fill the parent. Choosing one keeps the control's size. Below
it the Inspector shows each axis as a position and size, or as margins when the axis stretches;
**Anchors and offsets** has the raw numbers. **Pivot** is the point that **Rotation** and **Scale**
turn about, as fractions of the control's size. **Opacity** multiplies the control and everything
below it, **Visible** hides them all, and **Clip contents** cuts children off at the control's
edges. Siblings draw in increasing **Order** (the arrow buttons swap a control with its neighbor),
which is also the order the Hierarchy lists them and containers place them.

A **Canvas** scales its controls for the screen. With **Scale with screen size** (the default),
layouts are made for its **Reference size**, 1920 x 1080, and scaled to the screen, following its
width (**Match** 0), height (1) or a blend. **Constant pixel size** keeps one canvas unit at
**Scale** screen pixels. Higher **Sort order** canvases draw on top and get the pointer first.

### Containers

A container places its children itself; their anchors and offsets do nothing while they are in it.
**Vertical** stacks them, **Horizontal** lines them up, and **Grid** fills rows of **Columns**
children. Each child gets its minimum size along the layout: its **Min size**, or what its text,
check box or own children need, whichever is larger. Children with **Expand** on that axis share
the spare room; without any, **Align** gathers them at the start, center or end. Across the layout
each child fills its cell, or sits at its start, center or end, by its **Size x** and **Size y**.
**Padding** keeps room inside the container's edges and **Spacing** between children.

## The Interface editor

Open it with **Tools → Interface editor** or the **Open the Interface editor** button on a Canvas or
Control in the Inspector. It opens beside the viewport and draws the scene's interface as the game
would, over a checkerboard, at the screen size chosen at the top left: the game view's own size,
common resolutions, portrait, or a custom size. Check an interface at several sizes before relying
on it.

- Click a control to select it, and drag it to move it. The handles on the selected control resize
  it; **Ctrl** snaps to 8 units. The arrow keys nudge the selection by 1, or 8 with **Shift**. Each
  drag is one undo step.
- Green markers show where the selected control is anchored, and a circle its pivot.
- The mouse wheel zooms about the pointer, the middle button pans, and **Fit** frames the screen.
  **Outlines** shows every control's rectangle.
- Controls placed by a container cannot be dragged; change the container, or the child's minimum
  size and expand settings, instead.
- During Run Game it shows the game's interface as it is, and does not edit it.

Hidden controls (**Visible** off, such as a menu a script opens) do not draw; turn them on while you
work on them.

## Widgets

- **Panel**: fill **Color**, **Border** width and color, **Corner radius**, and a soft **Shadow**
  with its size and offset (a transparent shadow color draws none).
- **Label**: **Text** (new lines break lines), **Font** (a project `.ttf` or `.otf` file, or the
  built-in Inter), **Bold** for the built-in font, **Size** in canvas units, **Color**, horizontal
  and vertical alignment, **Wrap** at the control's width, **Line spacing**, and an **Outline** and
  drop **Shadow** for text over busy scenes.
- **Image**: a project PNG or JPEG, a tint **Color**, and a **Mode**: stretch, fit (keep the aspect
  inside the control), fill (keep the aspect and crop), center (the image's own size, one pixel per
  canvas unit), tile, or sliced, which keeps **Slice** pixels at each edge unstretched (nine-slice,
  for frames and buttons that stretch cleanly). It can be flipped either way.
- **Button**: the node's panel color is its normal look; **Hover color**, **Pressed color** and
  **Disabled color** replace it (or tint its image when it has no panel). **Toggle** makes each
  click flip **Pressed**, like a switch. **Padding** is room around the label, and **Click sound**
  plays a project sound on each click.
- **Check box**: **Checked**, a **Check** box or **Switch** style, the box's size and colors, the
  gap before the label, and a click sound.
- **Slider**: **Value** between **Minimum** and **Maximum**, snapping to **Step** when it is above
  zero; horizontal or **Vertical**; track, fill and handle colors and sizes. Click anywhere on the
  track or drag the handle.
- **Progress bar**: **Value** between **Minimum** and **Maximum**, filling from an edge in its
  **Direction** with the **Fill color**, inset from the edges. The node's panel is its empty track.

Interface colors are sRGB with alpha, from 0 to 1, as in design tools; light colors elsewhere in
Relay are linear. Translucent controls blend in linear light, so a half-transparent dark panel over
a bright scene looks a little lighter than the same values in a design tool.

## The pointer

Controls react to the mouse only while the cursor is free. A project whose input map locks the mouse
(**Edit → Game Configuration → Input → Lock the mouse cursor**, as in the demo) keeps the cursor
for looking around, and its interface only shows things until a script frees the cursor, as a menu
does. In the editor, click the viewport to give the game input first.

What the pointer does over a control is its **Mouse filter**:

- **Stop** takes the pointer: controls behind it and the game do not see clicks there. Panels,
  buttons, check boxes and sliders start with it.
- **Pass** lets clicks through to controls behind it and to the game.
- **Ignore** makes the control invisible to the pointer. Labels and images start with it.

A click the interface takes never reaches the game: pressing a button does not also fire.

## Scripts

Scripts change controls during the game and hear what the player does with them. `on_ui` reaches a
control's own scripts and those of every ancestor, so one script on a menu or canvas can handle all
of its buttons:

```cpp
#include "relay_script.hpp"

class PauseMenu : public relay::Behaviour {
public:
    void on_start() override {
        menu = relay::world::find("Menu");
        menu.set_visible(false);
    }
    void on_update(double) override {
        if (relay::input::key_pressed("tab")) open(!menu.visible());
    }
    void on_ui(const relay::ui::Event& event) override {
        if (event.clicked() && event.control.name() == "Resume") open(false);
        if (event.value_changed() && event.control.name() == "Volume")
            relay::audio::set_bus_volume("Music", 40.0 * std::log10(std::max(event.value, 0.001)));
    }
private:
    void open(bool show) {
        menu.set_visible(show);
        relay::input::set_mouse_locked(!show); // Free the cursor for the menu, lock it again after.
    }
    relay::Entity menu;
};
RELAY_BEHAVIOUR(PauseMenu)
```

- Events (`relay::ui::Event`): `clicked` (a button, check box or slider was pressed and released
  over it), `toggled` (a check box or toggle button flipped; `value` is 1 or 0), `value_changed` (a
  slider moved; `value` is its value), and `pressed` and `released`. `event.control` is the control.
- `entity.set_text(...)` and `text()` for labels; `set_visible` and `visible()`; `set_value` and
  `value()` for sliders and progress bars; `set_checked` and `checked()` for check boxes and toggle
  buttons; `hovered()` and `held()` for the pointer this step.
- `entity.set_ui("<component>.<field>", value)` sets any field the Inspector shows, such as
  `set_ui("panel.color", {1.0, 0.2, 0.2, 0.9})`, `set_ui("control.offset_min", {20.0, 20.0})` or
  `set_ui("label.horizontal_align", "center")`; `ui_numbers` and `ui_text` read them. Values are
  checked as in the Inspector, and a refusal is logged.
- `relay::input::set_mouse_locked(bool)` and `mouse_locked()` lock or free the cursor for the rest
  of the game.

Changes last until Stop Game restores the authored interface. See the
[scripting guide](scripting.md) for building and trusting scripts.

## Agents and the protocol

Agents build interfaces with the same operations: `scene.create` with an interface type,
`scene.set_ui` to add, change or remove a component (for example
`{"entity": "4:1", "component": "ui_label", "values": {"text": "Score: 0", "size": 32}}`, or
`"anchor_preset": "top_right"` for a control), and `component.types`, which lists every interface
field with its type, range, choices and default. Since the interface does not show in the editor's
view, `ui.render` draws it alone into a PNG at any screen size, while editing or during the game,
and `ui.layout` reports each control's rectangle in pixels, whether it shows, and the latest game
step's events. During Run Game, `ui.click` clicks a button, check box or slider as a player would.

## Text

The built-in font is [Inter](../third_party/inter/README.md), in regular and bold, embedded in the
engine. Project fonts are TrueType or OpenType files; characters a font lacks come from Inter.
Glyphs are rasterized at the size they appear on screen, so text stays sharp at every scale.
Kerning comes from a font's `kern` table; OpenType layout features, right-to-left scripts and
complex scripts such as Arabic or Devanagari are not shaped.

## Not yet supported

World-space interfaces (a screen in the 3D world), keyboard and gamepad focus navigation, text
input fields, scroll containers, rich text, and interface animation beyond what scripts do.
