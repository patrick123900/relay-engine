# Relay Engine — Session Handoff

Last updated: 2026-09-18

## Desktop interaction rule — read first

Follow [AGENTS.md](AGENTS.md).
Never control the user's mouse or keyboard, change focus, or open/show/activate/move/resize
windows without specific advance approval for those actions in the current task. Work in the
background using builds, headless tests and protocol checks. Desktop and windowed GPU tests are
optional, approval-only checks; record them as pending when approval has not been given.
Historical test results, reproduction commands and saved permissions do not authorize desktop use.
Agents must first run relevant background/headless builds and tests autonomously. Do not skip
these checks or default to asking for desktop access. Only ask after completing background checks
if an important verification gap cannot reasonably be covered without desktop interaction.

## Next-session starting point

- Branch: `main`; GitHub: https://github.com/patrick123900/relay-engine (public).
- Previous published baseline: `bf9a566` (`Keep repository documentation and agent policies professional`).
  The commit containing this handoff includes the usability, project-folder, timeline, icon and
  grid follow-ups. Use `git log -1` for the latest commit and inspect the working tree before editing.
- Phases A–D meet their recorded milestone definitions on the tested Linux/Vulkan path, not full
  Godot compatibility or Windows/macOS parity. Deferred work is listed explicitly below.
- Current on-disk/API versions: scene v4, import manifest v3, project v1, protocol v14 with 85 native
  methods and 72 generated MCP tools.
- Phase E and the tested Linux/Vulkan Phase F milestone are implemented. Earlier commit/push
  authorization was completed for `4c3d16e`; the user also authorized committing and pushing all
  subsequent work, including this documentation, on 2026-09-17.
  Later phase labels organize proposed work; they do not claim full editor completion or platform parity.
- The Phase E and Phase F sections below record this session; earlier phase results remain
  historical. Phase G permissions and embedded chat are implemented on this branch, but
  Phase G remains unfinished: long-running provider reliability and final UI/visual checks need work.

### Latest state and first follow-up

**Phase G is in progress, not complete.** Current chat uses OpenAI device authentication, discovered
models/reasoning and external Codex App Server dynamic tools. `relay_demo --editor` automatically
starts the bridge; standalone attachable MCP service is a separate integration. Keep the stable
`features.code_mode_host` enabled or registered tools cannot reach inference. Credentials, logs
and preferences stay in ignored `.relay/`, outside the C++ engine.

Current Agent UI has Chat/Account/Access/Activity tabs and no workspace heading, extra
New chat/Account/Expand header buttons, or per-message Copy buttons (user or agent). The composer
soft-wraps without changing submitted text. Enter sends; Ctrl+Enter adds a newline. A single
right-aligned icon sends or stops; a compact neutral model/reasoning label with a drawn chevron
opens the shared model menu and supported-level reasoning slider, without a description below.
Follow is automatic at the transcript bottom; scrolling up pauses it and shows a jump-down arrow.
Standard/narrow headless layout checks verify zero outer composer scrolling.

First follow-up: investigate intermittent service disconnections after several minutes of agent
work. This symptom is reported; the cause and a reproducible trigger are not established. Use
sanitized transport/process lifecycle diagnostics and sustained background fixtures to distinguish
service exit, RPC/stream failures and bridge shutdown/cancellation. Protect completed native actions,
avoid duplicate mutations on recovery, and keep credentials/conversation data out of tracked logs.
Do not label the adapter reliable or Phase G complete until the issue is reproduced and corrected.



The three usability priorities are implemented: multi-selection/session clipboard, folder projects,
and a dockable playback timeline. Project, Timeline and History start hidden; View reopens them.
Projects use workspace-relative `*.relayproject` paths. Their containing folder supplies recursive
Assets discovery and model imports, with scene files in its `scenes/` directory. Default Timeline
shares the wider bottom dock with Diagnostics. Duplicate panel headings are removed. Loop, Rotate
and Focus icons were redrawn after feedback; those final icon revisions have background verification
but were not reinspected on the desktop.

**Grid fading verified by the user on 2026-09-17.** After the final correction, the user confirmed
smooth fading during both upward and downward camera movement. The reported popping is resolved
in that manual check; no further grid verification is pending. The final implementation retains
GLSL `smoothstep(0.5, 1.0, fract(height))` in `shaders/editor_grid.vert`, changing only absolute height
selection and signed plane heights. The native `editor_grid_levels` helper and extra push-constant
fields were removed. This visual result comes from the user's verification, not the headless suites.

Run background verification first. Prefer an isolated offscreen/virtual-display GPU check if
practical; it must not affect the user's desktop, mouse or focus. A regular desktop check requires
fresh explicit approval. The earlier approval covered only the panel/folder review and does not
carry forward. Phase G local grants, host approval, authentication and external-bridge chat are
implemented with background verification. Treat the local-workflow completion records below as
historical implementation checkpoints, not completion of Phase G. Live tool use has been verified
in a temporary scene and confirmed working in editor testing. Intermittent ChatGPT/App Server
disconnections after several minutes of agent work remain unresolved. Continue Phase G reliability
and interface refinement before moving to Phase H; newer camera/image/composer desktop checks
and a live image-based model review remain pending.

Final recorded verification after the shader correction:

- The user confirmed smooth rendered grid fading in upward and downward motion after the final fix.
- Development and release builds passed, including grid shader compilation.
- All three test suites passed in each build: `relay_tests`, `relay_workflow_tests`, and
  `relay_editor_headless_tests`. These do **not** verify rendered grid crossfading.
- Generated protocol checks, MCP build/check and the 66-tool headless capture/clipboard smoke
  passed during the usability work. `git diff --check` passed after the correction.
- The approved desktop panel/folder review verified default hidden panes, removed headings, nested
  glTF import and the wider timeline. Its temporary editor and fixture folder were closed/removed.
- `projects/my-project/testing.relayproject` is the user-created empty test project included in
  this commit; preserve it unless the user requests otherwise.

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
- Scene format version 4 with strict validation, full-precision transforms, allocator generations,
  cameras, mesh renderers, animators, model-node bindings, morph overrides and lights.
- Migration support for scene versions 0, 1, 2 and 3.
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
- `.blend` sources use Blender's glTF exporter with script auto-execution disabled, bounded
  diagnostics, a timeout and a content-addressed cache below `assets/.relay-cache/blender`.
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
- Phase D adds engine-owned skeletons, skin/morph deformation, animation playback and imported cameras/lights.
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
  importer version, preset, content id, dependency list and generated asset ids.
- `scene.load` rebuilds imported assets from that manifest before applying the scene, and reports
  restored/changed/failed/rebound counts in its response. A source file that changed since the
  manifest was written is rebuilt and compatible saved mesh/material ids are rebound by index.
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

- Protocol schema version 8.
- 49 native methods and 49 generated MCP tools.
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
import behavior. glTF/GLB is the reliable recommended path. Like Godot, Relay imports `.blend` by
launching Blender and converting to a cached GLB. Relay's exporter settings are intentionally small
and do not yet mirror Godot's complete import-remap UI.

Reference: [Godot — Available 3D formats](https://docs.godotengine.org/en/latest/tutorials/assets_pipeline/importing_3d_scenes/available_formats.html).

FBX, OBJ and DAE are currently routed through Assimp. There is no ufbx-specific path matching newer
Godot FBX behavior.

### Static model data boundaries

- Vertex data contains positions, UV0, normals and tangents; skin influences/inverse binds and morph
  deltas live alongside the mesh rather than in the fixed GPU vertex layout. Vertex colors remain unsupported.
- Imported image decoding currently supports PNG and JPEG; other glTF image formats are rejected.
- Materials preserve PBR factors and base-color, metallic/roughness, normal, occlusion and emissive
  textures. Alpha modes and double-sided state are retained, but blended materials do not yet have
  a transparent draw queue and single-sided materials currently share the no-cull pipeline.
- CPU skin/morph deformation and deterministic per-instance animation playback are implemented.
- Perspective/orthographic cameras and directional/point/spot lights are instantiated and persisted.

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

Every dependency Assimp reads resolves through a canonicalizing `IOSystem` rooted at `assets/`, so
that stage cannot reach outside the project. Two caveats remain explicit:

- Blender conversion uses Bubblewrap on Linux with isolated filesystem/network/environment,
  read-only system runtimes/assets, private temporary storage and the conversion cache as its only
  persistent writable mount. Auto-execution is disabled and process-group cleanup enforces a
  120-second timeout. Other
  platforms fail closed unless an administrator opts into `RELAY_BLENDER_TRUSTED=1`; that mode must
  only be used for trusted files and is never an agent-controlled protocol field.
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

Phase E now feeds real Vulkan pixels into background images and WebM. Two fence-tracked slots
retain submitted extent/format across resize, with 64 MiB per-slot and eight active image jobs.
Explicit Vulkan requests fail without a live renderer; status reports the source. Recordings keep
the first sampled resolution and report differently sized frames as drops. Start a new recording
after resize to use its resolution. `video.stop` starts background finalization; completion/errors
are observable through `video.status`. Cancellation retains GPU storage until fence completion.

### Human editor and agent product surface are early

- The editor covers scene tree, inspector, viewport camera, gizmos, asset browser and undo history,
  verified with driven mouse and keyboard input. See Phase F.
- A dockable Agent panel supports external-bridge chat, explicit access review and action inspection.
- The initial text/function-call provider adapter is OpenAI-compatible. Canonical conversation state
  and provider credentials stay in the TypeScript bridge; live-provider verification is pending.
- Native project-bound grants, supported entity/file scopes, host approval and bounded audits with
  snapshot export are implemented. Loopback control requires a host-provisioned bearer token; each
  local client shares the configured listener policy. Multi-user accounts, simultaneous clients and
  continuous/tamper-proof audit retention are outside this local milestone.
- There is no plugin/project packaging system yet.

## What to build next

The **Renderable Static glTF v1** milestone and Phase D importing milestone are complete.

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
resources. The shader renders base-color, metallic/roughness, normal, occlusion and emissive inputs.
The golden fixture is present
as both `assets/relay-pbr-golden.gltf` and `assets/relay-pbr-golden.glb`.

Definition of done, verified: the representative GLB renders its textured metallic and rough
dielectric materials on the real GPU, and a saved scene reloads the same asset in a clean editor
process with a byte-identical capture. The reference image is `captures/pbr-golden.png`. Normal-map
decoding, generated tangent space and every PBR channel are additionally covered by native tests.

The implementation is deliberately still small: textures are capped at 16, live refresh rebuilds
the full registry after waiting for the device, and alpha blending needs a transparent queue.

### Phase D — finish Godot-oriented importing (complete)

1. Add the Blender executable adapter used conceptually by Godot: detect Blender, convert `.blend`
   to cached glTF/GLB, capture diagnostics and hash the converted dependencies.
2. Decide whether to retain Assimp FBX or add ufbx for closer Godot behavior.
3. Add skeleton, skin weights, animation clips, morph targets, imported cameras and lights.
4. Add import settings/presets and reimport-on-change.

The Phase D foundation is complete. `.blend` no longer goes directly through Assimp: Relay detects
Blender (or `RELAY_BLENDER_EXECUTABLE`), launches it without a shell, disables embedded-script
auto-execution, bounds diagnostics and execution time, and preflights external image/library/font/
cache-file dependencies. Each path must remain inside `assets/`. The validated GLB cache key covers
source and dependency bytes, Blender version, adapter version and import preset. Linux conversion
uses Bubblewrap to isolate filesystem, network and environment access. Cache paths are canonicalized
inside `assets/` and reject symlink escapes before creating directories. The `scene` and
`static_mesh` presets are exposed through the generated protocol. Changed imports are rebuilt during
`scene.load`, with compatible saved mesh/material references rebound by index. Asset groups whose
counts changed and conflicting source mappings are left untouched with diagnostics. The manifest
baseline is not overwritten during automatic reload, so older saved scenes remain remappable.
Index-based rebinding assumes stable mesh/material ordering; structural reordering still requires
explicit reimport rather than treating the same asset count as semantic identity.

Verified on Blender 5.2.1 LTS: a fresh sandboxed conversion imported successfully, reused its cache
on the next import and rendered in the live Vulkan editor on the RX 9070 XT. Native tests cover
dependency cache invalidation, corrupted-cache regeneration, external-path and cache-symlink
escapes, conversion timeout cleanup and changed/ambiguous scene rebinding.

FBX remains on Assimp for now. Adding ufbx would add another dependency and parallel material/
animation normalization path; revisit that decision when FBX-specific fixtures demonstrate a
correctness gap rather than introducing it speculatively.

Perspective camera importing is implemented: camera-local position/orientation is attached beneath
the matching imported node, projection is converted to Relay's vertical FOV, and cameras start
inactive. Assimp's glTF importer supplies a full horizontal angle despite its camera API's documented
half-angle convention; Relay normalizes glTF/GLB cameras before conversion. Camera projection and
orientation are validated, camera counts are capped at 4096, and camera data participates in asset
identity. Existing scene serialization and camera control work unchanged. The camera golden fixture
tests aspect-sensitive FOV, hierarchy, activation, save/load and static-mesh exclusion. Orthographic
and camera-only imports now work too, with correct Vulkan projection/depth conventions.

The remaining Phase D features are implemented:

- Model assets own node hierarchy/rest transforms and node/morph clips. Meshes own joint-to-node
  bindings, inverse bind matrices, normalized influences and position/normal/tangent morph deltas.
- Animator state lives on the imported model root; fixed steps advance time, pause freezes it,
  looping/reverse speed and terminal nonlooping poses work independently across instances.
- Native glTF/GLB animation accessor decoding retains LINEAR, STEP and CUBICSPLINE curves, including
  sparse accessors and cubic tangents/quaternion normalization, which Assimp otherwise discards.
- Valid implicit-zero glTF accessors are materialized in bounded memory before Assimp parsing,
  fixing Blender's zero-filled morph-normal exports without disabling morph normals.
- CPU morph-before-skin deformation updates bounds before culling and streams instance-specific
  vertices into fence-protected per-frame Vulkan buffers. Normal palettes are computed per joint.
- Cameras include orthographic projection. Directional/point/spot lights preserve attenuation,
  color, cones and optional range and drive the initial Vulkan PBR lighting path.
- Scene format v4 persists animator/model-node/light components, morph overrides and orthographic
  height; older scenes migrate. Protocol v5 adds animation, morph and light control.
- Manifest v3 records binding-layout and clip-order metadata. Compatible changed sources rebind
  renderer and animator IDs; changed hierarchy/rest/bind/morph layouts or clip ordering are diagnosed
  without silently retargeting saved playback. Old manifests remain readable but lack dynamic remap metadata.
- Limits: 8192 nodes, hierarchy depth 256, 4096 meshes, 1M vertices and 256 clips/1M keys per import,
  256 joints per mesh, eight influences per vertex, 64 morph targets and 64 MiB morph payload;
  animation accessor samples/buffers are bounded. Vulkan permits 4M deformed vertices per frame and
  initially renders 16 lights. Oversized deformation fails explicitly rather than growing indefinitely.

Golden fixtures cover LINEAR/STEP/cubic poses, weighted skinning, morph overrides, quaternion
interpolation, pause/step/stop, independent instances, persistence and compatible/incompatible reloads.
They also cover unnamed joints, optional identity inverse-bind matrices, static-preset exclusion and
native animation accessor count/view bounds. Dev/release builds, native tests, generated protocol
validation and MCP type-check/build passed. Real Vulkan playback/resize/capture passed for glTF,
FBX and sandboxed `.blend` on the Radeon RX 9070 XT, as did the built-in Vulkan resize regression.
Real Blender 5.2.1 LTS GLB/FBX round trips preserve clips, skins and morphs; Assimp's FBX
`node*local-mesh-index` morph convention is normalized. FBX therefore stays on Assimp, not ufbx.
`tools/generate_phase_d_fixtures.py` reproduces the Blender/GLB/FBX fixtures.
`relay_demo --vulkan-model-smoke [filename]` exercises playback, resize and GPU capture.

Platform/security boundaries remain: Windows/macOS do not yet have an OS sandbox equivalent to
Linux Bubblewrap, and refuse untrusted `.blend` conversion without the administrator trusted-input
opt-in. No Windows/macOS runtime verification is claimed. Crossfade/retargeting, vertex-cache
animation, area lights, transparent sorting and GPU skinning are later features, not this import milestone.
Meshes shared by nodes with different glTF skins must currently be separated in the source model;
the importer rejects incompatible sharing rather than silently applying the wrong skeleton.

## Pending work carried forward from phases A–D

These are deferred limitations and verification gaps, not a claim that the recorded A–D definitions
of done failed. Keep them open until implemented and tested; do not silently equate milestone
completion with full platform, material or importer parity.

### Phase A — asset lifecycle and safety follow-up

- [ ] Make import undo/redo and asset ownership transactional, or explicitly define cache retention
  and implement safe reclamation without invalidating scene/history references.
- [ ] Record per-dependency hashes for change detection rather than requiring a full reimport to
  discover dependency changes.
- [ ] Define safe retirement/pruning of deleted-source manifest entries; preserve diagnostics and
  avoid silently breaking saved scenes.
- [ ] Add a separate decoded-size budget for data URIs, beyond the containing-file limit.

### Phase B — renderer validation and scaling follow-up

- [ ] Install/use Vulkan validation layers and repeat depth, resize and frames-in-flight regressions.
  Earlier GPU results were observed without `VK_LAYER_KHRONOS_validation` on this host.
- [ ] Add spatial acceleration, batching and instancing when scene-scale measurements justify them;
  current culling is linear and each instance has its own draw call.
- [ ] Make the render graph the backend's resource/synchronization authority rather than only an
  inspectable description of part of the rendering flow.

### Phase C — material correctness and resource lifecycle follow-up

- [ ] Implement a transparent draw queue/sorting for retained alpha-blend materials.
- [ ] Honor single-sided versus double-sided material state with appropriate culling/pipelines.
- [ ] Replace whole-registry device-idle refresh with incremental/versioned asynchronous mesh,
  material and texture uploads with safe retirement of resources still in flight.
- [ ] Remove or make configurable the fixed 16-texture capacity when extending material coverage.
- [ ] Add vertex-color support and additional image formats only with explicit supported-format
  reporting and fixtures; PNG/JPEG remain the implemented image formats.

### Phase D — platform conversion and importer follow-up

- [ ] Add and verify Windows/macOS OS-level Blender conversion sandboxes equivalent to Linux
  Bubblewrap. This is also tracked in the README's Importing TODOs. Until then, untrusted `.blend`
  conversion must fail closed; `RELAY_BLENDER_TRUSTED=1` is administrator-only trusted-input opt-in.
- [ ] Run Windows/macOS build and import/cache/timeout/security regressions; no runtime verification
  on those platforms is currently claimed.
- [ ] Support a glTF mesh shared by nodes with different skins, if required; currently reject it
  safely and ask authors to separate the source meshes.
- [ ] Improve structural reimport mapping when needed. Stable counts alone are not semantic identity;
  incompatible hierarchy/rest/bind/morph layouts and clip-order changes must remain diagnosed.
- [ ] Expand editable import presets beyond `scene`/`static_mesh` as the corresponding editor
  features land. Reimport-on-change currently occurs on scene load, not via a live file watcher.

FBX stays on Assimp: the existing FBX round-trip fixtures passed, so adding ufbx is a conditional
future decision, not an unfinished mandatory Phase D step. Crossfade/retargeting, vertex-cache
animation, area lights and GPU skinning are later enhancements, not missing Phase D deliverables.

## Next phases — proposed implementation order

### Phase E — asynchronous real-GPU capture and recording (implemented; Linux verification below)

- Add a bounded rotating Vulkan staging/readback ring, fence-based completion and safe handling
  of swapchain resize/recreation; do not wait for device idle for every background capture.
- Feed completed GPU frames into the existing image/WebM workers with bounded queues, explicit
  drops/failures and observable job status. Retain the deterministic CPU source for headless tests.
- Expose source selection and capture provenance through the single generated protocol schema;
  never label CPU output as a GPU capture.

Definition of done: asynchronous PNG and WebM show the actual Vulkan output, including animated
models and lighting, while playback/resizing remains responsive; tests cover limits, cancellation,
shutdown and failure paths, and real GPU output is verified.

Phase E verification (2026-09-16):

- Dev/release builds and native tests, generated protocol consistency, MCP check/build and the
  real MCP headless capture smoke pass. Native tests cover bounded reservations, failed readback,
  cancellation with late completion, shutdown draining and asynchronous video success/failure.
- Real Radeon RX 9070 XT/RADV playback, resize and asynchronous PNG/WebM smoke passes for
  glTF, GLB, Assimp FBX and sandboxed Blender golden inputs, plus the built-in Vulkan resize test.
- Generated PNG and a decoded VP9 frame were inspected visually. The recording retains 640×360;
  the screenshot after resize is 960×540. Resize drops are explicitly reported. The smoke writes
  `/tmp/relay-phase-e.png` and `/tmp/relay-phase-e.webm`.
- Protocol v6 adds source selection, pending-capture cancellation and video finalization status;
  42 native methods/MCP tools. Scene v4, manifest v3, importer normalization/bounds, golden assets,
  compatible reload and fail-closed Blender sandbox policies remain unchanged.
- Final verification against the combined Phase E/F tree also passes: protocol v7/47 tools,
  debug native suite, a separate Release build with `RELAY_BUILD_TESTS=ON`, generated/MCP checks
  and capture smoke, and another real Vulkan asynchronous playback/resize/capture run.
- This remains a bounded short-recording implementation: PNG intermediates feed FFmpeg; streaming
  raw frames directly to an encoder and recording across resolution changes are future work.
  No Windows/macOS runtime verification or OS sandbox parity is claimed.

### Phase F — usable human editor (complete)

- Build a scene tree/selection model, reflected component inspector, viewport camera/gizmos,
  asset browser/import controls and undo history.
- Add animation, morph and light editing, scene save/load and visible diagnostics.
- Route editor mutations through the same native operations as automation; keep user input and
  agent edits consistent and preserve deterministic stepping.

Definition of done: a human can import, arrange, animate, save/reopen and inspect a small scene
  without sending protocol commands, and equivalent agent operations produce the same state.

Implemented 2026-09-16. The editor is a Dear ImGui interface on the live Vulkan window, with
`--editor` for the interface alone and `--editor-ui-stdio` for the interface and the agent transport
against one runtime. `--editor-stdio` is unchanged.

Architecture:

- `EditorUi` holds no reference to `Scene`, `SceneHistory` or `AssetRegistry`. Every panel issues the
  same newline-delimited JSON requests agents send, through the same `ControlProtocol` instance, so
  human and agent edits are one native operation, one undo history and one trace. There is no second
  mutation path to keep in sync.
- `EditorOverlay` is a narrow abstract seam in the window. Dear ImGui and ImGuizmo are linked only
  into `relay_demo`; the engine library keeps no UI dependency. Both are checksum-pinned, ImGui to
  release v1.92.1-docking and ImGuizmo to an exact commit.
- The UI is recorded into the swapchain render pass only when no capture buffer is bound, so
  captures, readbacks and golden images keep showing scene pixels.

Panels and interaction:

- Hierarchy tree with selection, drag-and-drop reparenting, renaming and a context menu.
- Inspector for transform, camera, mesh renderer, animator, morph weights and lights, including
  adding lights, light type, cone angles, range and attenuation.
- Toolbar with pause/resume, single-frame step, undo/redo, scene save/load, view source toggle,
  focus and gizmo mode.
- Asset browser listing importable models from `assets/`, with preset selection; importing selects
  and frames the new root.
- Navigable undo history list and a diagnostics panel with engine logs and last-operation status.
- Orbit/pan/dolly viewport camera, click-to-select, translate/rotate/scale gizmos, and `W`/`E`/`R`,
  `F`, `Delete` and `Ctrl+Z` shortcuts that are suppressed while a text field has focus.

Design decisions worth preserving:

- The editor camera is view state, not scene state. It creates no entity, is never saved, never
  enters the undo history and is never sent over the protocol, so navigating at mouse rate produces
  no trace entries. It does apply to captures, because a screenshot should show what the operator
  sees; the toolbar switches back to the scene camera.
- Picking is stateless. `scene.pick` takes a world-space ray rather than reading a stored viewpoint,
  so the engine holds no camera for the editor and an agent can pick with its own ray. `scene.bounds`
  reports world-space extents for framing and locating.
- The gizmo composes and decomposes transforms with Relay's own Euler order rather than the gizmo
  library's. A native test asserts that composition matches the matrix `build_render_scene` produces,
  so a convention drift cannot silently write back rotations that differ from what is drawn.
- `SceneHistory::execute` gained a gesture token, surfaced as `gesture` on `scene.set_transform`.
  Updates sharing a nonzero token fold into the transaction they extend, so one drag stays one undo
  entry instead of filling the bounded history. The token matters: an earlier edit of the same
  entity carries the same label, and matching on the label alone absorbed a new drag into that
  unrelated transaction. Driven-input testing caught this; the native tests now cover it.
- Read-only protocol methods are no longer recorded in traces. The editor polls twice a second, and
  tracing that would make trace files grow with idle time and bury the requests that changed state.
- `WorldResolver` in `scene_render.cpp` is now shared by rendering, picking and bounds, so the three
  cannot disagree about where an entity is.

Protocol v7 adds `scene.pick`, `scene.bounds`, `scene.rename`, `scene.history`, `assets.available`
and the `gesture` parameter: 47 native methods and 47 generated MCP tools. Scene v4 and
manifest v3 are unchanged.

Two pre-existing defects were found and fixed while verifying, both unrelated to the editor:

- `upload_buffer` and the texture upload called `vkResetFences` on `frame_fences[current_frame]`
  during `initialize()`, before `create_sync_objects()` had created them. A null fence handle
  segfaulted inside RADV, so `--vulkan-smoke` crashed on startup. Neither submit uses a fence — both
  synchronize with `vkQueueWaitIdle` — so both calls were removed.
- The live editor never exited on `runtime.quit` while a caller held its standard input open, because
  the detached stdin reader stayed blocked in `getline`. The session now runs in an inner scope and
  the process leaves deterministically once the window, engine and editor are destroyed. This
  affected `--editor-stdio` too; it had not surfaced because the MCP bridge closes stdin.

Verified on this host:

- Clean dev and release builds with zero warnings, native tests pass, no generated protocol drift,
  MCP `npm run check`/`build` pass and the MCP headless capture smoke reports 47 tools.
- Native tests cover the editor contract: the exact requests each panel issues, one undo entry per
  transform gesture, coalesced drags rewinding as a whole, full-precision values, camera activation,
  combined mesh/material assignment, reparenting to root, rename, history labels, asset listing
  filtering, pick hit/miss/degenerate-ray cases, bounds, and a destroyed selection failing
  `scene.inspect`. The gizmo's transform composition is asserted against the renderer's own matrix.
- Real Radeon RX 9070 XT/RADV: `--vulkan-smoke`, `--vulkan-model-smoke`, `--vulkan-async-smoke` and
  `--vulkan-capture` pass. The editor was driven end to end through `--editor-ui-stdio`.
- Visual evidence is `captures/phase-f-editor.png`. A `render.capture` taken while the interface was
  on screen contained no UI and matched the editor viewpoint.

Interaction verified with real input (`tests/editor_interaction_smoke.py`). The harness drives the
editor with xdotool and asserts engine state over the protocol channel rather than reading
screenshots. All of the following pass on this host:

- The toolbar Pause button pauses the runtime, which also calibrates the coordinate path.
- Double-clicking a model in the asset browser imports it and selects and frames its root.
- Clicking a hierarchy row selects it, proven by `Delete` destroying exactly that entity.
- Dragging the translate gizmo moves the entity and produces exactly one undo entry; `Ctrl+Z`
  rewinds the whole gesture rather than its last update. Rotate and scale modes render.
- Dragging one hierarchy row onto another reparents it.
- Clicking an object in the viewport selects it through `scene.pick`.
- Orbiting the camera adds nothing to the undo history and changes no entity.
- Three seconds of editor polling records zero trace events.

Two problems this driven testing exposed, both now fixed:

- Gesture coalescing matched on the transaction label, so a gizmo drag folded into an earlier,
  unrelated edit of the same entity instead of starting its own undo entry. Replaced with an
  explicit per-drag token.
- `Escape` closed the editor window instantly, discarding unsaved work on a key people press to
  dismiss menus. Escape now only quits the bare demo window; the editor quits by closing the window.

Notes for anyone re-running the harness: this desktop gates synthetic input behind a Remote Control
permission prompt that must be granted once, and the compositor scales pointer coordinates, so the
harness drives the pointer in a closed loop until it reports the intended position.

Remaining interaction gaps:

- Renaming through the context menu, the animator panel, and scene save/load are exercised
  through the protocol but not yet through driven clicks.
- Gizmo editing of an imported node driven by animation edits the stored transform, not the animated
  pose layered on top of it. The parent chain is composed from the cached scene listing, which does
  not include animation.

Editor audit and hardening (2026-09-16):

- The initial completion checks missed six human-facing defects: object-form transform vectors
  were decoded as arrays, the renderer used the whole window while picking/gizmos used the center
  panel, scalar drafts reset before release, lights decoded the wrong type/color layout, imported
  morphs required an existing override, and transform buffers stayed stale after agent edits/undo.
  These are corrected; inspector commits preserve all untouched channels.
- Shared editor vector/draft, viewport and matrix/ray helpers remove duplicated conventions.
  Normalized viewport coordinates account for logical vs framebuffer sizes. Picking/framing reuse
  rendered skin/morph evaluation, with an explicit four-million-vertex deformation query budget.
- Hierarchy lookup is indexed; asset snapshots no longer refresh on every gizmo update. History
  labels are computed once per query; disappearing asset files fail without filesystem exceptions.
  Gesture coalescing requires both token and transaction label, and mirrored matrix decomposition
  preserves handedness. Editor-view mouse input is consumed before gameplay input/trace recording.
- Strict warnings now apply to the demo/editor and native tests as well as the engine. Native
  tests cover protocol vector shapes, draft release, scaled viewport layout, off-center picking
  rays, mirrored transforms and dynamically deformed bounds.
- `tests/editor_regression_smoke.py` passed real mouse/keyboard tests for non-default transforms,
  spot-light intensity/type/color preservation, agent changes followed by a human edit and undo,
  central viewport selection/gizmo alignment, camera field-of-view edits preserving other fields,
  and a first morph override on the imported dynamic golden fixture.
- Final dev and Release builds/tests pass with zero warnings; generated protocol consistency,
  MCP check/build and the 47-tool headless capture smoke pass. The broader interaction harness
  passes, including actual orbit input while tracing (zero events). The final regression harness
  also captures the live GPU view: `captures/phase-f-audit-editor.png` shows the interface and
  `captures/phase-f-audit-vulkan.png` contains matching scene pixels without interface/gizmo pixels.
- Phase E remains verified on the final editor tree: built-in Vulkan presentation/resize and
  asynchronous golden glTF/GLB/Assimp FBX/sandboxed Blender playback/resize/PNG/WebM all pass on
  RX 9070 XT/RADV. Each recording reports `source=vulkan`, 31 submitted frames and 30 explicit
  resolution-change drops. VP9 is 640×360; the resized screenshot is 960×540. Deterministic CPU
  provenance and unsupported GPU requests failing closed remain covered by the MCP smoke.

Editor navigation and gizmo follow-up (2026-09-16):

- Perspective navigation now follows Godot defaults: MMB orbit, Shift+MMB pan, RMB or Shift+F
  cursor-captured freelook, WASD/QE flight, Shift/Alt speed modifiers and wheel fly-speed adjustment.
  Release RMB, toggle Shift+F or press Escape to exit; focus loss/destruction releases the pointer.
  Authoring-view keyboard/mouse input is kept out of gameplay traces; scene-camera mode forwards
  gameplay input outside panels. Gizmo shortcuts are suppressed during navigation.
- Fixed a one-frame mismatch: camera input/shortcuts are resolved before both gizmo drawing and
  scene rendering. Selection uses the same final camera snapshot.
- Gizmos manipulate a world matrix with a rigid view and convert edits back through the affine
  parent inverse. Parent scale is no longer folded into camera matrices. Drawing is clipped to
  the central viewport, sizing is explicit, and handles at/behind the near plane are hidden.
  Singular parents cannot generate invalid edits; each operation preserves untouched channels.
- Framing distinguishes renderable bounds from positional light/camera/empty nodes. Positional
  nodes use a three-unit standoff; tiny meshes have a minimum distance and normal geometry is fit
  to both horizontal/vertical field of view. The Sun Light rotation gizmo no longer fills the view.
- Native regressions cover fixed-eye freelook, orbit pivots, point/tiny bounds, fly-speed changes
  and scaled/mirrored/singular parent conversion. Desktop evidence is
  `captures/phase-f-light-focus.png` and `captures/phase-f-light-refocus.png`; the new
  `tests/editor_navigation_smoke.py` verifies view changes, unchanged scene/history, zero navigation
  trace events (including Shift+F/Escape), a usable Sun Light drag and whole-gesture undo, including
  a small nonuniformly scaled parent. All three desktop suites pass on the final dev build; dev and
  Release native tests, warning-free builds, generated consistency and the 47-tool MCP capture
  smoke pass. Native dev/release suites must run sequentially: their existing fixed temporary
  fixture paths collide if the two test processes run simultaneously.
- Final real RADV/RX 9070 XT asynchronous dynamic-golden playback/resize/PNG/WebM smoke passes:
  `source=vulkan`, 31 submitted frames, 30 explicitly reported resolution-change drops. Capture
  provenance, deterministic CPU capture and Blender sandbox/import contracts are unchanged.

Docking and layout follow-up (2026-09-17):

- All seven editor panels (Controls, Hierarchy, Inspector, Assets, History, Diagnostics and
  Viewport) use Dear ImGui's checksum-pinned v1.92.1-docking release. Drag tabs to dock, group or
  float panels; drag dividers or floating edges/corners to resize. Floating panels stay inside the
  SDL window; native desktop multi-window support remains deferred.
- `EditorLayout` owns default docking, the Layout > Reset layout command and persistent settings
  in `.relay/editor-layout.ini` (ignored by Git). `RELAY_EDITOR_LAYOUT_PATH` is a trusted local
  test/configuration override. Preferences are separate from scene/manifest state and undo.
- Vulkan scene drawing runs from a viewport-window draw callback, then ImGui restores its render
  state. Scene and gizmo follow the panel's size, position and ordinary floating-window stacking.
  Capture/readback paths still draw the scene directly and exclude editor chrome.
- Docking backend renderer shutdown also destroys platform-window registration. Swapchain
  invalidation now restarts SDL and Vulkan ImGui backends together while preserving the context,
  panel layout and fonts, preventing mouse/keyboard input loss after native resizing.
- `tests/editor_layout_smoke.py` verifies splitter resizing with unchanged scene/history,
  picking/gizmo alignment, floating Inspector move/resize, saved-layout restart, reset and a
  floating/resized Viewport's rendering and reversible edits. Evidence:
  `captures/phase-f-docking-adjusted.png`, `captures/phase-f-docking-restored.png`, and
  `captures/phase-f-docking-floating-viewport.png`.
- The existing interaction, inspector regression and Godot-navigation desktop suites pass.
  Harnesses use isolated layout files, updated docked-panel coordinates and explicit focus
  restoration after screenshots. Warning-free dev/release builds, both native suites, generated
  consistency and the 47-tool MCP deterministic capture/provenance smoke pass.
- Repeated native window resize checks pass with live toolbar input after both swapchain
  recreations. Real RX 9070 XT/RADV dynamic-golden animation/resize/asynchronous PNG/WebM
  playback passes (`source=vulkan`, 31 frames, 30 explicitly reported resize drops).
  Deterministic CPU capture, glTF/GLB normalization, FBX fallback, golden fixtures, import bounds,
  scene/manifest compatibility and fail-closed Blender conversion remain unchanged.

Menu-bar follow-up (2026-09-17):

- File/Edit/Scene/View/Run/Tools/Layout/Help menus expose existing protocol-backed workflows.
  Open/Save As/import use project-local filename dialogs with inline failures and Enter/Escape;
  Ctrl+O/Ctrl+S/Ctrl+Shift+S shortcuts respect text fields and open popups.
- Scene adds empty nodes, built-in quad/triangle meshes, cameras and directional/point/spot
  lights. Component-bearing node creation uses the existing create + component transactions;
  undoing the component and node remains two native history entries.
- View can close/reopen all seven panels and reset the editor camera; Layout reset reopens all
  defaults. Help supplies a controls reference and About window. Project/new-scene/export,
  clipboard/duplicate, scripts, physics, audio, standalone build, rendering overlays and agent
  workspace remain disabled placeholders marked Coming soon.
- Tools queues explicit Vulkan PNG capture and starts/stops 30-fps WebM (300-frame limit).
  Capture defaults use timestamped filenames. Dialogs identify provenance and excluded editor
  chrome. Paused simulation requires resume or steps to feed recording; Tools shows background
  finalization and disables new recording until it completes.
- GPU-related menu actions are queued until after frame presentation. Readback submits its own
  Vulkan frame, so submitting directly during menu build would reuse an acquired semaphore and
  stall the renderer. `EditorUi::process_actions()` drains the requests from the desktop loop;
  capture workers/protocol and deterministic CPU source remain unchanged.
- `tests/editor_menu_smoke.py` passes real-input node/component creation, menu undo/redo,
  compatible Save As/Open, asynchronous GPU PNG, WebM start/stop/finalization and Layout reset.
  Evidence includes `captures/phase-f-file-menu.png`, `captures/phase-f-scene-menu.png` and
  `captures/phase-f-menu-bar.png`. The docking test now opens Layout at its new menu position.
- Live panel hide/reopen and Help checks pass with unchanged scene history; screenshots are
  `captures/phase-f-inspector-hidden.png`, `captures/phase-f-inspector-reopened.png` and
  `captures/phase-f-editor-controls.png`. Final dev/release builds and native tests, generated
  consistency, 47-tool MCP provenance smoke and RX 9070 XT/RADV dynamic-golden GPU
  playback/resize/PNG/WebM smoke pass (`source=vulkan`, 31 frames, 30 reported resize drops).

Viewport and Controls polish (2026-09-17):

- Editor background is neutral dark grey (linear RGB 0.018). Bare demo/runtime presentation keeps
  its existing background. Empty Scene rendering no longer falls back to the demo triangle;
  the standalone first-light demo still intentionally renders its demo content.
- A procedural Vulkan XZ plane provides antialiased one-unit/ten-unit gridlines with minor-line
  subpixel suppression and a 35–90-unit camera-distance fade. The grid uses the scene's depth,
  draws after opaque meshes without writing depth, and respects viewport position/size/stacking.
  View > Ground grid toggles it; scene-camera mode suppresses it. This is editor chrome and is
  excluded from captures/readback, preserving scene-only golden fixture comparison.
- Grid shaders share the existing 128-byte push-constant ABI (view-projection plus camera data),
  checked through reflection. Their pipeline is rebuilt/destroyed with the swapchain and shader
  compilation is a normal CMake build dependency. Empty scenes now also build their camera view
  so an empty authoring viewport can show the grid without creating any entity.
- Controls drops the Relay label, filename field and redundant Save/Open actions (File/shortcuts
  remain available), uses vector icons with tooltips and displays smoothed ImGui editor FPS instead
  of the simulation frame counter. Disabled controls retain disabled icon styling. Native docking
  style places the header menu/hide-tab-bar button immediately left of its close button.
- Desktop harness calibration now targets the compact simulation icon and places the test window
  away from unreachable mixed-scale monitor seams. Visual evidence:
  `captures/phase-f-grey-grid-empty.png` and `captures/phase-f-grey-grid-object.png`.
- All five desktop suites pass on the final rendering/Controls implementation: interaction,
  inspector/morph regression, Godot-style navigation/Sun Light, docking/restart/native resizing,
  and menu PNG/WebM. The tab drag test targets the label rather than its newly shifted close
  button. Harness warps use damped corrections and a relative fallback; the test window stays
  fully on one display so monitor gaps cannot reject valid local widget targets.
- Warning-free dev/release builds, both native suites, generated consistency and the 47-tool MCP
  provenance smoke pass. RX 9070 XT/RADV real dynamic-golden playback/resize/asynchronous PNG/WebM
  passes (`source=vulkan`, 31 frames, 30 explicitly reported resize drops). Deterministic CPU
  capture, import normalization/fallbacks/bounds, scene/manifest reload and fail-closed Blender
  sandbox requirements remain unchanged.

Deferred from this phase, not blocking it:

- [x] Multi-selection, copy and paste (implemented in the 2026-09-17 usability follow-up).
- [x] Add headless editor interaction coverage to `ctest`: selection, clipboard, timeline, close
      guards and project Save As/browser now run through internal ImGui input with no windows.
      The older xdotool harnesses remain optional desktop-only checks.
- [ ] Per-triangle picking; `scene.pick` is bounds-level, which answers "which object" but not
      "where on the surface".

### Phase G — permissioned agent workflows and embedded chat

- Add explicit per-session capability grants, auditable tool scopes and approval/denial paths for
  mutations, filesystem effects and destructive operations before broader transport exposure.
- Integrate a chat panel with the out-of-process bridge; keep provider credentials, conversation
  history and model orchestration outside the C++ engine.
- Expose bounded logs, captures, telemetry and action results in the human workflow.

Definition of done: a user can grant limited access, collaborate with an agent and inspect its
  actions; denied actions cannot mutate state or bypass native path restrictions.

### Phase H — asset and rendering correctness/scaling

- Address the A–C lifecycle follow-ups: safe reclamation, dependency digests, manifest retirement,
  asynchronous versioned uploads and resource retirement.
- Implement transparent rendering and material culling correctness; scale texture resources and
  add batching/instancing/spatial indexing based on measurements.
- Consolidate render-graph resource/synchronization ownership and evaluate GPU deformation when
  CPU skin/morph streaming becomes a measured bottleneck.

Definition of done: representative multi-model scenes hot-reimport safely without whole-device
  stalls, preserve saved/playback bindings, render sidedness/transparency correctly and report
  bounded, measured resource usage. This phase can be pulled earlier if E/F expose a blocker.

### Phase I — lighting, shadows, HDR and post-processing

- Extend the initial direct-light PBR path with shadows, image-based lighting, HDR render targets
  and a post-processing/tone-mapping pipeline.
- Add typed, inspectable settings and golden scenes/captures; expand light types/budgets as needed.

Definition of done: representative lit scenes have verified shadows/HDR behavior and consistent
  controls through both the editor and automation, with GPU capture and performance evidence.

### Phase J — render-backend portability and platform verification

- Introduce a backend boundary without losing the deterministic CPU oracle or working Vulkan path.
- Add Direct3D 12 for Windows and Metal for macOS, plus build/package and platform test coverage.
- Close the Windows/macOS Blender sandbox and verification TODOs before claiming safe conversion
  support there; this safety work may be implemented earlier independently of new render backends.

Definition of done: a representative saved project builds, imports, renders and captures correctly
  on each claimed platform/backend, and unsupported capabilities fail explicitly.

### Phase K — ray tracing and denoising/upscaling

- Implement actual hardware ray-traced rendering behind capability checks and a usable fallback.
- Add denoising and a backend-neutral upscaling interface, then integrate DLSS/FSR where supported.
- Expose quality/performance controls and verify rendered results rather than capability detection.

Definition of done: supported GPUs render verified ray-traced/upscaled output with measured costs;
  unsupported configurations use a documented fallback without breaking the project.

### Phase L — game authoring, packaging and export

- Add game scripting/component authoring, safe hot reload and a project/plugin packaging model.
- Build reproducible standalone game/export workflows for the supported platforms.
- Preserve typed automation, deterministic tests and explicit permissions for new authoring tools.

Definition of done: a small playable game can be authored in Relay, packaged and launched outside
  the editor on supported platforms, with reproducible build/export tests.

## Recommended prompt for the next session

Use this as the next instruction after giving the agent this handoff:

> Read AGENTS.md first: never control mouse/keyboard, change focus or open windows without
> specific advance user approval for the current task. Inspect HANDOFF.md, README.md and the
> current branch before changing code. A–F and the three editor usability priorities are implemented
> on the tested Linux path, with the limits recorded below. Grid fading was visually verified by
> the user after the final correction; it is no longer pending. Preserve any subsequent local edits and
> project data. Continue Phase G: explicit session capability grants and audited scopes before
> chat. Use isolated background GPU verification where practical; get fresh approval before any
> normal desktop interaction.
> Keep providers and credentials outside the C++ engine. Validate using background builds, protocol
> tests and the new headless ImGui input tests; do not request desktop access when these suffice.
> Keep every editor mutation routed through ControlProtocol rather than
> touching Scene directly, keep one undo entry per gesture, keep read-only methods untraced, and
> keep the UI excluded from captures so golden images stay comparable. Keep the unsaved marker a
> comparison of scene revisions rather than a change count, so undoing back to a saved state still
> reads as saved, and keep runtime-only state such as animation playback time out of the undo
> history. Preserve the deterministic CPU capture source and capture provenance, glTF/GLB animation
> normalization, Assimp FBX fallback, golden fixtures, import bounds and compatible scene/manifest
> reload. Never relax fail-closed untrusted Blender conversion on platforms without an OS sandbox.
> Verify generated protocol consistency and background native/MCP tests. Windowed GPU checks
> and `tests/editor_*_smoke.py` are optional and require specific advance user approval. Report
> unrun desktop verification as pending; it must not block completion of background work.

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
- `include/relay/editor/editor_overlay.hpp`
- `include/relay/editor/editor_ui.hpp`
- `src/editor/editor_ui.cpp`
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
- Verify renderer milestones with background tests. Windowed GPU captures and desktop checks
  require specific advance user approval; otherwise report that verification as pending.
- Do not describe capability detection as feature implementation.
- Do not describe accepted file extensions as full Godot import parity.

## Current editor state and verification — 2026-09-17

This section supersedes the dated palette, font, Controls-panel and grid notes above. Those earlier
sections record tests at the layout that existed then, rather than validation of today's coordinates.

- Maximized human-editor startup, high-density SDL framebuffer and per-frame scale refresh using
  display scale / window pixel density. ImGui applies framebuffer scaling and font rasterization.
  Native KDE/Wayland 110% was observed as preferred scale 132/120; no doubled content scaling.
- Charcoal chrome based on HTML `#202020`, neutral grey surfaces and blue UI selections. Body and
  heading fonts are 17 logical units; diagnostic monospace is 16, including fallback sizing.
  Packed linear ImGui colours approximate the HTML base; viewport clear uses linear 0.014444.
- Fixed icon toolbar beneath File/Edit; no Controls dock tab, close button or View-panel toggle.
  Hierarchy, Inspector, Assets, History, Diagnostics and Viewport remain movable/resizable/dockable.
  Old Controls settings migrate without resetting the other saved panes. Shared tabs are retained;
  removing a dedicated Controls leaf merges its sibling into the parent. Layout reset restores six
  content panes. Preferences remain separate from scene/manifest and undo history.
- Godot-style camera navigation, same-frame camera/gizmo/render alignment and useful framing of
  lights/cameras/empty nodes. Existing editor-contract and transform/morph fixes remain in place.
- XZ grid: unit lines, ten-unit major lines, antialiasing/subpixel suppression and distance fade
  35–90 units. Height planes are Y=0,10,20,...: Y=0 holds through camera Y=5, transitions 5–10;
  Y=10 holds through 15, transitions 15–20, and so on, mirrored below ground through negative levels. Smoothstep weights
  are continuous at boundaries. View > Ground grid toggles it; scene-camera mode suppresses it.
- Selected editor-camera meshes have a yellow-orange silhouette (approximately `#FFB930`), including
  drawable descendants. A depth-tested stencil mask and eight translated screen-space copies form
  a two-logical-pixel outline, using the current MVP and deformed vertices. Alpha-mask cutouts are
  respected; blended fragments with alpha below 0.05 are skipped. This is no scene mutation.
- Vulkan depth/stencil formats: D32S8 preferred, D24S8 fallback. Stencil clears each render pass.
  Grid/outline shaders are compiled by CMake and reflected against the 128-byte draw ABI; pipelines
  are retired/recreated with swapchain resources. Decorations obey viewport scissor and UI z-order.
- Exports omit UI, gizmos, grid and selection outlines, while preserving the editor viewpoint.
  Deterministic CPU capture/provenance, glTF/GLB normalization, Assimp FBX fallback, golden fixtures,
  import bounds and compatible scene v4 / manifest v3 reload remain unchanged. Untrusted Blender
  conversion remains fail-closed on platforms without an OS sandbox.

Current verification and reproduction (windowed GPU and desktop commands require specific
advance user approval; these commands do not grant it):

```sh
cmake --build --preset dev -j4
ctest --preset dev
cmake --preset release -DRELAY_BUILD_TESTS=ON
cmake --build --preset release -j4
ctest --test-dir build/release --output-on-failure
npm --prefix tools/mcp-bridge run check
npm --prefix tools/mcp-bridge run build
python3 tests/mcp_capture_smoke.py
python3 tests/editor_visuals_smoke.py
./build/dev/relay_demo --vulkan-async-smoke relay-dynamic-golden.gltf
```

- Dev/release native tests and generated protocol/MCP checks have passed. MCP smoke reports 47 tools,
  deterministic provenance and an explicit GPU request failing closed in headless mode.
- The current desktop regression verifies amber silhouette pixels, vertical flight across a full
  interval, unchanged history, resize/reframing and byte-identical GPU exports before/after selection.
  It requires xdotool, Spectacle, Pillow and an interactive desktop; it is not a headless CI test.
- Actual desktop input also verified fixed-toolbar pause/resume and resistance to dragging, and
  native Wayland verified maximized startup, fractional scaling and saved-layout migration.
- Real RX 9070 XT/RADV GFX1201 dynamic-golden playback/resize/async PNG/WebM has passed with explicit
  source=vulkan, 31 frames and 30 reported resolution-change drops. Earlier combined-tree tests also
  exercised golden GLB, Assimp FBX and sandboxed Blender. No Windows/macOS parity is claimed.
- Evidence is regenerated locally, not committed: `captures/editor-outline-framed.png`,
  `captures/editor-grid-raised.png`, `captures/editor-outline-resized.png`,
  `captures/editor-navy-fixed-toolbar.png` (older navy palette), `/tmp/relay-phase-e.png` and
  `/tmp/relay-phase-e.webm`. Current palette/thresholds are defined in source, not older screenshots.

Carry forward:

- [ ] Recalibrate the five older pixel-coordinate desktop suites for maximized startup, 17-unit
  fonts and the fixed toolbar. They last passed before these layout changes; do not claim a fresh
  pass from those historical results. The current visuals suite remains runnable independently.
- [x] Phase G native session capability grants and bounded action audits.
- [x] Phase G local host approval interface, token authentication, supported entity/file scopes and
      external-bridge chat integration, verified in background tests with a mock provider.
- [ ] Phase G live-provider and normal desktop verification; expanded adapters/multi-client identities.
- [ ] Preserve the earlier A–D importer/resource-lifecycle checklist and platform sandbox TODOs.

## Daily editing and save workflow — 2026-09-17

This continues the editor-usability work above: duplication, knowing whether a scene needs saving,
starting and reopening scenes, and animator controls that can actually be used to judge a pose.

Protocol v8 adds two methods, 49 native methods and 49 generated MCP tools. Scene v4 and manifest
v3 are unchanged, and no capture, import or sandbox behaviour moved.

- `scene.duplicate` copies an entity and its descendants beside the original as one transaction.
  Components are preserved. A copied camera is forced inactive, because duplicating an object
  should never silently steal the view and the scene holds at most one active camera. A copy of a
  whole imported model rebinds its `model_node` roots to itself so it drives its own animation,
  while a copy of a single node from inside a model stays bound to the original model root, which
  is the instance it still belongs to. The copied root is renamed `X Copy`, then `X Copy 2`, and a
  copy of a copy does not grow `X Copy Copy`. A subtree is bounded at
  `Scene::maximum_duplicate_entities` (4096) and an oversized one is refused whole rather than
  copied part-way.
- `scene.clear` empties the scene as one undoable transaction, destroying each entity so handles
  taken beforehand stay stale instead of aliasing whatever is created next.
- `SceneHistory::revision()` names the authored content instead of counting changes. Each
  transaction carries a unique serial and the revision is the serial on top of the undo stack, so
  undoing back to a state that was already saved restores that state's revision and the editor
  stops reporting unsaved work. An update folded into a gesture takes a new serial, so a scene
  saved mid-drag does not look saved for the rest of the drag. Eviction and `clear()` move the
  base revision to the state underneath the stack rather than resetting to zero.
- The revision appears in `history_json`, so every response that already embedded history now
  carries it, and `scene.save` reports the revision the file holds. Result fields are not part of
  the generated schema, so these are additive.

Editor behaviour:

- The window title is `<scene file or "Untitled scene">[*] - Relay Editor`. The trailing name is
  kept so anything matching on it, the desktop harnesses included, still finds the window.
- File gains New scene (`Ctrl+N`). New, Open and Quit (including the native window close button) route through one guard that prompts when
  there is unsaved work. `Enter` saves and continues, but only when the scene already has a file;
  defaulting to discarding unsaved work would be worse than leaving that choice explicit. `Escape`
  cancels. Save with no file behind it yet opens Save As rather than claiming whatever name the
  field happened to hold.
- Edit > Duplicate (`Ctrl+D`) and a hierarchy context-menu entry issue `scene.duplicate` and select
  the copy, so the next drag or inspector edit lands on the new object.
- The animator section offers clips by name from `render.assets` and a time slider bounded by the
  selected clip's length, with the clip length shown and a Restart control. An unknown or
  zero-length clip falls back to the previous open-ended drag rather than becoming unusable, and a
  model whose clip list has not arrived yet still gets the raw index field. The slider uses the
  draft buffer across refreshes and sends each changed time immediately. A shared animation gesture
  token folds these live updates into one undo entry.
- Animation playback mutates animator time outside the history, so a playing clip does not mark
  the scene modified. Because that makes the inspector's playhead move without any transaction,
  panels poll at 20 Hz instead of 2 Hz while a selected animator is playing; the expensive asset
  and model listings keep the slow cadence, and read-only polling is still untraced.

Verified on this host:

- Warning-free dev and Release builds, both native suites, no generated protocol drift, MCP
  `check`/`build`, and the MCP capture smoke reporting 49 tools with deterministic provenance and
  an explicit GPU request failing closed in headless mode.
- Native tests cover the revision round trip through undo/redo, a folded gesture still moving the
  revision, `scene.save` reporting it, duplication as one undo entry with descendants and
  components intact, the inactive copied camera, `model_node` rebinding for a whole model versus a
  single node, the subtree bound refusing without leaving a partial copy, and clear/undo.
- `tests/editor_workflow_smoke.py` passes 23 real-input checks: Ctrl+D duplication with its
  selection proven by what `Delete` removes, whole-gesture undo, the unsaved prompt blocking and
  then returning keyboard control, Save As, New scene, reopening, and the inspector's play and
  restart controls found by their effect rather than by fixed coordinates.
- `tests/editor_visuals_smoke.py` still passes, including with the new window title.
- Real RX 9070 XT/RADV dynamic-golden playback/resize/asynchronous PNG/WebM passes unchanged:
  `source=vulkan`, 31 frames, 30 explicitly reported resolution-change drops.

Two things worth knowing before extending this:

- Synthetic key delivery on this desktop is not fully reliable. A repeated `Ctrl+O` reached the
  editor roughly half the time when sent per-window and most of the time through XTEST, so the
  workflow suite sends shortcuts through XTEST and retries interactions that have a checkable
  outcome, printing when it retried. Every assertion is still against engine state. An earlier
  version of that suite failed for a different reason worth remembering: it cleared the dialog's
  prefilled filename with `Ctrl+A`, which did not always reach the field, so the new name was
  appended to the old one and the editor was asked to open `<name><name>`.
- Driven testing is what surfaced that the unsaved-changes prompt could not be answered from the
  keyboard at all, which strands anyone who reached it from a shortcut. That is now fixed.

Deferred from this work:

- [ ] Multi-selection and cut/copy/paste. Duplication is per-entity for now.
- [ ] A project model above single scene files; File still offers only scenes, and New/Open/Export
      project remain Coming soon.
- [ ] An animation timeline. The inspector controls one animator at a time with no curve editing,
      and Tools > Animation timeline is still a placeholder.
- [ ] The playback poll rate is a fixed 20 Hz while an animator plays. If that becomes a cost,
      drive the playhead from the editor's own clock between polls instead of polling harder.

Publishing scope: include native/editor/capture implementation, generated protocol/MCP artifacts,
shaders, regression scripts and documentation. Exclude build products, captures, import caches,
Python bytecode, local layout preferences and the local manual-test scene `scenes/phase-f.relay.json`.

Publication checks rerun on the final snapshot (2026-09-17): dev/release builds and native suites,
generated protocol check, MCP check/build and 47-tool headless capture smoke, current desktop
visuals regression, and RADV real-GPU dynamic playback/resize/asynchronous PNG/WebM all passed.
Historical desktop coordinate suites remain explicitly deferred as above.


Audit follow-up (2026-09-17): animation time sliders now seek while held down, using a generated
`scene.set_animation` gesture parameter and entity-specific history labels to keep one scrub in one
undo entry. Mutation responses immediately update the editor revision, closing the brief stale-state
window in the unsaved-work guard. Native SDL quit events now route through the editor guard rather
than bypassing it in the Vulkan window. Native regression coverage includes live seeks, whole-scrub
undo and a new gesture after undo; the desktop workflow checks seek state before mouse release.

Audit validation: dev build and `ctest --preset dev` passed; generated artifacts are current;
MCP build/check and capture smoke passed. The desktop workflow passed all 25 checks, including
pose changes before slider release and one undo entry throughout the drag. A separate desktop
check on the final build verified that Alt+F4 leaves an unsaved scene open and intact, and Escape
cancels the close prompt. Desktop tests required access outside the sandbox to the X11 session.


## Three editor usability priorities — implemented 2026-09-17

This section supersedes earlier next-session recommendations to implement selection, projects and
an animation timeline. These changes follow the published `4c3d16e` baseline and are included in this commit.

- Multi-selection: Ctrl-toggle in hierarchy/viewport, Shift-range in visible tree order, Ctrl+A,
  active-object inspector, group outlines/framing and world-space group gizmos. Complete selected
  forests are copied/cut/pasted/duplicated/deleted atomically; overlapping ancestor selections do
  not process children twice. Shared gesture tokens keep a group transform in one undo entry.
  Singular parents or unrepresentable shear reject the complete edit. `SceneHistory::execute`
  now restores its before-state when a mutation returns false after partial changes.
- Session clipboard lives on Engine so protocol instances share it. Copy does not enter scene
  history; clipboard data survives deletion and scene/project changes. Paste recreates fresh
  entities, remaps internal parent/model bindings and keeps cameras inactive. Partial imported-node
  copies drop external model bindings. Forest size remains bounded to 4096 entities.
- Project v1 `*.relayproject` files in project folders organize up to 128 scene members with a name and startup
  scene. New/Open project and the Project browser are implemented; Save As/Save add membership.
  Scene context menus set startup or remove membership while preserving files. Metadata writes
  atomically; unsafe names, symlinks and invalid/missing startup content are rejected without
  changing the active scene/project. Each file owns its containing folder: recursive Assets discovery and model imports use that
  root, scene storage uses its scenes/ directory, and opening projects does not change cwd.
  Paths are workspace-relative. Metadata edits persist separately from scene undo; export is deferred.
- Timeline is a dockable panel: model-instance rows, active clip list, seconds ruler, transport,
  frame stepping, display FPS, frame snapping, speed and loop. The ruler pauses/seeks selected
  animation roots live, as one undo transaction, with per-clip clamping. Imported-child selection
  resolves to its animation root; no selected root defaults to the first available animator.
  Channel key markers are read-only and bounded (256 channels, 256 sampled keys each, full counts
  reported). Animation authoring/keyframe editing, blending and retargeting remain deferred.
- Protocol v9 adds forest/clipboard/group-transform methods, `scene.set_animations`, `animation.clip`
  and eight `project.*` methods (66 total). The generator and native validator now support bounded
  string/number arrays using the shared strict JSON reader. Generated C++, MCP TS/JS and docs agree.

Background verification architecture:

- `relay_workflow_tests` runs against temporary workspace files and actual ControlProtocol/Engine
  state: clipboard persistence and bindings, forest deduplication, group undo/rollback, rotated
  and singular parent transforms, project reopen/startup/failure protection, safe paths and symlinks,
  bounded channel markers, frame snapping/stepping and synchronized animation seeks.
- `relay_editor_headless_tests` constructs CPU-only ImGui frames through
  `EditorUi::initialize_headless`. No SDL initialization, SDL windows, Vulkan device or OS input
  are used. Internal input events exercise actual hierarchy modifiers, clipboard shortcuts,
  held-ruler seek/undo, native close guard and project Save As/browser workflows. Widget rectangles
  are exposed only as read-only observations of headless frames. Layout/files use temporary paths.
- Both targets are part of `ctest`; the UI target is conditional on editor dependencies. Historical
  desktop test commands are optional approval-only checks. For background MCP execution set
  `RELAY_RUNTIME_MODE=headless` so its automatic editor mode cannot open a desktop window.

Next substantive phase: G, capability grants and audited scopes before embedded chat. The user
verified the grid visual follow-up. Keep the background-first rule in AGENTS.md and CLAUDE.md;
no desktop interaction is authorized for the next session.

Validation for the three priorities: dev and Release builds completed without warnings. All three
`ctest` suites passed in both builds (engine, background workflow, headless editor input). Generated
protocol checks, MCP TypeScript build/check and the 66-tool headless MCP capture/clipboard smoke
passed. No desktop windows were opened and no OS mouse/keyboard events were sent during this work.
Windowed GPU visual checks were not rerun; existing selection shaders are unchanged and the new
selection routing is compiled, with selection and editor behavior checked through headless tests.


### Project folders and panel cleanup (2026-09-17)

Project, Timeline and History start hidden and remain available from View. Removed duplicate panel
headings. Optional panels join existing docks when a layout predates them. Timeline transport uses
vector icons with tooltips, a compact FPS/speed row and a separate scrolling track area; channel
labels are clipped to their column. Folder projects use `*.relayproject`, and Assets lists all visible
files recursively from the project root with nested models selectable for import. Headless coverage
checks default visibility, explicit reopening, project asset isolation and scene persistence.
Desktop inspection was specifically approved by the user for this task; future desktop actions
still require fresh approval under AGENTS.md.

Verification for the panel/folder follow-up: desktop screenshots confirmed hidden optional panes,
removed duplicate headings, project-folder discovery, actual nested glTF import and the wider
timeline sharing Diagnostics' bottom dock. The temporary review editor and project folder were
closed/removed. Ruler labels now use evenly spaced decimal intervals and speed shows two decimals.

Icon follow-up: Loop uses rounded opposing arrows, Rotate uses an arrowhead attached to the
arc along its tangent, and Focus uses a centered filled dot instead of a tiny outlined ring.

Grid follow-up: retain the original vertex-shader smoothstep crossfade and 128-byte push layout.
The negative-height correction is limited to absolute camera height for level selection and signed
plane heights. An initial native calculation/payload change was reverted after the user reported
popping; do not describe CPU math checks as rendered fade verification.

User verification update (2026-09-17): the final grid fade is visually confirmed smooth during
both upward and downward camera movement. This closes the earlier pending rendered check.


## Phase G continuation — native capability boundary (2026-09-18)

Historical initial-boundary record; superseded by the completed local-workflow record below.
At this stage the native permission prerequisite was implemented and the rest of Phase G remained incomplete.
Protocol v10 has 68 native methods/MCP tools. Scene v4, manifest v3 and project v1 are unchanged.

- Agent stdio, editor-stdio and each accepted loopback connection dispatch through
  `ControlProtocol::handle_agent`. Default access is limited to `session.status` and
  `session.audit`; every action/inspection method needs its own explicit host grant.
- The host supplies exact comma-separated native names through `RELAY_AGENT_GRANTS`, or uses
  `set_agent_grants` on the render/control thread for replacement/revocation. Invalid grant lists
  revoke all access; wildcards and agent self-grant tools are unsupported. Environment policy is
  snapshotted at startup/per accepted connection and cannot be changed through the wire.
- Trusted human editor requests still use `ControlProtocol::handle`; agent restrictions do not
  disable human editing or introduce a separate Scene mutation path/undo history.
- A bounded 256-entry connection-local audit records host grant changes, validation failures,
  denied actions and allowed outcomes. Entries expose exact method scopes, request IDs,
  read-only/destructive flags and host policy snapshots, without request parameters. Sequence
  bounds expose eviction; successful status/audit reads neither trace nor audit themselves.
- Denials run before native dispatch and trace recording. Generated validation, native path
  checks and import sandboxes remain enforced after grants. Read-only operations stay untraced.
- Agent `trace.replay` remains ungrantable: nested commands, frame advancement and input effects
  need a full authorization preflight before replay can safely execute.
- MCP defaults to headless operation; a desktop runtime requires explicit `RELAY_RUNTIME_MODE=editor`.
  The old headless synchronous Vulkan capture subprocess fallback was removed because it escaped
  the owned session's permission boundary. Vulkan requests now fail without a live renderer.
- Provider SDKs, credentials, conversation history and orchestration remain outside C++.

Verification:

- Development and release builds passed with no compiler warnings.
- All four background CTest suites passed: native, workflow, session stdio/loopback and headless
  ImGui input tests. Loopback tests required reviewed sandbox access for local sockets; they do
  not initialize SDL/Vulkan or interact with the desktop.
- Generated protocol consistency, MCP type check/build, 68-tool deterministic capture smoke and
  default-ungranted MCP startup/capture regression passed. `git diff --check` passed.
- An isolated Xvfb GPU smoke was attempted with desktop display variables disabled. RADV could
  not present because this private X server lacks DRI3; no software Vulkan ICD is installed.
  Rendered GPU verification is therefore pending, as are unrun normal desktop checks. Background
  permission/protocol verification is complete and does not require desktop interaction.
- No normal desktop windows, OS input or focus controls were used. The previously user-verified
  grid fade remains resolved; this phase changes no shaders, captures or scene serialization.

Remaining boundaries: grants currently cover exact methods across the active project, not files
or entities; local clients share the listener's configured policy without authentication; audit
history is in memory and ends with its connection. Build a human grant/revocation review interface,
resource-specific scopes, authenticated connection identity and optional durable audit export
before broader exposure. Embedded chat remains pending and must use the out-of-process bridge.


## Phase G — completed local permission/chat workflow (2026-09-18)

The Phase G local milestone is implemented on the tested Linux path: a human can grant limited
access, collaborate through the external bridge and inspect actions. Native denial/path boundaries
are verified through real protocol dispatch and headless ImGui input. This does not claim live
provider/rendered desktop verification, broad transport exposure or Windows/macOS parity.

Current protocol: v11, 79 native methods and 69 MCP tools. Scene v4, manifest v3 and project v1
remain unchanged. Generated host-only and authenticated-bridge-only methods are excluded from MCP
and from the provider tool list; agent tools cannot self-approve or publish bridge messages.

Implemented:

- **View > Agent** is hidden by default and dockable beside Diagnostics. Chat, Access and Actions
  show bridge messages, request effects/project context, Allow/Deny, manual method/entity/file
  grants, per-method/all-access revocation, bounded tool summaries and audit export. Approval
  controls precede longer descriptions so they remain reachable in the default bottom dock.
- `session.request` queues at most 32 deduplicated access requests without approving or executing
  them. Host `session.grant`, `session.decide`, `session.revoke` and `session.export_audit` dispatch
  through ControlProtocol and do not alter scene undo or deterministic traces.
- Exact-method grants and supported entity/file scopes are bound to the active project. Project
  open/create/close attempts revoke all grants. Scene load/clear and undo/redo attempts revoke
  entity grants to prevent restored allocator handles from inheriting access. Stale project
  requests cannot be approved. Unsupported resource/method combinations fail closed.
- Entity scopes support inspect/bounds/transform/morph/light/renderer/name methods. File scopes
  cover scene save/load, model imports, capture, video start and trace start. Filename bounds
  restrict a method's source/output, not all its normal effects: imports/loads can restore
  dependencies/manifests and replace the scene; entity transforms/bounds retain descendant
  effects. Cameras, project operations, whole-scene history and replay have no entity scope.
- Loopback startup requires `RELAY_AGENT_TOKEN` (32–128 characters). A socket-only
  `session.authenticate` handshake must succeed before dispatch; bad/missing tokens close the
  client without mutation. Initial receive timeout/buffering are bounded. Authentication failure
  diagnostics contain no tokens. Stdio remains an owned-process private pipe.
- Audit records include decisions/outcomes, host-approved scope metadata, project context and
  request IDs. Export publishes a bounded JSONL snapshot under `.relay/audits/` atomically without
  overwriting files or following symlink ancestors. Exports are optional persistent snapshots,
  not continuous or tamper-proof journals; eviction and connection-lifetime limits remain explicit.
- The TypeScript bridge owns an OpenAI-compatible text/function-call adapter, bounded canonical
  conversation turns and HTTP/tool orchestration. Endpoint/model/key are configured only in its
  environment. Runtime children receive no provider credential/configuration variables. A fresh
  private bridge token authorizes bounded mailbox/display projection calls, never native actions.
- Chat checks native grants for every tool, stops for access review and resumes only on a human
  follow-up. Stop aborts active provider work/future calls, preserving completed native actions.
  Round/tool/time/response bounds, redirect refusal, credential redaction and rejection of
  credential-bearing tool arguments are covered by tests. No provider request occurs without a
  submitted human message; unconfigured chat reports its configuration state.
- A human can build the bridge and run `npm run editor` from `tools/mcp-bridge` to launch the editor
  with chat, or explicitly select editor mode through MCP. These commands open desktop windows;
  agents still need fresh task-specific desktop approval. Default MCP mode remains headless.
- MCP scene-save results now resolve their absolute path from the native response, preserving
  active project scene roots. Captures retain native authorization and source provenance.

Final background verification:

- Development and release builds passed without compiler warnings.
- All four CTest suites passed in both builds: native engine, editor workflows, authenticated
  stdio/loopback transport and headless ImGui input. Headless approval/denial/revocation/export
  tests run at the default dock size, without SDL, desktop windows, OS input or focus changes.
  Chat Send/Stop also run through actual ImGui input and the authenticated bridge mailbox, without
  provider calls or scene-revision/undo changes.
- Three bridge tests passed using mocked fetch and the real native session test host: environment
  separation, approval pause/resume and actual scoped tool dispatch/denial, bridge authentication,
  provider failure/redaction, credential-bearing argument refusal and cancellation/response bounds.
- Generated protocol consistency, MCP type check/build, 69-tool deterministic capture and default
  ungranted/headless startup regressions passed. `git diff --check` passed.
- Normal desktop and live-provider verification are pending. The earlier isolated Xvfb attempt
  lacked DRI3 presentation support; it was not rerun against the normal desktop. No shader/capture
  or scene serialization changes were made. User-verified grid fading remains resolved.

Boundaries/follow-ups: the initial provider adapter is non-streaming text/function calling; live
endpoint/model access requires external host configuration and has not been exercised. There are
no multi-user accounts or simultaneous socket clients, transport remains loopback-only, and the
private bridge owns its runtime rather than attaching to an unrelated editor. Audit exports are
manual snapshots. Agent trace replay remains unavailable pending authorization preflight of every
nested command/frame/input effect. Broader provider APIs, attachments, live-provider verification,
multi-client policy identities and continuous audit retention remain enhancements. Phase H can
proceed independently of optional desktop/provider verification.


## Phase G — full Auto approval and private editor provider setup (2026-09-18)

Protocol v12 has 81 native methods and 69 MCP tools. New host-only `session.auto_approval`
and `chat.configure` remain absent from generated MCP/provider tools.

- Agent panel header: Auto approval (all actions), default off each session. Allows every public
  engine method without individual approval/scopes, including destructive operations and replay.
  Persists across project changes; disabling restores limited grants and Revoke all clears access.
  Permission changes and automatic actions include the mode in bounded audit records. Agent tools
  cannot enable the switch. Replay dispatches nested commands through agent enforcement/auditing;
  host/bridge administration and recursive replay remain unavailable through imported traces.
- Setup tab: endpoint/model, bearer token/API key, custom authentication header or no authentication.
  Credentials are transient in the trusted UI/mailbox, cleared after submission/consumption, never
  returned in projections, and excluded from scenes/projects/traces/audits. Canonical authentication
  remains in the external TypeScript bridge, in `.relay/agent-provider.json`. The whole `.relay`
  directory (including temp/backup files) is gitignored. File mode 0600, atomic replacement, symlink
  path refusal, bounded loading and sanitized configuration errors protect private storage.
  Blank credential keeps the current secret; explicit removal clears it. Saved setup overrides
  environment fallback. Initial adapter remains compatible Chat Completions, not an OAuth browser flow.
- Public `relay_demo --editor` delegates to the bridge before initializing any window, which starts
  the same selected binary with `--editor-ui-stdio`. Build bridge assets first. No normal desktop
  launch was performed by agents. A fake Node executable verifies delegation entirely in background.
- Full Auto approval chat has no scoped-mode round/tool-count/total-work budget; it continues until
  provider completion or Stop. Response size and individual network timeouts remain bounded.
  Native schema/path validation, import bounds and fail-closed Blender sandbox policy are unchanged.

Final background verification:

- Development and release builds passed without compiler warnings.
- All four CTest suites passed in both builds: native engine, workflow, stdio/loopback authorization
  and headless editor input, including Auto approval/revocation and provider setup controls.
- Six bridge tests passed against development and release fixtures: private credential persistence,
  restrictive file permissions, gitignore coverage, symlink refusal, sanitized errors, custom-header
  authentication, more than 8 rounds/16 automatic calls, and window-free editor launch delegation.
- Headless MCP capture/default-policy smoke, generated protocol consistency, TypeScript checks and
  whitespace checks passed. No scene/project/asset/shader data was modified.
- Live-provider and normal desktop/GPU verification remain pending; grid fading remains
  user-verified, not pending. No normal desktop interaction was performed.


## Phase G — OpenAI device sign-in and agent workspace (2026-09-18)

Protocol v13: 82 native methods and 69 MCP tools. `chat.control` is a new host-only mailbox method
for provider, sign-in/cancel/sign-out/refresh, model/reasoning and New chat controls. All previous
local edits, scene/project/assets and compatible-provider settings are preserved.

- External `openai.ts` uses the official Codex App Server device-code flow, login completion,
  account metadata and model catalogue. Tokens/refresh remain in Codex; only sanitized display
  metadata/device code reach the host-only UI. Dedicated `.relay/openai` home isolates Relay from
  existing host Codex authentication and configuration. Directory mode 0700, restrictive process
  creation mask, private files and symlink/hardlink refusal protect storage. `.relay` remains
  entirely gitignored, including auth, state, logs, provider/harness preferences and backups.
- OpenAI is the default for new configurations. Existing compatible providers are retained. Account
  switches providers and exposes device sign-in/cancel/reconnect/sign-out/refresh; advanced manual
  endpoints remain available for other providers. Model/reasoning options are discovered, validated
  and stored privately, with no hardcoded model list. Codex CLI 0.154.0 and Node.js 26 are tested.
- OpenAI threads are ephemeral, with streaming message projections and generated dynamic tools.
  Every action uses native agent dispatch; host/bridge controls are absent from model tools.
  Pending access requests interrupt the turn. Stop preserves completed actions; New chat releases
  the old thread. Shell/exec, computer/browser, apps/delegation and code-mode capabilities are
  disabled; the isolated empty workspace is read-only. Full Auto approval still permits every
  public engine method, retaining schema/path/import/sandbox safeguards and nested replay auditing.
- Agent opens by default in native editor launch; fresh layouts share the full-height Inspector
  dock. Tools > Agent workspace / Ctrl+Shift+A expands it inside the same native surface. Compact
  saved layouts expose Open workspace rather than clipping controls. Chat has separate message
  cards, basic heading/code formatting, copy, model/reasoning, composer/send shortcut, Follow,
  streaming/busy status, Stop/New chat; Account/Access/Activity separate the supporting controls.
  Activity results are expandable. UI stays excluded from captures.
- Background tests cover mock device login/cancel/logout, token-free account projection, model
  persistence, real native actions, host-tool refusal, pending-review/cancellation, private paths
  and all account/model controls through headless ImGui. An installed-Codex test registers all
  69 dynamic tools and checks signed-out metadata/ephemeral startup without model generation.
  The runtime schema uses `sandbox: "read-only"`, verified against the installed CLI.

The user specifically approved desktop visual verification for this task. A separate temporary-layout
Relay window was used with fixture provider/account data; no existing account/authentication was
accessed and no project or saved layout was modified. Conversation and device-code screens were
visually inspected. Footer spacing/contrast/device-code size were corrected after inspection.
This is not reusable desktop approval for future tasks. Final background/visual results follow below;
completing real account authentication and a live provider turn remain separate pending checks.


Final verification for the OpenAI harness:

- Development and release builds passed without compiler warnings. All four CTest suites passed
  in both builds, including native authorization/socket checks and headless ImGui account/model
  controls. Stop preserves queued private provider settings as well as completed scene actions.
- All 11 bridge tests passed against development and release binaries. The installed Codex runtime
  accepted isolated signed-out account/model metadata and ephemeral registration of all 69 Relay
  dynamic tools, without requesting authentication or running a model turn.
- Generated protocol consistency, TypeScript checks, headless MCP capture/default-deny smoke and
  whitespace checks passed. No scene, project, asset or shader data was modified.
- Task-authorized desktop inspection checked the corrected conversation footer, message contrast,
  expanded workspace and enlarged device code using fixture data in a separate temporary-layout
  Relay window. The verification window was closed afterward and prior focus restored.
- Real Vulkan captures before and after replacing the chat contents were byte-identical, confirming
  UI exclusion (SHA-256 9c192b20f0785bb67c1c486e798226c42859a358786b590044c9c761655b0cdc).
  Only those temporary capture files were removed. Grid fading remains user-verified.
- Real account device-code completion and a live OpenAI conversation remain pending. Fixture
  authentication tests and installed-runtime metadata/tool registration do not claim live sign-in
  or provider generation. Broader optional desktop smoke suites were not run.


## Phase G — dynamic tool delivery correction (2026-09-18)

A live model turn reproduced tool-unavailable replies with Auto approval enabled: no dynamic calls
were emitted and the temporary scene remained empty. `features.code_mode_host=false` disabled the
stable inference tool host despite successful `thread/start` dynamic-tool registration. The bridge
now explicitly enables that stable host; general code mode, shell/exec, browser/computer and apps
remain disabled. This supersedes the earlier blanket code-host-disabled record above.

A subsequent live OpenAI turn in a disposable headless scene successfully called `scene.create`
and `scene.set_light`, creating one point light. No desktop interaction or user project mutation
was performed. The installed-Codex regression checks the effective tool-host and disabled-tool
settings through `config/read`; credentials remain external and gitignored. Existing editor
processes must be restarted to load the rebuilt bridge and replacement Codex service.

Verification: rebuilt TypeScript assets, generated-protocol/TypeScript checks, all 11 bridge tests,
headless MCP capture/default-deny smoke and whitespace checks passed. No native engine code changed.


## Phase G — chat composer and automatic follow (2026-09-18)

- Enter sends and Ctrl+Enter inserts a newline. The rounded input composer contains a single
  arrow send icon that becomes a square Stop action during generation. Separate Send/Stop/Follow
  buttons and the shortcut/Ready footer were removed; actionable connection/error status remains.
- Model and reasoning controls are hidden in one composer dropup. The model list remains discovered
  from Codex; the slider maps only to that model's supported reasoning levels and commits on release.
  Controls remain disabled during generation and use host-only ControlProtocol mailbox actions.
- Follow is enabled at the transcript bottom. Scrolling up pauses it; a circular down arrow returns
  to the latest reply and restores follow. The overlay has its own ImGui click surface so message
  cards cannot intercept clicks. Manual scrolling back to the bottom also resumes follow.
- Headless regression exercises both keyboard shortcuts, send/stop replacement, hidden menu/model
  choices, slider submission, follow on new replies, paused follow, jump click and manual resumption.
  Chat controls leave scene revision/undo unchanged. No provider/authentication settings were changed.
- Development and release builds, all four CTest suites in each build, all 11 bridge tests, generated
  protocol/TypeScript checks, headless MCP capture/default-deny smoke and whitespace checks passed.
  Scene/project/asset/shader data was preserved. Desktop visual verification of this new composer
  remains pending; no desktop interaction was performed or requested for this task.


## Phase G — compact composer alignment (2026-09-18)

Removed the workspace heading and extra New chat/Account/Expand header buttons. Account remains
available as a tab; Tools > Agent workspace and Ctrl+Shift+A remain available for expansion.
The composer uses its actual available height to size the text input above the action row, with
no outer scrolling. The send/stop icon is aligned to the right edge. A text-sized neutral model
control sits beside it, displaying the configured reasoning level in dimmer text and a drawn
chevron instead of a caret character. The description below the reasoning slider is removed.

Headless input/layout checks passed at standard and narrow panel widths, including zero composer
scroll extents, containment/right alignment and preserved model/reasoning/send/stop/follow behavior.
Development and release builds and all four CTest suites passed. Concurrent native suites initially
conflicted through their shared temporary import fixture; the development suite passed on an
isolated rerun. Run those native build suites sequentially in future. All 11 bridge tests, generated
protocol/TypeScript checks, background MCP smoke and whitespace checks passed. No scene/project,
asset/shader or authentication data was changed. Desktop visual verification remains pending;
no desktop interaction was performed for this task.


## Phase G — wrapped composer and inspection camera tools (2026-09-18)

Protocol v14 has 85 native methods and 72 generated MCP tools. Existing credentials/provider
settings, project data, deterministic capture source/provenance and import sandbox policies remain
unchanged. The composer now soft-wraps at font/viewport width, including long words and Unicode,
while preserving original submitted whitespace/hard newlines. Display-only breaks are mapped away
from submitted text; editing/resizing retains cursor/selection mapping and Delete/Backspace cross
soft boundaries. Enter/Ctrl+Enter behavior and outer composer containment remain intact.

New `editor.camera.status/set/frame` tools use the live editor's view-only camera seam after native
validation and agent authorization. Set configures target, yaw/pitch (radians), distance and
inspector/scene mode. Frame fits entity/descendant bounds through native scene.bounds, without
changing human selection. Status is read-only/untraced. No view control enters scene undo/revisions,
project persistence or deterministic CPU capture; missing editors fail closed. The editor updates
its capture viewpoint immediately. The external OpenAI instructions describe this visual workflow.

Successful PNG render.capture replies now attach App Server inputImage content to the model.
Only bounded regular PNG files under the capture directory are read; unsafe paths/symlinks and
invalid data are refused. Images stay outside native bridge display projections/audits. Existing
Vulkan capture UI exclusion and provenance are preserved; asynchronous capture remains unchanged.

Development/release builds passed without warnings. All four CTest suites passed sequentially in
both builds. All 13 bridge tests passed against development and release fixtures, including actual
image-reply dispatch and installed-Codex registration of all 72 tools without live generation.
Headless tests cover wrapped editing/submission/Unicode/hard blank lines/soft-boundary Delete,
authorization, camera validation/framing/immediate view override/scene mode and revision stability.
Generated protocol/TypeScript checks, 72-tool MCP smoke (including headless camera refusal), default
policy/provenance and whitespace checks passed. No scene/project/asset/shader data was modified.

Desktop visual/GPU verification of the new controls and a live image-based model review remain
pending. Only Radeon Vulkan is installed; the previously documented private-Xvfb DRI3 presentation
limitation still makes isolated GPU checks impractical here. No desktop interaction was performed
or requested for this task. Prior grid fading verification remains resolved.


## Documentation checkpoint — Phase G remains unfinished (2026-09-18)

Recorded the current composer, automatic follow, compact icons/model controls, soft wrapping,
inspection-camera tools and image replies, plus removal of Copy from both message roles. The Copy
removal was rebuilt in development and passed existing headless editor tests; release verification
for that final cosmetic edit was not rerun. Earlier full development/release verification records
apply to the wrapped composer/camera checkpoint, not later untested edits.

Intermittent ChatGPT/App Server service disconnections during multi-minute agent work are an open
reliability issue. No cause, fix or successful long-duration verification is claimed. Retain all
existing edits/project/authentication data and continue Phase G troubleshooting/refinement later.
This checkpoint changes documentation only; no provider request, build or desktop check was run.


## Publication checkpoint (2026-09-18)

The current feature/protocol documentation is synchronized with the Phase G implementation;
earlier entries remain historical. The final Copy-button removal has now also been rebuilt in
release. Development/release builds and all four CTest suites in each build passed, alongside all
13 bridge tests, generated protocol/TypeScript checks, 72-tool headless MCP smoke and whitespace
checks. Credential/authentication/settings/log paths under `.relay` are ignored and untracked.

Phase G remains unfinished: intermittent multi-minute ChatGPT/App Server disconnections, latest
composer/camera desktop verification and live image-based review still require follow-up. This
publication checkpoint does not claim those issues resolved or authorize future desktop access.
