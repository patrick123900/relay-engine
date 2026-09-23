<p align="center">
  <img src="docs/images/relay-mark.svg" width="96" alt="Relay Engine logo">
</p>

<h1 align="center">Relay Engine</h1>

<p align="center">
  A native 3D workspace where people and software agents build together.
</p>

<p align="center">
  <img alt="C++ 20" src="https://img.shields.io/badge/C%2B%2B-20-5b8def">
  <img alt="Vulkan" src="https://img.shields.io/badge/renderer-Vulkan-ac6cff">
  <img alt="status" src="https://img.shields.io/badge/status-experimental-f0b44d">
</p>

![Relay Editor](docs/images/relay-editor.webp)

Relay is an experimental game engine and editor built around a shared control surface. Human UI,
automation, tests, and model tools all operate through the same typed protocol. Agents can inspect
the scene, edit it, render the result, review captures, and correct their work while a person stays
in control of the same project.

## Highlights

- **Native editor** — dockable hierarchy, inspector, viewport, assets, history, diagnostics, and
  animation controls built with SDL3, Dear ImGui, and ImGuizmo.
- **Agent workspace** — embedded chat, OpenAI sign-in, model and reasoning controls, file
  attachments, inline media, usage meters, and permission management.
- **Visual feedback loop** — agents can frame objects, control an inspection camera, capture the
  Vulkan viewport, inspect the image, and iterate.
- **HDR lighting** — a floating-point scene target, camera exposure, procedural environment
  lighting, and tone mapping produce the final SDR viewport and captures.
- **One typed API** — 99 versioned native methods cover scene editing, rendering, projects,
  observability, capture, and session authorization. A generated MCP bridge exposes the supported
  model-facing subset.
- **Deterministic core** — fixed-step simulation, transactional undo/redo, strict scene validation,
  and a CPU renderer used as a headless test oracle.
- **Editor and game modes** — the editor does not advance simulation. **Run Game** starts a
  temporary scene session; **Stop Game** restores the authored scene. Pause and single-frame step
  remain available while the game runs.
- **Practical asset pipeline** — glTF/GLB, OBJ, FBX, DAE, and sandboxed Blender conversion on Linux,
  with PBR materials, animation, skinning, morph targets, cameras, punctual lights, stabilized
  cascaded directional shadows, spot shadows, and point-light cubemap shadows.
- **Bounded GPU uploads** — explicit staging and estimated device-memory limits, upload telemetry,
  and transfer-queue mip generation when a dedicated queue is available.
- **Scene authoring and export** — editable, undoable transform keyframes and portable project tar
  packages of saved scenes and assets.
- **Jolt physics** — authored box colliders support oriented overlap checks and raycasts.
  Static and dynamic rigid bodies provide gravity, momentum, friction, bounce, and angular motion
  during Run Game. Runtime impulses can be applied at a world-space point through the protocol.

Select an entity and open **Transform keyframes** in the Inspector to add, edit, scrub, or play
position, rotation, and scale keys. To share a project, save its scenes, then use **Export saved
project** in the Project panel. Relay writes an uncompressed `.tar` under that project's `exports/`
folder, containing the project metadata, member scenes, and non-hidden assets. Export refuses to
overwrite an existing package.

Use **Run Game** from the toolbar or Run menu to test the current scene. The game viewport uses the
active scene camera. Scene edits and saves are unavailable during the run; **Stop Game** restores
the scene as it was when the run began. Pause and frame step control the running game only.
Select an entity and open **Box collider** in the Inspector to add a collider independent of its
visible mesh. Its layer and mask determine which other colliders it can overlap; raycasts can also
filter by layer. The queries report intersections without changing objects. The editor camera
shows collider wireframes in the viewport: selected colliders are amber, enabled colliders are
green, and disabled colliders are gray. Toggle them with **View → Collider wireframes**.
The editor viewport also shows clickable camera and light icons. Directional, point, and spot
lights have distinct markers; selecting a camera shows a compact perspective or orthographic
view guide. The guide preserves the camera's field of view and aspect but caps its display size.
These guides can be toggled from **View** and stay out of Run Game.
Open **Physics body** in the Inspector to make an object static or dynamic. Dynamic bodies fall
under gravity and respond to enabled box colliders during Run Game. A collider without a body is
static. Mass, gravity scale, bounciness, friction, and linear/angular damping are editable;
Stop Game restores authored positions. Jolt uses continuous collision detection for dynamic
bodies. An off-center `physics.apply_impulse` command adds rotation as well as linear motion.
Bodies with transform keyframes follow those keys rather than dynamic simulation.
Setting a box-collider size axis to zero currently crashes the engine; use a small positive
thickness for flat surfaces such as floors until this is fixed.

## Built-in agent workspace

<p align="center">
  <img src="docs/images/agent-chat.webp" width="520" alt="Relay's embedded Agent chat showing an inline render, composer controls, and usage meters">
</p>

The Agent panel keeps conversation, file attachments, rendered images and videos, model controls,
permissions, activity, and account usage inside the editor. Agents work against the same live scene
as the human operator and can use viewport captures to check and refine their results.

<p align="center">
  <img src="docs/images/agent-pyramid.webp" width="760" alt="A pyramid assembled and rendered through Relay's agent tools">
  <br>
  <sub>A scene assembled and reviewed through Relay's agent tools.</sub>
</p>

## Project status

Relay is a Linux-first research project under active development. The editor, Vulkan renderer,
scene workflow, import pipeline, control protocol, MCP bridge, and embedded agent workspace are
functional. APIs and file formats may still change. Windows and macOS renderer parity, broader
import sandboxing, and sustained live-provider validation remain in progress.

The next engine work is collision contact begin/end events, followed by gameplay scripting tied
to Run Game and Stop Game. Joints and additional collider shapes follow those foundations.

## Build

You need CMake 3.25+, Ninja, Python 3.10+, and a C++20 compiler. The graphical editor additionally
needs SDL3, Vulkan, and `glslc`. CMake fetches pinned Jolt 5.6 sources for physics, plus Dear ImGui
and ImGuizmo sources when the editor is enabled.

```sh
cmake --preset dev
cmake --build --preset dev
./build/dev/relay_demo
```

The command above opens the SDL/Vulkan demo. For a display-free runtime:

```sh
./build/dev/relay_demo --headless
```

### Agent bridge

The bridge requires Node.js 24+ and a current Codex CLI with App Server support.

```sh
npm --prefix tools/mcp-bridge install
npm --prefix tools/mcp-bridge run build
./build/dev/relay_demo --editor
```

The final command opens the full editor and starts its agent bridge.

Open **Agent → Account** to sign in with ChatGPT or configure a compatible API. Relay keeps provider
credentials, conversation state, attachments, and logs under the ignored `.relay/` directory.
Every model action still crosses the native session authorization boundary.

## Test

The regular suite runs without controlling the desktop:

```sh
cmake --build --preset dev
ctest --preset dev
npm --prefix tools/mcp-bridge test
```

Windowed smoke tests live under `tests/editor_*_smoke.py`; they are intended for deliberate manual
or dedicated-desktop runs because they move focus and synthesize input. The focused
`tests/editor_hdr_upload_smoke.py` verifies exposure, upload telemetry, transform key rendering,
and portable project export against a live Vulkan window.

## Architecture

```mermaid
flowchart LR
    Human[Human editor] --> Protocol[Versioned control protocol]
    Agent[Agent bridge / MCP] --> Protocol
    Tests[Headless tests] --> Protocol
    Protocol --> Engine[Deterministic engine]
    Engine --> Scene[Scene and assets]
    Engine --> CPU[CPU test renderer]
    Engine --> GPU[Vulkan renderer]
    GPU --> Capture[Capture and visual review]
    Capture --> Agent
```

The engine library has no model-provider dependency. Provider integration and canonical chat state
live in the external TypeScript bridge; permission checks and mutations remain native. The protocol
schema generates its C++, TypeScript, and reference documentation, preventing the surfaces from
drifting apart.

## Documentation

- [Protocol reference](docs/protocol.md) — generated methods, parameters, and safety annotations
- [Agent bridge](tools/mcp-bridge/README.md) — provider integration and bridge behavior
- [Engineering handoff](HANDOFF.md) — current implementation state, limits, and next priorities
- [Agent instructions](AGENTS.md) — repository working and verification rules

## Near-term direction

1. Validate and improve long-running live-provider sessions.
2. Expand renderer correctness with image-based environment lighting, configurable effects, and
   resource streaming.
3. Add Windows and macOS backend and import-sandbox parity.
4. Extend game authoring with more component tracks and expand project export formats.

Relay currently targets local, single-user authoring. Treat project files and imported assets as
untrusted input; the loader, protocol, and importer intentionally fail closed at their boundaries.
