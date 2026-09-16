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
`render.assets` lists models/clips and mesh joint/morph counts. FBX remains on Assimp, verified by
the Blender-exported animated/skin/morph fixture.

Imports are sandboxed. A model may only be named by a top-level filename inside `assets/`, and every
dependency it goes on to reference — external `.bin` buffers, images — is canonicalized and must
resolve inside that same directory. Relative escapes, absolute paths and symlinks that leave the
asset root are refused, reads are capped at 64 dependency files and 64 MiB each, and the importer is
given no write path. Blender conversion uses fixed arguments, disables file-embedded script
auto-execution, preflights export-related external paths, enforces a timeout and writes below
`assets/.relay-cache/blender`. Source and external dependency hashes invalidate the cache. On Linux,
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

Near-term milestones, in the order they should be taken:

1. Add asynchronous Vulkan capture/readback and the human editor interface.
2. Expand import presets as those data types become editable in Relay.
3. Replace whole-registry hot refresh with versioned asynchronous resource uploads.

See [`HANDOFF.md`](HANDOFF.md) for the full phase breakdown and the current list of known
boundaries.

## Rendering TODOs

- [ ] Add a rotating Vulkan staging-buffer readback ring for asynchronous GPU screenshots and
  WebM recording. Until this lands, synchronous screenshots can capture the real Vulkan swapchain,
  but background captures and video use the deterministic CPU stream. Revisit this before relying
  on recordings to diagnose shaders, ray tracing, DLSS/FSR, lighting or driver-specific artifacts.

## Importing TODOs

- [ ] Add and verify OS-level Blender conversion sandboxes on Windows and macOS equivalent to
  Linux Bubblewrap. Until then, keep untrusted `.blend` conversion disabled on those platforms;
  `RELAY_BLENDER_TRUSTED=1` must remain an explicit administrator opt-in for trusted files only.
