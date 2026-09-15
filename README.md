# Relay Engine

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
- a dependency-free CPU renderer used as a temporary backend and test oracle;
- a Vulkan presentation backend with swapchain recreation, synchronized frames and a GPU pipeline;
- scene-driven Vulkan draws with resolved hierarchy transforms and an explicit perspective camera;
- a compiled render graph with inspectable resource transitions and dependency validation;
- staged uploads into device-local mesh buffers plus reflected SPIR-V pipeline interfaces;
- persistent mesh/material scene components with built-in triangle, quad and color assets;
- a bounded bindless texture table with staged image uploads and GPU-generated mip chains;
- content-addressed project model import for glTF/GLB, FBX, OBJ, DAE and Blender files, with
  hierarchy instantiation and live Vulkan mesh-buffer refresh;
- synchronized Vulkan swapchain readback for agent-visible screenshots of the real GPU output;
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
cannot escape that directory. Version 3 adds persistent mesh renderers, while version 2 introduced
perspective cameras alongside the reflected component metadata, full-precision transforms and
allocator generations from version 1. The loader migrates all earlier versions, including version 0 files whose entities were top-level
and whose transform rotation field was named `rotation`. Unknown future versions,
duplicate handles, non-finite numbers, malformed component metadata, stale parents and hierarchy
cycles are rejected before the live scene is changed.

Model files live in `assets/`. Relay treats glTF 2.0 (`.gltf`/`.glb`) as its native, recommended
interchange format, matching Godot's preferred pipeline. The first importer also accepts FBX, OBJ,
DAE and Blender files through Assimp. Imported geometry, UVs, base material colors and node
hierarchies are available now; image textures, PBR channels, skeletons and animation playback are
reported as deferred rather than silently presented as complete. Blender-to-glTF conversion matching
Godot's exact `.blend` workflow is also planned.

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

Run a short real-presentation smoke test that draws, resizes the window, recreates the swapchain and
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
headless runtime, then exposes thirty-eight narrowly scoped tools with JSON Schema validation and safety
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

Near-term milestones:

1. Add image texture, PBR channel, skeleton and animation import to the model pipeline.
2. Add a Blender-to-glTF adapter matching Godot's `.blend` workflow.
3. Add depth buffering, opaque sorting and physically based material inputs.

## Rendering TODOs

- [ ] Add a rotating Vulkan staging-buffer readback ring for asynchronous GPU screenshots and
  WebM recording. Until this lands, synchronous screenshots can capture the real Vulkan swapchain,
  but background captures and video use the deterministic CPU stream. Revisit this before relying
  on recordings to diagnose shaders, ray tracing, DLSS/FSR, lighting or driver-specific artifacts.
