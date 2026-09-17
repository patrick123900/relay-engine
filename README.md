# Relay Engine

Agent desktop policy: follow [AGENTS.md](AGENTS.md). Never control mouse/keyboard, change focus, or open/show/activate/move/resize windows
without specific advance user approval for the current task. Use background/headless checks by
default. Desktop and windowed GPU tests below are optional, approval-only reproduction commands.
Agents must first run relevant background/headless builds and tests autonomously. Do not skip
these checks or default to asking for desktop access. Only ask after completing background checks
if an important verification gap cannot reasonably be covered without desktop interaction.


Relay is an experimental, native game engine designed as a shared workspace for human developers
and software agents. Its north star is simple: anything a human can inspect or operate in the editor
should also have a typed, observable and safe automation surface.

For a detailed continuation brief covering product requirements, completed work, known boundaries
and the recommended implementation order, see [`HANDOFF.md`](HANDOFF.md).

This repository currently contains the first vertical slice:

- a portable C++20 runtime with a fixed, deterministic timestep;
- generation-checked entities with transforms, hierarchy, reflection and transactional undo/redo;
- versioned scene files with strict validation, legacy migration and atomic cross-platform saves;
- a human-facing SDL3 window;
- a Dear ImGui editor with a scene tree, reflected inspector, viewport camera, transform gizmos,
  asset browser, undo history and diagnostics, whose every mutation is routed through the same
  control protocol agents use;
- a dependency-free CPU renderer used as a temporary backend and test oracle;
- a Vulkan presentation backend with swapchain recreation, synchronized frames and a GPU pipeline;
- scene-driven Vulkan draws with resolved hierarchy transforms and an explicit perspective camera;
- a compiled render graph with inspectable resource transitions and dependency validation;
- staged uploads into device-local mesh buffers plus reflected SPIR-V pipeline interfaces;
- depth testing with frustum culling and a front-to-back opaque draw order, so overlapping meshes
  resolve correctly no matter what order they were created in;
- persistent mesh/material scene components with built-in triangle, quad and color assets;
- a bounded bindless texture table with staged image uploads and GPU-generated mip chains;
- content-addressed project model import for glTF/GLB, FBX, OBJ, DAE and Blender files, with
  hierarchy instantiation and live Vulkan mesh, material and texture refresh;
- static glTF PBR import with generated normals/tangents, PNG/JPEG image decoding, color-space and
  sampler metadata, and base-color, metallic/roughness, normal, occlusion and emissive channels;
- an engine-owned asset registry with versioned SHA-256 asset identities, a project import manifest
  that rebuilds imported assets on scene load, and importer dependency reads sandboxed to `assets/`;
- fence-tracked Vulkan readback for screenshots and recording of the real GPU output;
- dependency-free PNG/BMP encoding plus a bounded asynchronous capture worker;
- short WebM recordings sampled without blocking frame encoding, with explicit dropped-frame counts;
- bounded CPU/GPU timing, process-memory, draw-call, resource and entity telemetry;
- deterministic command and keyboard/mouse/gamepad traces with timestep and random-seed checks;
- bounded, structured in-memory logging;
- an agent control protocol for status, pausing, exact frame stepping, capture and log queries;
- Vulkan loader, adapter and logical-device probing with ray tracing feature discovery;
- a loopback-only socket transport for simultaneous local tool integrations;
- tests that exercise the runtime and automation contract.

## Build and run

Requirements are CMake 3.25+, Ninja, a C++20 compiler and optionally SDL3 and FFmpeg. FFmpeg enables
WebM recording and its availability is exposed at runtime. When a Vulkan loader is
present but its development headers are missing, CMake fetches a checksum-pinned release directly
from Khronos into the build directory; nothing is installed system-wide.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/relay_demo
```

Run without a display and write a test frame:

```sh
./build/dev/relay_demo --headless
```

## Agent control protocol

`--agent-stdio` starts a deterministic headless runtime. Requests and responses are one JSON object
per line. This narrow transport is the initial automation boundary; an MCP server can map typed tools
onto it without linking model-specific concerns into the engine.

```sh
./build/dev/relay_demo --agent-stdio
```

`--editor-stdio` uses the same protocol while keeping the human-facing Vulkan window alive. All
requests are queued from standard input and executed on the render thread, so scene changes and
captures cannot race GPU presentation:

```sh
./build/dev/relay_demo --editor-stdio
```

## Human editor

`--editor` opens the Dear ImGui editor interface on the live Vulkan window, and
`--editor-ui-stdio` runs that interface and the agent transport against one runtime at the same
time:

```sh
./build/dev/relay_demo --editor
./build/dev/relay_demo --editor-ui-stdio
```

The editor holds no reference to the scene, the undo history or the asset registry. Every panel
issues the same newline-delimited JSON requests an agent sends, through the same `ControlProtocol`
instance, so a human drag in the inspector and an agent's `scene.set_transform` are literally the
same native operation, land in the same undo history and are recorded in the same deterministic
trace. Native tests assert that contract directly.

The six movable panels are Hierarchy, Inspector, Assets, History, Diagnostics and Viewport.
They provide hierarchy selection/reparenting/renaming, component and animation editing, model
import, undo history, logs and scene presentation. Playback, stepping, undo/redo and editing modes
live in a fixed toolbar; scene save/load lives in File.

The menu bar provides **File, Edit, Scene, View, Run, Tools, Layout and Help**. New/Open/Save As
use project-local filename dialogs; `Ctrl+N`, `Ctrl+O`, `Ctrl+S` and `Ctrl+Shift+S` provide quick
access. The window title carries the scene's filename, or `Untitled scene`, and an asterisk while
there is unsaved work. Starting a new scene, opening another one or quitting with unsaved work asks
first; `Enter` saves and continues when the scene already has a file and `Escape` cancels. Saving is
what clears the marker, and undoing back to a saved state clears it too, because the comparison is
between scene revisions rather than a count of edits. Animation playback time never enters the undo
history, so a playing clip does not report the scene as modified.
Scene can add empty nodes, quads, triangles, cameras and three light types. **Edit > Duplicate**
(`Ctrl+D`, also on the hierarchy context menu) copies the selection and everything beneath it beside
the original as one undoable step, names the copy apart from its source, and selects it so the next
edit lands on the copy. View can hide or
reopen every panel and reset the editor camera. Run controls simulation and exact single-frame
stepping. Tools queues real GPU PNG screenshots and records WebM at 30 fps, up to ten seconds,
with an early-stop action. Recording follows simulation frames: resume or step a paused runtime.
Capture dialogs identify the Vulkan source and explain that editor panels are excluded.
Help provides a controls reference and About window. Planned features such as project export,
scripting, physics, audio, wireframe and an agent workspace are disabled and marked Coming soon.

The authoring viewport uses a charcoal background based on `#202020` and an antialiased XZ grid.
Grid lines are one unit apart, with major lines every ten units and a distance fade from 35 to
90 units. Height levels are ten units apart: Y=0 holds until camera height 5, then smoothly
crossfades to Y=10 between camera heights 5 and 10. Y=10 holds until height 15, then crossfades to
Y=20 between heights 15 and 20. The same transitions mirror downward below ground, through
Y=-10, -20 and onward.
**View > Ground grid** toggles the grid. An empty scene shows no demo triangle.
The shader retains the original smoothstep crossfade. The user visually verified smooth fading
during both upward and downward camera movement after the correction (2026-09-17). Builds and
background checks also pass; headless editor tests do not verify those rendered pixels.

In editor-camera mode, selected meshes have a thin yellow-orange (`#FFB930`) silhouette outline.
It follows animated/deformed geometry, respects scene depth and material cutouts, and includes
drawable descendants when a parent is selected. The grid and outline follow the viewport's
position, size and stacking; they create no scene entities and stay out of exported captures.

The fixed toolbar beneath the menu bar uses drawn icons with tooltips for simulation, undo/redo,
camera mode, framing, transform modes and local/world axes. Its FPS display measures editor
presentation independently of simulation pause/step. Rotate uses a connected circular arrow and
Focus a centered solid dot; the timeline Loop control uses rounded opposing arrows. Dock-header menus sit immediately left of
their close buttons.

The perspective viewport uses Godot-style navigation: middle-drag orbits, Shift+middle-drag pans,
and the wheel dollies. Hold right mouse or toggle `Shift+F` for cursor-captured freelook: mouse
motion looks around, `WASD` flies, `E` rises and `Q` descends. Shift speeds up, Alt slows down, and
the wheel changes fly speed. Release right mouse, toggle `Shift+F` again, or press Escape to leave
freelook. `F` frames the selection; lights, cameras and empty nodes retain a useful standoff.
The editor opens maximized with charcoal panels and blue UI selections. Text defaults to
17 logical units (16 for diagnostics), and the UI follows SDL's desktop content scale. High-density
framebuffers keep fonts sharp on fractional Wayland scaling; monitor/scale changes update the
interface without restarting or multiplying the desktop scale twice.

Translate, rotate and scale gizmos use `W`, `E` and `R` outside freelook, with a local/world toggle.
Clicking an object selects it. `Delete` destroys the selection and
`Ctrl+Z` / `Ctrl+Shift+Z` undo and redo. Keyboard shortcuts are ignored while a text field has
focus.

The editor camera is view state, not scene state: it creates no entity, is never saved, never enters
the undo history and is never sent over the protocol, so navigating generates no trace entries. It
does apply to captures, because a screenshot should show the viewport the operator is looking at.
Use the toolbar camera icon or **View > Scene camera** to see the scene's active camera view.

Transform edits commit once per gesture rather than once per frame, so one inspector drag or one
gizmo drag is one undo entry, and values are carried as full-precision doubles to match Relay's
transform storage. A gizmo drag sends many updates carrying a shared gesture token, which the scene
history folds into the single transaction that token opened. Animation time sliders also update
the pose throughout a drag and fold the scrub into one undo entry.

Inspector widgets retain drafts through release and commit only the edited channels, preserving
other values and following later agent edits or undo. Imported morph targets can be overridden
without pre-existing overrides. Rendering, picking and gizmos share the central viewport, including
framebuffer scaling; picking and framing use the current skinned/morphed bounds.

Selection and picking are stateless on the engine side. `scene.pick` takes a world-space ray and
returns the nearest entity whose mesh bounds it enters, so an agent can pick without an editor
running, and `scene.bounds` reports an entity's world-space extent for framing or locating it. The
gizmo composes and decomposes transforms using Relay's own Euler order rather than the gizmo
library's, and a native test asserts that composition matches the matrix the renderer actually draws
with.

The interface is drawn into the swapchain render pass for presentation only and is deliberately
excluded from every capture and readback, so existing golden images stay comparable.

The six content panels can be resized with their dividers and moved by dragging their tabs.
The toolbar stays fixed and cannot be closed or moved. Drop a tab on a docking guide to place it beside another panel or group
panels into tabs. Hold Shift while dragging to keep a panel floating inside the editor window;
resize floating panels by their edges or corners. The layout is remembered in
`.relay/editor-layout.ini`; **Layout > Reset layout** restores the defaults. Layout changes do not
modify the scene or undo history. Old saved layouts automatically retire the movable Controls
panel while retaining the other panes. Separate desktop windows are not supported yet.

Panels poll read-only methods roughly twice a second. Read-only requests are not recorded in
deterministic traces, so polling cannot bury the operations that changed the scene.

Multi-selection supports Ctrl-click to toggle an object in the hierarchy or viewport, Shift-click
for a visible hierarchy range, and Ctrl+A to select all. Ctrl+C/X/V copy, cut and paste complete
subtrees through a session clipboard. Ctrl+D duplicates the selection and Delete removes it.
Selecting both a parent and its child processes the subtree once. Group gizmos apply a shared
world-space transform, framing includes the whole selection, and each group edit is one undo step.
All selected subtrees receive amber outlines; inspector properties edit the active object.
The clipboard survives deletion and scene/project changes within the same engine session. Whole
model copies remap animation bindings, copied cameras stay inactive, and a partial imported-node
copy drops its external animation binding. Copy itself does not mark a scene modified.

File > New/Open project manages folders containing a `*.relayproject` file, for example
`projects/my-game/project.relayproject`. Paths are relative to this workspace. The file's folder
is the project root: Assets discovers up to 4096 visible files recursively there, excluding hidden entries and symlinks.
Model imports use paths relative to it, and scenes are stored in its `scenes/` directory. Opening another project switches these roots
without changing the process working directory. Save adds scene membership; the Project panel
sets startup scenes and removes membership while keeping files. The project browser discovers
up to 128 project files under `projects/`; Open also accepts other safe workspace-relative paths. Project metadata is saved atomically
and kept separately from scene undo. Without an open project, standalone editing uses workspace
`assets/` and `scenes/`. Project, Timeline and History are hidden on startup; open them from View.
Panel tabs supply their titles without repeated headings inside the panels.

View > Timeline (also Tools > Animation timeline) opens the dockable animation panel. It shows
model-instance tracks, clip selection for the active track, a seconds ruler, Play/Pause, Restart,
frame stepping, Loop, playback speed and optional frame snapping at a chosen display FPS. Transport
uses vector icons with tooltips. Timeline shares the wider bottom dock with Diagnostics by default,
with a scrolling track area, decimal ruler labels and clipped channel captions. Dragging
the ruler pauses and seeks all selected animation roots immediately as one undo step, clamping to
each clip's duration. Selecting an imported child resolves its animation root. When no animation
root is selected, the timeline follows the first available one. Imported node/morph channels show
read-only key markers, bounded to 256 channels and 256 sampled keys per channel; full counts are
reported. Keyframe creation/editing and clip blending are not implemented.

Background verification is available through the normal `ctest --preset dev` command:

- `relay_workflow_tests` checks forest clipboard operations, group undo/rollback, project persistence
  and startup loading, filename/symlink rejection, synchronized seeks and timeline time conversion.
- `relay_editor_headless_tests` drives the actual editor through ImGui's internal input queue. It
  checks modifier selection, clipboard shortcuts, held-ruler scrubbing, close guards and project
  Save As/browser interaction without initializing SDL/Vulkan or opening windows. It is included
  when editor dependencies are available and uses temporary files and its own layout.

These tests do not move the OS pointer or send OS keyboard input. For background MCP work, set
`RELAY_RUNTIME_MODE=headless`; the bridge's automatic live-editor mode can open a desktop window.

The desktop suites below open windows, change focus and control the real mouse and keyboard.
Their isolated layout does not isolate desktop input. Agents must obtain specific user approval
before running them; otherwise leave desktop verification pending and continue background work.

`tests/editor_visuals_smoke.py` is the current real desktop check for amber mesh outlines,
vertical grid movement, capture isolation and resize/reframing. It requires a desktop session,
xdotool, Spectacle and Pillow, and uses an isolated layout. Only after specific user approval,
run it from the repository root:

```sh
python3 tests/editor_visuals_smoke.py
```

`tests/editor_workflow_smoke.py` is the current real desktop check for the everyday editing and
save workflow: duplication and its selection, the unsaved-work guard and its cancel, Save As, New
scene, reopening, and the animator's play and restart controls. It asserts engine state over the
protocol and reads the unsaved marker back from the window manager. It needs a desktop session and
xdotool, and uses an isolated layout:

```sh
python3 tests/editor_workflow_smoke.py
```

Synthetic key delivery on this desktop is not completely reliable — a repeated `Ctrl+O` reached the
editor most but not all of the time — so interactions with a checkable outcome are retried and the
suite prints when it retried. Every assertion is still made against engine state, so a real
regression fails rather than being retried away.

Earlier desktop suites cover hierarchy/gizmo interaction, inspector/morph edits, Godot navigation,
docking/persistence and menus (`tests/editor_{interaction,regression,navigation,layout,menu}_smoke.py`).
They passed before the font, maximized-startup and fixed-toolbar changes; their old hardcoded
coordinates and Controls-panel calibration need updating before they can validate the current
layout. These desktop checks are separate from `ctest`.

Multi-selection, clipboard operations, folder projects and the dockable playback timeline are
implemented. Embedded chat remains planned; capability grants must precede broader agent access.

Build the interface with `-DRELAY_ENABLE_EDITOR_UI=ON` (the default). Dear ImGui is fetched from a
checksum-pinned v1.92.1-docking release and ImGuizmo from a checksum-pinned commit; both are linked
into `relay_demo` and the headless editor test executable; the engine library stays free of UI dependencies.

Each request is a single line holding an `id`, a `method` and that method's parameters as
**top-level fields**. Parameters are not nested inside a `params` object, so this is not JSON-RPC.

Example requests:

```json
{"id":1,"method":"runtime.status"}
{"id":2,"method":"runtime.step","frames":10}
{"id":3,"method":"render.capture","path":"captures/agent-view.bmp"}
{"id":4,"method":"logs.read","after":0}
{"id":5,"method":"runtime.quit"}
```

The complete method, MCP-tool and parameter reference is generated from the versioned schema in
[`docs/protocol.md`](docs/protocol.md).

Scene files live in `scenes/`. Control and MCP calls accept only a filename—not a path—so they
cannot escape that directory. Version 4 adds animation state, model-node bindings, morph weights,
punctual lights and orthographic cameras. Version 3 added persistent mesh renderers, while version 2 introduced
perspective cameras alongside the reflected component metadata, full-precision transforms and
allocator generations from version 1. The loader migrates all earlier versions, including version 0 files whose entities were top-level
and whose transform rotation field was named `rotation`. Unknown future versions,
duplicate handles, non-finite numbers, malformed component metadata, stale parents and hierarchy
cycles are rejected before the live scene is changed.

Model files live in `assets/`. Relay treats glTF 2.0 (`.gltf`/`.glb`) as its native, recommended
interchange format, matching Godot's preferred pipeline. FBX, OBJ and DAE use Assimp. `.blend`
sources are converted through Blender's glTF exporter into a content-addressed cache before entering
the same import path. Imported static geometry includes UVs, generated normals/tangents, node
hierarchies, PBR factors and PNG/JPEG image textures. Dynamic imports include skeleton hierarchy,
inverse bind matrices, normalized skin weights, node/morph animation clips and morph targets.
Playback is deterministic and independently controlled per model instance. glTF/GLB preserves
LINEAR, STEP and CUBICSPLINE interpolation; CPU deformation is streamed to per-frame Vulkan buffers.
`scene.set_animation` selects, seeks and plays clips; `scene.set_morph` overrides or resets weights.
`render.assets` lists models/clips and mesh joint/morph counts, which is what lets the inspector
offer clips by name and bound its time slider by the selected clip's length rather than showing a
raw index and an open-ended number. FBX remains on Assimp, verified by
the Blender-exported animated/skin/morph fixture.

Imports are sandboxed. Models use safe relative paths within the active project folder (or
`assets/` for standalone editing), and every dependency they reference — external `.bin` buffers, images — is canonicalized and must
resolve inside that same directory. Relative escapes, absolute paths and symlinks that leave the
asset root are refused, reads are capped at 64 dependency files and 64 MiB each, and the importer is
given no write path. Blender conversion uses fixed arguments, disables file-embedded script
auto-execution, preflights export-related external paths, enforces a timeout and writes below
`.relay-cache/blender` below the active asset root. Source and external dependency hashes invalidate the cache. On Linux,
Bubblewrap isolates the filesystem, network and environment: system runtimes are read-only, assets
are read-only except for the conversion cache, and the user's home is not exposed. Conversion fails
closed on other platforms unless an administrator explicitly opts into trusted-input conversion with
`RELAY_BLENDER_TRUSTED=1`. Do not enable that mode for untrusted `.blend` files.

Blender is discovered as `blender` on `PATH`; set `RELAY_BLENDER_EXECUTABLE` to an administrator-
chosen executable when it is installed in a mounted system-runtime directory (`/usr` or `/opt` on
Linux). The `scene` import preset retains conversion-time
animation/camera/light channels, while `static_mesh` strips them from Blender's cached GLB.
Perspective and orthographic cameras are instantiated as inactive components under their original nodes,
preserving projection and hierarchy; they can be activated through the existing camera interface
and survive scene save/load. The static-mesh preset also excludes camera components for direct
imports, together with skinning, morphs and clips. Camera-only scenes are supported. Invalid
projection/orientation data produces diagnostics. Imported directional, point and spot lights
preserve color, attenuation, cone angles and optional range; `scene.set_light` controls them.
The initial Vulkan light budget is 16 lights per scene; area/ambient lights are diagnosed as unsupported.
Windows/macOS still fail closed for untrusted Blender conversion; the default OS sandbox is Linux-only.

Each import is content-addressed with a versioned SHA-256 digest (`sha256-v1-<hex>`), and recorded in
a project manifest at `assets/.relay-imports.json` alongside its source name, importer version,
dependency list and generated asset ids. Loading a scene rebuilds those assets first, so a scene
saved with imported meshes opens correctly in a fresh process without a manual reimport. If a source
file changed since the manifest was written, `scene.load` reports it, rebuilds the assets and rebinds
compatible saved mesh/material references by import index.

For a long-lived local connection, run Relay on an IPv4 loopback port:

```sh
./build/dev/relay_demo --agent-port 7777
```

The wire format is the same newline-delimited JSON used by `--agent-stdio`. The listener never binds
to a LAN or public interface. A future security milestone will add per-session capability grants
before any transport is allowed to perform filesystem or editor mutations.

Probe the graphics device directly:

```sh
./build/dev/relay_demo --probe-vulkan
```

Only after specific user approval, run a real-presentation smoke test that draws, resizes the window, recreates the swapchain and
draws again:

```sh
./build/dev/relay_demo --vulkan-smoke
```

Capture the actual Vulkan swapchain through a synchronized GPU-to-CPU transfer:

```sh
./build/dev/relay_demo --vulkan-capture captures/vulkan-frame.bmp
```

## MCP bridge

The official MCP TypeScript SDK powers Relay's first model-facing bridge. It launches and owns a
headless runtime, then exposes sixty-six narrowly scoped tools with JSON Schema validation and safety
annotations. Engine messages stay on a private child-process channel so MCP's standard output is
never polluted by runtime logs.

```sh
cd tools/mcp-bridge
npm install
npm run build
npm start
```

MCP hosts should launch `node tools/mcp-bridge/dist/index.js` from the repository root. Set
`RELAY_ENGINE_BINARY` only when the runtime lives somewhere other than `build/dev/relay_demo`.
The `render_capture` MCP tool uses synchronized Vulkan swapchain readback by default and offers a
`deterministic` source for display-free testing.
When a desktop is present, the bridge owns one live Vulkan editor process: scene operations,
simulation stepping, human presentation and GPU capture all address that same runtime. Set
`RELAY_RUNTIME_MODE=headless` for CI and servers without a display.

## Architecture direction

Relay's runtime remains model-agnostic. MCP, chat history, permissions and model providers belong in
an out-of-process Agent Bridge. The engine exposes one versioned control API shared by the editor,
tests, command-line tools and agents.

The windowed demo now uses Vulkan on Windows and Linux when SDL3, a Vulkan loader and `glslc` are
available. It compiles GLSL into SPIR-V, selects a presentation-capable adapter, creates distinct
graphics/present queues when required, and renders through a synchronized two-frame pipeline. The
CPU renderer remains the deterministic headless test and capture oracle. Direct3D 12 and Metal
backends remain planned for native Windows and macOS integration.

## Protocol development

`protocol/relay.protocol.json` is the sole definition of native method names, MCP tool names,
parameters, validation constraints, descriptions and safety annotations. After editing it, run:

```sh
python3 tools/generate_protocol.py
```

Both CMake builds and `npm run check` reject stale generated C++, TypeScript or documentation.

Near-term milestones, in the order they should be taken:

1. Phase G: add per-session capability grants and auditable action scopes before broader agent
   access, then embedded chat.
2. Phase H: expand import presets and replace whole-registry refresh with asynchronous resource uploads.
3. Continue editor authoring beyond the implemented playback timeline and folder project model:
   editable keyframes, external project folders/export and multi-object property editing.

See [`HANDOFF.md`](HANDOFF.md) for the full phase breakdown and the current list of known
boundaries.

## Asynchronous capture and recording

`render.capture_async` and `video.start` accept `source: "vulkan"` or
`source: "deterministic"`. Omitted sources use Vulkan in the live editor and the CPU oracle in
headless mode. An explicit Vulkan request requires a live renderer and never falls back to CPU.
Capture jobs and video status report their source. The synchronous MCP screenshot tool continues
to default to Vulkan. Job status includes provenance; callers must inspect failure/error fields
rather than treating an accepted request as a completed image or recording.

Two rotating staging slots retain the submitted frame's dimensions and pixel format, and transfer
completed pixels after their graphics fence signals. Image encoding and FFmpeg WebM finalization
run on background workers. Each GPU slot is capped at 64 MiB; the shared image queue has eight
active jobs. Full queues/rings report capture failures or recording drops. Cancellation through
`render.capture_cancel` discards queued pixels but retains GPU storage until completion.

`video.stop` drains pending GPU readbacks and starts finalization; poll `video.status` until
`finalizing` is false and check `error`. A recording keeps its first sampled resolution; frames
at a different resolution after resize count as drops. Start a new recording to use the new size.
Odd dimensions are padded for VP9. Shutdown drains readbacks and joins the workers.

Protocol v9 has 66 native methods and generated MCP tools. Project files use version 1. Scene v4 and manifest v3 are unchanged.
Linux/RADV is the verified platform; no Windows/macOS parity is claimed.

```sh
./build/dev/relay_demo --vulkan-async-smoke relay-dynamic-golden.gltf
python3 tests/mcp_capture_smoke.py
```

The GPU smoke exercises animation, resize, asynchronous PNG and WebM and writes evidence to
`/tmp/relay-phase-e.png` and `/tmp/relay-phase-e.webm`.

## Importing TODOs

- [ ] Add and verify OS-level Blender conversion sandboxes on Windows and macOS equivalent to
  Linux Bubblewrap. Until then, keep untrusted `.blend` conversion disabled on those platforms;
  `RELAY_BLENDER_TRUSTED=1` must remain an explicit administrator opt-in for trusted files only.
