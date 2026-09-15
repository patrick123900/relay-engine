# Relay Engine — Session Handoff

Last updated: 2026-09-15

## User’s product goal

Build a custom native game engine designed from the beginning for both humans and LLM-driven
agents. The engine should be pleasant for a human developer to use, while exposing nearly every
editor/runtime operation through safe, typed automation surfaces suitable for agentic workflows.

The desired end state includes:

- fast native runtime and rendering code;
- low-token, easy-to-understand code and APIs for LLMs to modify;
- a normal human editor, not an agent-only or headless system;
- built-in or tightly integrated chat/agent workflows;
- many MCP tools for creating, editing, running, inspecting and debugging games;
- deterministic frame stepping and reproducible traces;
- screenshots, real GPU captures, screen recordings, logs and performance data available to agents;
- modern rendering, eventually including ray tracing and vendor/upscaling technologies such as
  DLSS and FSR;
- straightforward engine and game builds for Windows and Linux, with macOS where possible;
- practical compatibility with the 3D model files used by Godot.

Rust was explicitly rejected. The user previously found cutting-edge graphics integration in Rust
too difficult. The selected implementation language is C++20 because it has direct access to Vulkan,
Direct3D, Metal interop and vendor SDKs while remaining portable and familiar to graphics tooling.

The working engine name is **Relay Engine**. The central product principle is:

> Anything a human can inspect or operate in the editor should also have a typed, observable and
> safe automation surface.

## Architectural direction

Keep the native engine model-agnostic. Rendering, scenes, assets, input, capture, telemetry and a
versioned control protocol belong in C++. MCP, model providers, chat history and higher-level agent
orchestration belong in an out-of-process Agent Bridge. The same native protocol should be shared by
the human editor, tests, command-line tools and MCP integrations.

Current major layers:

- `include/relay` and `src`: native C++ engine library;
- `apps/relay_demo`: headless, Vulkan demo and editor/control entry point;
- `protocol/relay.protocol.json`: single source of truth for the automation protocol;
- `tools/generate_protocol.py`: generates native C++, TypeScript/Zod MCP registrations and docs;
- `tools/mcp-bridge`: TypeScript MCP server using the official MCP SDK;
- `shaders`: GLSL compiled to SPIR-V;
- `tests/engine_tests.cpp`: native integration-style test suite;
- `assets`, `scenes`, `captures`, `traces`: project-local runtime data.

## What has been built

### Core runtime and scene model

- Portable C++20 runtime with a fixed deterministic timestep and fixed random seed.
- Pause, resume, exact frame stepping and orderly shutdown.
- Generation-checked entity handles, preventing stale-handle reuse.
- Entity names, local transforms and safe parent/child hierarchy.
- Cycle detection and recursive subtree destruction.
- Perspective camera component with one active camera.
- Mesh renderer component storing mesh and material asset IDs.
- Component reflection metadata.
- Transactional scene undo/redo with a bounded history.
- Scene format version 3 with strict validation, full-precision transforms, allocator generations,
  cameras and mesh renderers.
- Migration support for scene versions 0, 1 and 2.
- Atomic cross-platform scene saves.

### Rendering

- Dependency-free deterministic CPU renderer used as a headless test oracle.
- SDL3 human-facing window.
- Vulkan presentation backend with:
  - instance/device/surface/swapchain setup;
  - graphics and presentation queue selection;
  - two synchronized frames in flight;
  - resize and swapchain recreation;
  - scene-driven indexed drawing;
  - resolved hierarchy/model/view/projection matrices;
  - staged uploads into device-local vertex and index buffers;
  - GLSL-to-SPIR-V build integration;
  - SPIR-V reflection of vertex inputs, descriptor bindings and push constants;
  - a small compiled render graph with resources, dependencies and transitions;
  - per-swapchain-image depth attachments with depth testing and writes for opaque geometry;
  - per-mesh frustum culling and a front-to-back opaque draw order independent of creation order;
  - a bounded 16-slot texture descriptor table;
  - staged procedural texture uploads and GPU-generated mip chains;
  - synchronized swapchain readback to PNG/BMP;
  - GPU timestamp measurements.
- Built-in triangle and quad meshes.
- Built-in orange, azure and violet materials.
- Built-in checker and gradient textures.
- Vulkan capability probing reports adapter identity and support for ray-tracing pipelines, ray
  queries, mesh shaders and descriptor buffers. This is detection only; ray tracing is not rendered.

### Model import

- Assimp-backed model importer is optional at build time.
- Accepted project-file extensions are `.gltf`, `.glb`, `.fbx`, `.obj`, `.dae` and `.blend`.
- glTF 2.0/GLB is the recommended native interchange path, matching Godot’s recommendation.
- Imported static geometry includes positions, UV0, generated normals/tangents and triangle indices.
- Node transforms/hierarchy and PBR material factors are imported. Embedded and external PNG/JPEG
  images support base-color, metallic/roughness, normal, occlusion and emissive channels.
- Import can register assets only or instantiate the node hierarchy into the active scene.
- Imported mesh/material IDs are a versioned SHA-256 digest of normalized imported content, so the
  identity covers an external `.bin` payload and not just the container file.
- Reimporting identical content reuses stable IDs rather than adding duplicate assets.
- The Vulkan backend notices asset-registry revisions, waits for the device to become idle and
  safely rebuilds mesh, material, texture and descriptor resources so live imports render
  immediately.
- Hierarchy-only nodes do not render placeholder geometry; only entities with a mesh-renderer
  component are drawable.
- The importer reports deferred animations and skeletons/skin weights as warnings rather than
  pretending the import is complete.
- Tiny OBJ and embedded-buffer glTF fixtures plus a golden PBR glTF/GLB pair are covered by tests.

### Durable and safe asset foundation

- `AssetRegistry` owns every mesh, material and texture; the process-global singleton is gone.
- `Engine` owns one registry and hands a const view to render-scene construction and Vulkan drawing.
  Independent registries can coexist, so tests no longer share mutable state.
- Imported asset identity is a versioned SHA-256 digest (`sha256-v1-<hex>`), verified against the
  NIST test vectors. The scheme version is part of every generated asset name.
- A sandboxed Assimp `IOSystem` resolves every dependency read inside the project assets root.
  Relative escapes, absolute paths and symlinked escapes are refused; reads are capped at 64
  dependency files and 64 MiB each, and the importer is given no write path.
- Imports are recorded in a project manifest (`assets/.relay-imports.json`) holding the source name,
  importer version, content id, dependency list and generated asset ids.
- `scene.load` rebuilds imported assets from that manifest before applying the scene, and reports
  restored/changed/failed counts in its response. A source file that changed since the manifest was
  written is reported rather than silently re-pointed.
- Scene files, the import manifest and any future on-disk format share one strict JSON reader.

### Agent observability and debugging

- Structured bounded in-memory logs with sequence-based reads.
- Bounded performance samples containing CPU time, latest GPU time, memory, draw calls, render
  resources and entity count.
- Deterministic traces for agent commands and normalized keyboard/mouse/gamepad input.
- Trace replay checks timestep and random seed.
- Dependency-free PNG/BMP writing.
- Bounded asynchronous CPU capture worker with queryable jobs.
- Short WebM recording through FFmpeg/libvpx-vp9 with bounded queues and explicit dropped frames.
- Synchronous real-Vulkan screenshots are working.

### Control protocol and MCP

- Protocol schema version 4.
- 38 native methods and 38 generated MCP tools.
- Strict generated validation for required fields, types, ranges, patterns, enums, nullability,
  unknown fields and duplicate JSON keys.
- Generated safety annotations identify read-only, destructive and idempotent operations.
- Native newline-delimited JSON transports:
  - `--agent-stdio` for deterministic headless operation;
  - `--editor-stdio` for control of the same live Vulkan editor shown to a human;
  - `--agent-port` for an IPv4 loopback-only socket.
- Live editor requests are queued and executed on the render thread to avoid scene/GPU races.
- MCP bridge launches and owns the runtime, keeping engine messages off MCP standard output.
- Important tool groups include runtime control, scene CRUD, cameras/renderers, undo/redo,
  save/load, render capture and inspection, logs, telemetry, video, traces and model import.
- `assets.formats` reports import coverage, the active identity scheme and the import sandbox limits.
- `assets.import_model` safely accepts a top-level filename from the project `assets/` directory, and
  every dependency that model references is resolved inside the same directory.
- Every file-writing method decides its own directory natively: scenes resolve under `scenes/`,
  traces under `traces/`, video and captures under `captures/`. None of this depends on the bridge.
- `scene.load` reports an `imported_assets` summary of assets rebuilt from the import manifest.

## What was verified

At the end of the Phase A session:

- clean development and release builds passed with **zero compiler warnings** under
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`;
- the native test suite passed, now 107 assertions;
- generated protocol files regenerated with no drift, and the MCP TypeScript check passed;
- the SHA-256 implementation matched all four official NIST test vectors, including the
  one-million-character case, and produced identical digests across chunked updates;
- the import sandbox refused a `../` dependency, an absolute dependency path and a missing
  dependency, and left the asset registry untouched in each case;
- **Phase A definition of done:** importing `relay-test-triangle.gltf` and saving the scene in one
  process, then loading it in a *separate* process, reported
  `imported_assets: {restored: 1, changed: 0, failed: 0}` and resolved the entity's mesh renderer to
  the same `sha256-v1-…` id with no manual reimport;
- a capture requested as a bare filename over `--agent-stdio` landed in `captures/` rather than the
  process working directory;
- Vulkan still ran on the host’s **AMD Radeon RX 9070 XT (RADV GFX1201)**: `--vulkan-smoke` passed
  and `--vulkan-capture` produced a correct textured frame through the new registry-backed path.

From earlier sessions, still current:

- live OBJ and glTF import each caused a successful Vulkan mesh-buffer refresh and capture;
- the reference glTF capture is `captures/imported-gltf.png`;
- an earlier bindless-texture scene capture is `captures/bindless-scene.png`.

Useful commands:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev

cmake --preset release
cmake --build --preset release

cd tools/mcp-bridge
npm run check
npm run build
```

Host GPU checks require access to the real display/GPU:

```sh
./build/dev/relay_demo --probe-vulkan
./build/dev/relay_demo --vulkan-smoke
./build/dev/relay_demo --vulkan-capture captures/vulkan-frame.bmp
./build/dev/relay_demo --editor-stdio
```

## Important limitations and technical debt

These distinctions must remain explicit in future status reports.

### Import compatibility is not yet Godot feature parity

Relay recognizes the main model extensions used by Godot, but does not yet reproduce all of Godot’s
import behavior. glTF/GLB is the reliable recommended path. Godot imports `.blend` by launching
Blender and converting to glTF; Relay currently asks Assimp to load `.blend` directly. Modern Blender
files may therefore fail or differ. A Blender-to-glTF adapter is still required.

Reference: [Godot — Available 3D formats](https://docs.godotengine.org/en/latest/tutorials/assets_pipeline/importing_3d_scenes/available_formats.html).

FBX, OBJ and DAE are currently routed through Assimp. There is no ufbx-specific path matching newer
Godot FBX behavior.

### Static model data boundaries

- Vertex data contains positions, UV0, normals and tangents, but not vertex colors, joints or
  weights.
- Imported image decoding currently supports PNG and JPEG; other glTF image formats are rejected.
- Materials preserve PBR factors and base-color, metallic/roughness, normal, occlusion and emissive
  textures. Alpha modes and double-sided state are retained, but blended materials do not yet have
  a transparent draw queue and single-sided materials currently share the no-cull pipeline.
- Skeletons, skinning, morph targets and animation playback are absent.
- Cameras and lights embedded in imported scenes are not instantiated.

### Remaining asset lifecycle gaps

Phase A replaced the global registry, the FNV-1a identity and the unsandboxed importer. These parts
are still outstanding:

- Undoing a model import removes instantiated scene entities but does not unregister the imported
  assets. This is still cache-like behavior, not transactional asset lifecycle management.
- Hot import waits for the entire Vulkan device to become idle and rebuilds all combined mesh
  buffers. It is correct but stalls and will not scale. Use versioned/asynchronous uploads later.
- Texture resources participate in safe live refresh, but the current implementation waits for the
  device and rebuilds the full registry rather than updating only changed resources.
- The manifest records a content id per source file but does not store per-dependency hashes, so a
  changed dependency is detected by re-importing rather than by comparing recorded digests.
- Nothing prunes manifest entries whose source file has been deleted; they surface as failures on
  the next load instead of being retired.

### Import sandbox boundary

Every dependency read now resolves through a canonicalizing `IOSystem` rooted at `assets/`, so a
model cannot reach outside the project. Two caveats remain explicit:

- `.blend` is still handed to Assimp directly, so the sandbox does not cover a future Blender
  subprocess adapter. That adapter will need its own containment.
- Data URIs are decoded by Assimp in memory and are bounded only by the containing file's size
  limit, not by a separate decoded-size budget.

### Rendering is still an early renderer

- Depth testing and frustum culling exist, but there is still no transparent render queue, no
  batching and no instancing. Every visible instance is its own draw call.
- Culling is per-instance against the mesh's local axis-aligned bounds. There is no spatial
  acceleration structure, so the cull cost is linear in drawable entity count.
- Depth attachments are isolated per swapchain image so frames in flight cannot write the same
  depth image concurrently.
- Normals, normal maps, a direct-light metallic/roughness shader and tone mapping exist. There is no
  image-based lighting, shadowing, HDR render target or post-processing pipeline yet.
- No ray tracing, DLSS, FSR or temporal upscaling has been implemented; only hardware capability
  discovery exists.
- Texture table capacity is fixed at 16.
- The render graph describes a small geometry/presentation flow but is not yet the sole resource and
  synchronization authority for the Vulkan backend.
- The Vulkan backend is the active Windows/Linux direction. Direct3D 12 and Metal are only planned.
- The code has been exercised on Linux/RADV. Windows and macOS builds have not been verified here.

### Capture boundary to revisit

Capture paths are now contained at the native protocol boundary: `render.capture` and
`render.capture_async` route through `safe_capture_path`, which always resolves under `captures/`.
It accepts the explicit `captures/` prefix the MCP bridge sends without applying it twice, so a
native agent on `--agent-stdio` can no longer write into the process working directory. Every
file-writing protocol method now enforces its own directory natively rather than relying on the
TypeScript bridge.

Real GPU screenshots use a synchronous swapchain readback and work. Background screenshot encoding
and WebM recording still consume the deterministic CPU-renderer stream. They therefore cannot yet
diagnose shader, lighting, ray-tracing, DLSS/FSR or driver-specific GPU output. The README contains a
TODO for a rotating Vulkan staging/readback ring that feeds asynchronous captures and recordings.

### Human editor and agent product surface are early

- The “editor” is currently a Vulkan window plus protocol control, not a full scene-tree/inspector/
  asset-browser UI.
- There is no built-in chat panel yet.
- Model-provider and conversation state integration has not been built.
- There are no per-session capability grants or authentication. The socket is loopback-only, but
  mutating tools still need a formal permission model before broader exposure.
- There is no plugin/project packaging system yet.

## What to build next

The **Renderable Static glTF v1** milestone is complete through Phase C. Phase D is next.

### Phase A — durable and safe asset foundation (complete)

All seven steps landed: an `Engine`-owned `AssetRegistry`, a const registry threaded explicitly into
render-scene construction and Vulkan drawing, a project import manifest, automatic rebuild on scene
load, a versioned SHA-256 identity, a sandboxed Assimp `IOSystem`, and tests covering external
`.gltf` buffers, traversal and absolute-path escapes, duplicate import, changed dependency content,
missing dependencies and reload into a fresh registry.

Definition of done, verified: importing `relay-test-triangle.gltf`, saving the scene and loading it
in a separate process reports `restored:1, changed:0, failed:0` and resolves the entity's mesh to the
same `sha256-v1-…` id with no manual reimport. A model referencing `../` or an absolute path outside
`assets/` is refused and leaves the registry untouched.

### Phase B — correct basic 3D visibility (complete)

All five steps landed: a device-selected depth attachment with render-pass and framebuffer
integration and cleanup on swapchain recreation, depth test and write enabled with a `LESS` compare
op, an explicit `scene_depth` resource and transition in the render graph, per-mesh bounds driving
frustum culling, and a front-to-back opaque draw order with a total tiebreak so it never depends on
entity creation order.

Definition of done, verified: two overlapping meshes captured on the real GPU produced a
**byte-identical** PNG whether the near or the far entity was created first, with the near mesh
correctly occluding the far one. The capture is `captures/depth-occlusion.png`. Swapchain recreation
is exercised by `--vulkan-smoke`, which resizes mid-run and rebuilds the depth resources.

Note: the host has no `VK_LAYER_KHRONOS_validation`, so the Vulkan work here is verified by
observed output and clean runs, not by validation layers. Installing them remains worthwhile.

### Phase C — static glTF material completeness (complete)

1. Extend `MeshVertex` with normal and tangent data; update Vulkan vertex declarations and shaders.
2. Generate missing normals/tangents when appropriate.
3. Decode embedded and external glTF images using a portable dependency.
4. Import and upload base-color, metallic/roughness, normal, occlusion and emissive textures.
5. Replace the fixed color/checker material with a PBR material record including alpha mode,
   alpha cutoff and double-sided state.
6. Add sRGB versus linear texture formats and sampler settings.
7. Make texture/descriptor updates hot-reload safely; replace the initial whole-registry refresh
   with incremental/versioned uploads when scaling work begins.
8. Add golden glTF fixtures and GPU captures for textured and metallic/roughness materials.

The Phase C import and rendering path has landed. The importer generates normals/tangents, decodes
embedded and external PNG/JPEG images, retains the full static glTF PBR material record, selects
sRGB or linear Vulkan formats, applies glTF sampler settings and safely refreshes live GPU
resources. The shader renders
base-color, metallic/roughness, normal, occlusion and emissive inputs. The golden fixture is present
as both `assets/relay-pbr-golden.gltf` and `assets/relay-pbr-golden.glb`.

Definition of done, verified: the representative GLB renders its textured metallic and rough
dielectric materials on the real GPU, and a saved scene reloads the same asset in a clean editor
process with a byte-identical capture. The reference image is `captures/pbr-golden.png`. Normal-map
decoding, generated tangent space and every PBR channel are additionally covered by native tests.

The implementation is deliberately still small: textures are capped at 16, live refresh rebuilds
the full registry after waiting for the device, and alpha blending needs a transparent queue.

### Phase D — finish Godot-oriented importing

1. Add the Blender executable adapter used conceptually by Godot: detect Blender, convert `.blend`
   to cached glTF/GLB, capture diagnostics and hash the converted dependencies.
2. Decide whether to retain Assimp FBX or add ufbx for closer Godot behavior.
3. Add skeleton, skin weights, animation clips, morph targets, imported cameras and lights.
4. Add import settings/presets and reimport-on-change.

### Later milestones

- Asynchronous Vulkan screenshot/readback ring feeding image and WebM workers.
- Human editor UI: scene tree, inspector, viewport gizmos, asset browser, undo history and embedded
  agent chat.
- Explicit agent capability/permission grants and auditable tool scopes.
- Lighting, shadows, HDR and post-processing.
- Render-backend abstraction followed by Direct3D 12 and Metal implementations.
- Hardware ray tracing, followed by a denoising/upscaling abstraction and then DLSS/FSR integrations.
- Game scripting/component authoring, packaging, hot reload and cross-platform export tooling.

## Recommended prompt for the next session

Use this as the next instruction after giving the agent this handoff:

> Continue Relay Engine with Phase D from HANDOFF.md. Phases A-C are complete. Start with the
> Blender-to-glTF adapter used conceptually by Godot: detect a configured Blender executable,
> convert `.blend` files into a contained cache, capture actionable diagnostics, include conversion
> inputs in durable identity and keep every subprocess and dependency inside an explicit sandbox.
> Add import settings and reimport-on-change without weakening the existing path and symlink checks.
> Keep direct Assimp import for glTF/GLB and the current FBX/OBJ/DAE fallback. Add focused tests, then
> run dev/release, ctest, MCP validation and a real GPU smoke test.

## Key files to inspect first

- `README.md`
- `HANDOFF.md`
- `CMakeLists.txt`
- `include/relay/core/engine.hpp`
- `include/relay/core/hash.hpp`
- `include/relay/core/json.hpp`
- `include/relay/render/assets.hpp`
- `include/relay/render/asset_manifest.hpp`
- `src/render/assets.cpp`
- `src/render/asset_manifest.cpp`
- `src/render/model_import.cpp`
- `include/relay/render/scene_render.hpp`
- `src/render/scene_render.cpp`
- `include/relay/platform/vulkan_window.hpp`
- `src/platform/vulkan_window.cpp`
- `include/relay/scene/scene.hpp`
- `src/scene/scene.cpp`
- `src/scene/scene_io.cpp`
- `protocol/relay.protocol.json`
- `tools/generate_protocol.py`
- `tools/mcp-bridge/src/generated_protocol.ts`
- `tests/engine_tests.cpp`

## Guardrails for future work

- Preserve the model-agnostic native core and keep MCP/provider concerns out of it.
- Keep the versioned schema as the only source of protocol/MCP documentation and validation.
- Never hand-edit generated protocol artifacts.
- Keep all human-visible editor mutations available through typed automation where practical.
- Prefer project-relative safe filenames at the protocol boundary; validate all transitive resource
  access too.
- Preserve deterministic behavior and stable identifiers across save/load and process restarts.
- Continue verifying meaningful renderer milestones with both automated tests and real GPU captures.
- Do not describe capability detection as feature implementation.
- Do not describe accepted file extensions as full Godot import parity.
