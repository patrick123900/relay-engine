# Gameplay scripting

Relay gameplay code is native C++20. Scripts live in a project's `scripts/` folder, compile into a
shared library in the background, and run during **Run Game** at the fixed 60 Hz game step. The
whole API is one header, [`sdk/relay_script.hpp`](../sdk/relay_script.hpp); agents read the same
file through `scripts.sdk`.

## A first behaviour

```cpp
#include "relay_script.hpp"

class Spinner : public relay::Behaviour {
public:
    void on_update(double dt) override {
        self().set_rotation(self().rotation() + relay::Vec3{0, speed * dt, 0});
    }
private:
    double speed = 90.0; // degrees per second
};
RELAY_BEHAVIOUR(Spinner)
```

1. Select a node, press **+ Add Component** in the Inspector and choose **New C++ script...** in
   the Add Component window, or
   use **Create → C++ script...** in the Assets panel, or write the file with any editor.
   Subfolders and shared headers under `scripts/` work.
2. Once scripts have built, built behaviours appear under **Scripts** in the Add Component window. A
   node can carry several script components, including the same behaviour twice.
3. Press **Run Game**. The first time, Relay asks you to trust the project (see below), builds its
   scripts, and starts the game once the build succeeds.

## Properties

A behaviour lists its editable fields in `properties()`:

```cpp
class Mover : public relay::Behaviour {
public:
    void properties(relay::Properties& p) override {
        p.add("speed", speed);
        p.add("axis", axis);
    }
    void on_update(double dt) override {
        self().set_position(self().position() + axis * (speed * dt));
    }
private:
    double speed = 2.0;
    relay::Vec3 axis{1, 0, 0};
};
RELAY_BEHAVIOUR(Mover)
```

`bool`, `int`, `float`, `double`, `relay::Vec3` and `std::string` fields are supported. The
Inspector shows each declared property on every script component using that behaviour; changing a
value stores it on that node (in the scene file), and **Reset** returns it to the code's default.
Properties a node has not changed follow the code, so editing a default in code updates every node
that kept it. Relay assigns stored values after construction and before `on_start`. A stored
value whose field was renamed or changed type is skipped with a warning in the log.

## Input

Scripts read the project's input map, edited under **Edit → Game Configuration... → Input**:

```cpp
void on_update(double dt) override {
    const auto move = relay::input::vector("move_x", "move_y"); // length at most 1
    self().set_position(self().position() + relay::Vec3{move.x, 0, -move.y} * (speed * dt));
    if (relay::input::pressed("jump")) self().apply_impulse({0, 5, 0});
}
```

- `pressed`, `held` and `released` take an action name. Input is latched once per game step, so
  `pressed` is true in exactly one `on_update` per press, even for a tap shorter than a step.
- `axis` returns a value from -1 to 1 after the axis deadzone; `vector` combines two axes and
  scales diagonals down to length 1.
- `key_held`/`key_pressed` read raw keys by physical position ("w", "space", "left_shift"),
  `mouse_held`/`mouse_pressed` read "left", "middle" and "right", and `mouse_position`,
  `mouse_delta` and `mouse_wheel` read the pointer. Prefer actions, so players and other people can
  rebind them.
- Unknown action or axis names read as released and zero.

## Callbacks

## Built-in gameplay

Some gameplay is built into the engine as components, so it needs no script: **Node › Physics Body
› First Person Controller** in the Add Node window is a ready player you can walk around with.
Scripts can still read and adjust it: the controller runs each step before `on_update`.

A behaviour can override `on_start`, `on_update(dt)`, `on_contact_begin(other)`,
`on_contact_end(other)`, `on_stop` and `on_reload`. Scripts can find a child node by name (`self().child("Camera")`), make a
camera the one the game renders through (`camera.make_active_camera()`, restored by Stop Game),
read and set local transforms,
read world positions, set velocities, apply impulses, raycast against the live physics world, find
entities by name and log to the editor. The header documents each call.

Each frame runs every `on_update`, then animation and physics, then the contact callbacks for that
step. Stop Game calls `on_stop` and restores the authored scene, so scripts can change the scene
freely during a run.

## Trust

Scripts are native code. They run inside the editor with your full user permissions, like any
program you install, so a project's scripts never compile or run until a person trusts that
project. Trust is stored per user in `~/.config/relay-engine/trusted-script-projects` (on Linux),
keyed by the project's folder, so a project cannot mark itself trusted. Moving the folder removes
the trust. The `scripts.trust` method is host-only: agents cannot call it.

Agents can write script source in any project, but it only compiles once the project is trusted.
**In a trusted project, code an agent writes runs on your machine.** Review agent-written scripts
the way you would review any other code you run.

**Diagnostics → Scripts** shows trust, build state and errors, and can revoke trust.

## Building and hot reload

Relay compiles each changed `.cpp` separately and caches the object files, so a rebuild only
recompiles what changed. A changed header recompiles every script. Build output goes to the
project's hidden `.relay-cache/scripts/` folder, which project export skips.

With **Build on change** (on by default), the editor rebuilds whenever script files change on
disk. If a build finishes during Run Game, the running behaviours are swapped for the new code
without stopping the game. Each instance is recreated and its `on_reload` runs; by default that
calls `on_start`. Member variables start fresh, while state in the scene (transforms, velocities)
carries over.

Compiler errors appear in **Diagnostics → Scripts** with file and line. `scripts.status` reports
the same information. Run Game refuses to start while scripts are building, failed to build, have
changed since the last build, or reference a behaviour the scripts don't define.

The compiler is the one Relay was built with. Set `RELAY_SCRIPT_COMPILER` to use another, and
`RELAY_SCRIPT_SDK_DIR` to point at a different copy of the SDK folder.

## Errors and limits

- An exception escaping a callback disables that one instance and is reported with its entity,
  behaviour and callback. The rest of the game keeps running. Hot reload gives disabled instances
  a fresh start.
- A crash (bad pointer, abort) or an endless loop in a script takes the editor down with it. This
  is the cost of native speed.
- Register behaviours with unqualified class names.
- Collider shapes keep the scale they had when Run Game started, even if a script rescales the
  entity.
- Scripts cannot yet create or destroy entities, instantiate templates, or read input.
- Scripts are verified on Linux. The macOS path (`.dylib`, `dlopen`) exists but is untested, and
  Windows reports scripts as unsupported for now.

## For agents

The protocol methods are `scripts.sdk`, `scripts.read`, `scripts.write`, `scripts.create`,
`scripts.build` and `scripts.status`, plus `component.add`/`component.remove` with component
`script`, `scene.set_script` (behaviour and enabled state, by index) and
`scene.set_script_property`. `component.types` lists built behaviours with their properties and
code defaults. `input.map` and `input.set_map` read and replace the input map, and
`input.simulate` holds an action or sets an axis for a number of steps during Run Game, so agents
can play-test with `runtime.step` and check the result. The loop is: read the SDK, write the
source, build, poll `scripts.status` until the state is `ready` or `failed`, fix any diagnostics,
then call `runtime.play`, and check `runtime_errors` and the logs while the game runs. See
[`docs/protocol.md`](protocol.md) for the parameters.
