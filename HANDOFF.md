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
- Imported static geometry currently includes positions, UV0 and triangle indices.
- Base material colors and node transforms/hierarchy are imported.
- Import can register assets only or instantiate the node hierarchy into the active scene.
- Imported mesh/material IDs are based on an FNV-1a hash of normalized imported content.
- Reimporting identical content reuses stable IDs rather than adding duplicate assets.
- The Vulkan backend notices asset-registry revisions, waits for the device to become idle and
  rebuilds the combined mesh buffers so models imported into a live editor can render immediately.
- Hierarchy-only nodes do not render placeholder geometry; only entities with a mesh-renderer
  component are drawable.
- The importer reports deferred animations, skeletons/skin weights and image/PBR textures as
  warnings rather than pretending the import is complete.
- Tiny OBJ and embedded-buffer glTF fixtures exist in `assets/` and are covered by tests.

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
- `assets.formats` reports import coverage.
- `assets.import_model` safely accepts a top-level filename from the project `assets/` directory.

## What was verified

At the end of the previous session:

- development configuration and build passed;
- the complete native test suite passed;
- release configuration and build passed;
- generated protocol files were current;
- MCP TypeScript type-check and production build passed;
- Vulkan ran on the host’s **AMD Radeon RX 9070 XT (RADV GFX1201)**;
- live OBJ import caused a successful Vulkan mesh-buffer refresh and capture;
- live glTF import caused a successful Vulkan mesh-buffer refresh and capture;
- the final glTF capture is `captures/imported-gltf.png`;
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

### Static model data is incomplete

- Vertex data contains position and UV only—no normals, tangents, vertex colors, joints or weights.
- Imported image textures are not decoded or uploaded.
- Materials only preserve base color and use a built-in checker texture.
- Metallic/roughness, normal, occlusion, emissive, alpha modes and double-sided flags are absent.
- Skeletons, skinning, morph targets and animation playback are absent.
- Cameras and lights embedded in imported scenes are not instantiated.

### Asset lifecycle needs a foundational cleanup

- The asset registry is currently a process-global mutable singleton in `src/render/assets.cpp`, not
  owned by an `Engine` or project instance.
- It is safe in the current live editor because protocol mutations run on the render thread, but it
  is not a final multi-engine or multi-threaded design.
- Imported asset IDs use FNV-1a 64-bit with the algorithm included in the ID. This is deterministic
  and adequate for the prototype, but it is not cryptographically collision-resistant. SHA-256 or
  BLAKE3 should replace it before asset IDs become durable public data.
- A scene saves imported asset IDs, but the imported registry is not persisted or reconstructed
  automatically on the next process launch. Saved scenes referencing imported meshes therefore
  require manual reimport in the current prototype.
- Undoing a model import removes instantiated scene entities but does not unregister the imported
  assets. This is currently a cache-like behavior, not transactional asset lifecycle management.
- Hot import waits for the entire Vulkan device to become idle and rebuilds all combined mesh
  buffers. It is correct but stalls and will not scale. Use versioned/asynchronous uploads later.
- Imported texture resources do not yet participate in hot reload.

### Import security boundary is incomplete

The MCP/native command accepts only a safe top-level filename under `assets/`, preventing direct
`../` paths in the command. However, a glTF/DAE/etc. file can itself reference external files.
Assimp currently uses its normal filesystem access, so an untrusted model could reference a path
outside the project. Before accepting arbitrary agent/user-supplied model files, add a custom Assimp
`IOSystem` or equivalent canonicalized virtual filesystem that restricts every dependency read to
the project asset root, with explicit policy for data URIs and file-size/count limits.

### Rendering is still a first-light renderer

- There is no depth buffer or depth testing.
- There is no opaque/transparent render queue, sorting, culling, batching or instancing.
- There are no normals, lighting, shadows, HDR, tonemapping or physically based shading.
- No ray tracing, DLSS, FSR or temporal upscaling has been implemented; only hardware capability
  discovery exists.
- Texture table capacity is fixed at 16.
- The render graph describes a small geometry/presentation flow but is not yet the sole resource and
  synchronization authority for the Vulkan backend.
- The Vulkan backend is the active Windows/Linux direction. Direct3D 12 and Metal are only planned.
- The code has been exercised on Linux/RADV. Windows and macOS builds have not been verified here.

### Capture boundary to revisit

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

The recommended next milestone is **Renderable Static glTF v1**, but begin with the asset-foundation
work below so the new material/texture system does not deepen prototype shortcuts.

### Phase A — durable and safe asset foundation

1. Replace the global registry with an `AssetRegistry` owned by `Engine` or a `Project` object.
2. Pass a const registry/view explicitly into render-scene construction and Vulkan drawing.
3. Add a project asset manifest/import cache that records source filename, importer version,
   dependency list, content hash and generated asset IDs.
4. On scene/project load, rebuild or reload imported assets automatically so saved imported IDs are
   immediately valid.
5. Replace FNV-1a identities with SHA-256 or BLAKE3 and version the identity scheme.
6. Add a sandboxed Assimp `IOSystem`/virtual filesystem enforcing canonical paths under `assets/`,
   bounded dependency count, bounded file sizes and controlled data URIs.
7. Add tests for external `.gltf` buffers, path traversal inside a model, duplicate import, changed
   dependency content, missing dependencies and scene reload across a fresh engine instance.

Definition of done: importing a project-local `.gltf` or `.glb`, saving the scene, restarting Relay
and loading the scene produces the same valid registered asset IDs without manual reimport; referenced
files cannot escape the asset root.

### Phase B — correct basic 3D visibility

1. Add Vulkan depth images per swapchain image, depth attachment/render-pass integration and cleanup.
2. Enable depth testing/writes for opaque geometry.
3. Extend the render graph to represent depth explicitly.
4. Add frustum culling and a deterministic opaque draw order.
5. Add overlapping/occluded mesh tests and a real-GPU screenshot proving depth correctness.

Definition of done: two overlapping imported meshes render correctly regardless of creation order,
and swapchain recreation safely rebuilds depth resources.

### Phase C — static glTF material completeness

1. Extend `MeshVertex` with normal and tangent data; update Vulkan vertex declarations and shaders.
2. Generate missing normals/tangents when appropriate.
3. Decode embedded and external glTF images using a portable dependency.
4. Import and upload base-color, metallic/roughness, normal, occlusion and emissive textures.
5. Replace the fixed color/checker material with a PBR material record including alpha mode,
   alpha cutoff and double-sided state.
6. Add sRGB versus linear texture formats and sampler settings.
7. Make texture/descriptor updates hot-reload safely rather than rebuilding globally.
8. Add golden glTF fixtures and GPU captures for textured and metallic/roughness materials.

Definition of done: a representative static Godot-exported GLB renders with correct geometry, UVs,
base color, normal mapping and metallic/roughness response after a clean restart.

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

> Continue Relay Engine with Phase A of the “Renderable Static glTF v1” milestone from HANDOFF.md.
> First inspect the existing asset/import/render interfaces and preserve all working behavior. Replace
> the process-global asset storage with an engine/project-owned registry, add durable import metadata
> sufficient for scene reload, and sandbox all importer dependency reads to the project assets root.
> Use a versioned SHA-256 or BLAKE3 identity. Add focused tests, regenerate the protocol artifacts if
> its surface changes, run dev/release and MCP validation, and clearly report any remaining boundary.

## Key files to inspect first

- `README.md`
- `HANDOFF.md`
- `CMakeLists.txt`
- `include/relay/core/engine.hpp`
- `include/relay/render/assets.hpp`
- `src/render/assets.cpp`
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
