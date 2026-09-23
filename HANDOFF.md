# Relay Engine — engineering handoff

This file records only the state needed to continue development. User-facing material belongs in
[`README.md`](README.md); protocol details belong in [`docs/protocol.md`](docs/protocol.md). Follow
[`AGENTS.md`](AGENTS.md) for working and verification rules.

## Current snapshot

- C++20 engine/editor with SDL3, Dear ImGui, ImGuizmo, Vulkan, and a deterministic CPU renderer.
- External TypeScript agent bridge using Codex App Server and generated MCP tools.
- Protocol schema v29: 105 native methods. Scene v11, project v1, import manifest v3.
- Linux/RADV is the verified graphics path. The project is experimental and pre-1.0.
- HDR rendering, bounded asynchronous uploads, transform keyframes, box/sphere/capsule/convex/mesh
  colliders, Jolt body simulation, and portable project export are implemented. Preserve unrelated working-tree
  edits and inspect `git diff` before changing them.

## Product intent

Relay is a shared editor for humans and agents. Anything important that a person can inspect or
change should have a typed, observable control-protocol equivalent. The native engine owns scene
state, validation, authorization, and mutations. Provider integrations stay outside the engine.
Agents should be able to execute long tasks autonomously, inspect rendered results, and iterate
without blocking simultaneous human editing.

## Implemented systems

### Runtime, scene, and editor

- Fixed deterministic timestep; generation-checked entities; hierarchy and reflected components.
- Live editor sessions start in Editor mode, where ticks and frame steps do not advance simulation.
  `runtime.play` snapshots authored scene state and starts a temporary game session; `runtime.stop`
  restores it without changing undo history. During a run, scene/project/asset mutations and trace
  replay are rejected through the control protocol. Pause and step apply only in Game mode. The
  game viewport uses the active scene camera rather than the editor inspection camera.
- Transactional undo/redo, multi-selection, clipboard workflows, strict loading, migration, and
  atomic saves.
- Folder projects with contained `scenes/`, `assets/`, `captures/`, and traces.
- Dockable hierarchy, inspector, viewport, transform gizmos, asset browser, animation timeline,
  history, diagnostics, and project/save workflows.
- `EditorLayout` persists the ImGui dock layout plus a `[Relay][Preferences]` section (panel
  visibility and View toggles bound with `EditorLayout::bind`) in the user config directory
  (`$XDG_CONFIG_HOME/relay-engine`, `%APPDATA%\Relay`, `~/Library/Application Support/Relay`),
  so dev and release builds and any working directory share it. A legacy
  `.relay/editor-layout.ini` is copied once. Saved panels override host defaults such as the
  Agent panel; the old optional-panel docking migration runs only for layouts without the
  preferences section. `RELAY_EDITOR_LAYOUT_PATH` overrides the path (smoke tests use temporary
  files); headless tests use `.relay/headless-layout.ini`, reset per scenario.
  Each dock node's selected tab is restored from the ini. Panels skip focus-on-appearing during
  the first two frames, because a panel focused as it appears becomes its node's selected tab.
- Hierarchy and asset entries rename in place (F2, context menu, or a slow second click on the only
  selected row). Entity creation lives in context menus and the Create menu; the hierarchy has no
  standing text field. The Assets panel is a lazily listed tree over `assets.browse`,
  `assets.create_folder`, `assets.move`, and `assets.delete` (protocol-visible, so agents can do
  the same). Paths are workspace-checked; moves never overwrite and re-point import-manifest
  entries; deletes move into the hidden `.relay-trash`; the project file and member scenes, and
  folders containing them, are protected. Only the root and expanded folders are listed; expanded
  state and the selection follow moves and renames. Entries carry a name-based `kind`
  (`AssetKind` in `project_files.hpp`); `assets.search` walks the whole tree (hidden and symlinked
  entries skipped, 65,536 visited, 512 results) for a case-insensitive name fragment and optional
  kinds. The panel's search box and Filter popup switch it to a flat result list with folder
  paths and removable filter chips. The filter menu keeps itself open while toggling categories.
  **Open in file browser** is an editor-only host action (`show_in_file_browser` in
  `src/editor/file_browser.cpp`: FreeDesktop FileManager1 over `gdbus`, falling back to
  `xdg-open`; `explorer /select` on Windows; `open -R` on macOS), not a protocol method, so agents
  cannot open desktop windows. Headless editors install no handler; tests inject one. The
  platform launch itself has not been exercised on a desktop. Models import by double-click, context
  menu, or drag onto the viewport (ground-plane placement) or a hierarchy row (child). The
  Create menu lists future file types (scripts, text, shaders, materials) as disabled
  placeholders.
- Development builds (`RELAY_OPEN_DEMO_PROJECT`, on in the dev preset) open
  `examples/demo/demo.relayproject` when the UI editor starts from the repository root, unless
  `RELAY_OPEN_DEMO_PROJECT=0`. Smoke tests set that override. `tools/generate_demo_project.py`
  writes `models/primitives.glb` and runs `relay_build_demo_project`, which authors the showcase
  scene through the trusted protocol; commit the regenerated project, including
  `.relay-imports.json`. `relay_workflow_tests` opens the committed demo, resolves every asset,
  builds every collider, and simulates it.
- Importer fixtures live in `tests/fixtures/models/`. Without a project, the protocol still uses
  the ignored scratch folder `assets/`; the desktop smoke launcher copies the fixtures there.
- Human editor mutations use the same versioned control protocol as tests and agents.
- Scene-owned transform keys interpolate position, Euler rotation, and scale. The inspector and
  protocol can add, edit, delete, scrub, play, and loop keys with undo/redo. Scene v6 saves keys;
  older scenes load without them. Imported animation channels remain read-only.
- Scene v7 adds undoable, editable box colliders with local center/half extents, enable flag, and
  32-bit collision layer/mask fields. They save independently of mesh geometry. The `physics.raycast`
  and `physics.overlaps` read-only protocol methods test oriented boxes under entity hierarchy and
  scene-owned transform keys. Queries cap active colliders at 4096 and overlap results at 128.
  Colliders on imported model nodes currently follow the authored entity hierarchy, not imported
  animation node poses.
  The Inspector clamps a zero half extent to a visible 0.01-unit minimum; the protocol and scene
  validator reject zero extents. A headless editor input regression covers the zero-entry path.
- Scene v10 adds authored sphere and capsule collider types, radius, and capsule cylinder half height.
  Older scenes load their colliders as boxes. The Inspector, protocol, Jolt simulation, raycasts,
  overlaps, saves, and undo handle all three shapes. Sphere and capsule use the largest world scale
  axis uniformly.
- Scene v11 adds convex-hull and triangle-mesh collider types plus an optional `mesh` name; empty
  uses the entity renderer mesh. Geometry comes from the engine `AssetRegistry` (bind pose; skin and
  morph deformation are ignored), with the full world scale baked into body-local vertices.
  `scene.set_collider` rejects unregistered meshes; a mesh that later disappears leaves the
  collider inactive. One collider accepts at most 65,536 triangles and one Jolt world build
  1,048,576 mesh triangles; queries over that budget return an error. Jolt hulls keep at most 256
  points. Dynamic bodies use the convex hull of a mesh collider because Jolt cannot simulate
  dynamic triangle meshes. Mesh-versus-mesh overlaps are skipped and mesh overlaps report surface
  crossings, not containment. Mirrored scale flips triangle winding so raycasts hit front faces.
  Collision free functions take an optional registry; `Engine` wires its registry into the
  protocol and `PhysicsWorld`.
- Scene v8 adds undoable static/dynamic physics bodies with mass, gravity scale, and restitution;
  scene v9 adds friction plus linear and angular damping. Existing v8 files migrate with Jolt's
  default material values. The game-only Jolt 5.6 world provides gravity, full rigid-body contact
  response, angular motion, friction, restitution, and linear-cast continuous collision detection.
  Colliders without bodies are static. Linear/angular velocities are runtime state, cleared on Run
  Game and Stop Game, and observable through `physics.body_status`; `physics.apply_impulse` accepts
  an optional world-space point to generate torque. Paused games advance only via frame step.
  Dynamic bodies with authored transform keys are kinematic. Joints remain future work. Jolt
  contact callbacks record ordered entity-pair
  begin/end events in a bounded, sequence-cursor stream exposed by `physics.contact_events`.
  The stream resets on Run Game and Stop Game, and reports the oldest retained sequence for
  detecting missed events.
- The live editor presents with FIFO (vsync) and advances the fixed 60 Hz game step from real
  elapsed time (at most four steps per frame), so frame rate and game speed are independent of
  the monitor. Mailbox presentation with a fixed 16 ms loop sleep previously dropped a frame
  about 2.5 times per second on 60 Hz displays.
- The editor refreshes panels every 0.5 s, spread over four consecutive frames (scene state,
  collider outlines, Agent panel, assets); refreshes right after an edit still run all at once.
  Large periodic replies (`scene.list`, `physics.debug_boxes`, `session.review`,
  `session.audit`) are compared as text and parsed only when changed, and outlines are
  fetched only while the overlay can be drawn. Convex hull outlines are built once per mesh in
  collider-local space and transformed per call. On the demo scene this took Debug-build refresh
  frames from about 33 ms to under 3 ms headlessly, with the Agent panel open.
- The editor camera overlays world-space collider outlines from the same shape computation as
  physics queries: boxes, three great circles for spheres, rings and arcs for capsules, Jolt hull
  edges for convex colliders, and unique triangle edges for meshes. Selected colliders are amber,
  enabled green, disabled gray. The View menu toggles them. The host-only read-only
  `physics.debug_boxes` method caps output at 4096 colliders, 768 lines per collider, and 8192
  lines per call; a truncated outline also draws its bounding box.
- The editor viewport overlays clickable camera and directional/point/spot light icons, with type
  labels on hover. Selected cameras show a compact perspective or orthographic view guide; its
  display depth is capped independently of the authored far clipping plane. Markers follow
  scene-owned transform keys and parent transforms, can be toggled in View, and are hidden during
  Run Game.
- `project.package` writes a bounded uncompressed tar under `exports/` containing normalized
  project metadata, member scenes, and non-hidden project assets, including import metadata.
  It excludes private state, captures, traces, and prior exports, and refuses overwrites. The
  project panel exposes the same operation. Save scene changes before packaging.

### Rendering and assets

- Deterministic CPU renderer for headless verification.
- Vulkan scene geometry and transparent blending render into a per-swapchain-image RGBA16F target.
  A fullscreen post-process pass applies camera exposure in EV stops, ACES-fit tone mapping and
  correct SDR transfer encoding before editor UI or capture. The PBR shader uses an analytic
  sky/ground hemisphere for diffuse and roughness-aware specular environment lighting. Camera
  exposure is serialized, undoable and available through the typed protocol and inspector.
- Vulkan presentation, resize/swapchain handling, selection outlines, grid, PBR materials (including
  masked and correctly ordered alpha-blended geometry), cameras, punctual lights, animation,
  skinning, morph targets, synchronized capture, and bounded punctual shadows. The first GPU-visible
  directional light receives three stable, camera-fitted cascades (2048/1024/1024) out to 120
  units; the brightest spot light receives a perspective 1024-square map and the brightest point
  light receives a six-face 1024-square cubemap. The one-per-type budget is deterministic. All use
  3x3 PCF, with cascade blending/fade for directional light,
  slope plus receiver bias, alpha-mask and double-sided casting, off-camera caster retention,
  per-cascade culling, depth-format fallback, and explicit unshadowed behavior.
- Asset revisions batch all mesh, material, texture, and mip-generation transfers into one command
  submission without blocking frame production. Queue ordering makes the replacement visible to
  later draws; staging allocations and the previous complete resource set retire only after the
  upload fence signals. Mid-upload registry changes coalesce into the next batch, and setup failures
  restore the previous set.
- Vertex, index, and material buffers reserve geometric headroom. Append revisions with an unchanged
  texture table upload only new geometry/material ranges in place. Replacement batches reuse all
  unchanged texture images and upload only appended images while atomically replacing descriptors;
  capacity overflow remains non-blocking.
- Uploads preflight a 512 MiB staging budget, 2 GiB estimated resident device budget, and
  256 MiB per-texture mip-chain budget. `render.upload_status` reports staging peaks, latest
  batch size, estimated residency, rejects, and whether a transfer-only queue is in use. When a
  transfer-only queue exists, buffer copies and texture mip blits run there. A semaphore makes the
  new resources visible to graphics without blocking the CPU; old resources retire only after
  both the upload and preceding graphics frames complete. Other devices use the graphics queue.
- Asynchronous PNG capture and WebM recording with bounded queues and explicit provenance.
- Native glTF/GLB path; Assimp for OBJ/FBX/DAE; Blender-to-glTF conversion on Linux through a
  Bubblewrap sandbox. Imports are dependency-bounded, content-addressed, and recorded in a manifest.

### Protocol and authorization

- `protocol/relay.protocol.json` is the source of truth. `tools/generate_protocol.py` produces the
  C++, TypeScript, and protocol reference; builds reject stale generated output.
- New sessions default to no scoped grants. Native authorization covers method, entity, and file
  scopes. Requests, decisions, execution outcomes, and revocation are audited.
- **Allow all actions** is a human-controlled editor setting and currently defaults on. Input,
  containment, import, and format validation remain active.
- Socket mode is loopback-only and requires a host-provided token. Owned stdio children use private
  pipes and need no socket handshake.

### Embedded agent workspace

- ChatGPT device authentication, discovered models/reasoning levels, compatible API support, and
  private persisted provider state under ignored `.relay/` paths.
- Streaming chat with automatic follow, selectable history, wrapped multiline input, stop, file
  picker and drag/drop attachments, inline images/videos, and a zoomable large-media viewer.
- Five-hour and weekly usage pills sit below the rounded composer and use grey/yellow/red thresholds.
- Inspection-camera tools let the agent frame entities and adjust the view without changing scene,
  selection, undo, or deterministic capture state. PNG captures are returned as model image input.
- Shared model instructions explain Relay geometry, coordinate and rotation conventions, concurrent
  human edits, entity reuse, and the capture/inspect/correct workflow.

### Long-task behavior

- App Server threads persist under `.relay/openai`; bridge restarts can resume the original thread.
- Native mutation calls are deduplicated by stable call ID across lost replies and reconnects.
- Recovery waits for in-flight work, inspects current state, supplies a bounded completed-action
  checkpoint, and continues remaining work. Stop cancels retry backoff; project switches pause work.
- RPC acknowledgements allow 120 seconds. Turns have no overall time or tool-count deadline.
  Completion polling reconciles missing terminal notifications every 30 seconds.
- Three recoveries without tool progress stop safely. Account and usage failures are not retried.
  Recovery does not survive a native-editor or bridge-process crash.

## Important invariants

- Keep `relay_engine` free of UI and model-provider dependencies.
- Route editor, agent, and test mutations through the control protocol.
- Keep secrets and canonical conversations outside scene/project files, traces, audits, and model
  arguments. `.relay/` remains private and ignored.
- Scene/control filenames cannot escape their project directories. Retain symlink, size, count,
  and canonical-path checks.
- Vulkan requests must fail explicitly when Vulkan is unavailable; never silently substitute CPU
  output. Record capture source in status and results.
- Preserve deterministic tests and captures while extending live rendering.
- Do not hand-edit generated protocol artifacts. Change the schema and regenerate them.

## Known gaps

1. Multi-minute live OpenAI sessions have previously disconnected. Mock-provider and injected-fault
   tests validate recovery, but the original live-provider cause has not been established.
2. The newest composer, media, and inspection-camera work has strong headless coverage but still
   needs deliberate live-provider and desktop visual validation.
3. Untrusted `.blend` conversion fails closed on Windows and macOS because equivalent OS sandboxes
   are not implemented. `RELAY_BLENDER_TRUSTED=1` is only for administrator-approved input.
4. Rendering remains early: image-based environment maps, scalable resource streaming,
   configurable shadow-quality controls, Direct3D 12, and Metal are not implemented. The HDR
   path now has live desktop capture coverage. The tested GPU exposes no dedicated transfer-only
   queue, so that queue branch still has compile and headless checks only.
5. Agent sessions are designed for one local human/editor workflow; multi-client identities and a
   tamper-proof continuous audit store are not implemented.
6. The hierarchy and asset browser changes are covered headlessly only. The coordinate-based
   desktop smokes (`TREE_FIRST_ROW_Y`, `ASSET_FIRST_ROW`) predate the removed create and import
   rows and need recalibration. The demo scene has not had a real-desktop visual review.

## Next priorities

1. Add gameplay scripting with a lifecycle tied to Run Game and Stop Game, including access to
   contact events.
2. Add joints to the Jolt backend.

## Verification baseline

The current implementation was verified with development and release builds, all four native
CTest suites, generated-protocol checks, and 26 ordinary bridge tests. The desktop smokes below
predate the asset browser, layout persistence, and frame-pacing changes and were not rerun for
them; those changes are covered by headless and protocol tests. The live Vulkan shadow and
visual smokes pass, as does `tests/editor_hdr_upload_smoke.py`: exposure changes captured pixels,
keyframe scrubbing changes the image, a model import submits another GPU upload, and a package
containing the saved keys and imported asset opens as a tar. The legacy coordinate-based
`editor_interaction_smoke.py` misses targets on this desktop's 2560x1410 layout; its input
assertions are inconclusive until recalibrated. Socket-dependent tests were run with loopback
access after the sandbox refused local socket creation. Headless editor tests cover
the current hierarchy spacing, chat selection/wrapping, attachments, media viewer, composer layout,
usage meters, camera controls, authorization, collider zero-extent input, and round collider
outlines. Convex and mesh colliders have headless physics coverage but no real-desktop visual check. A separate sustained fixture has exercised more
than two minutes of editing/capture work with injected disconnects. The Vulkan directional-shadow
path also has real-desktop coverage that compares captured receiver pixels with and without an
occluder for cascaded directional, perspective spot, and cubemap point shadows; the general visual
smoke covers capture isolation and swapchain resizing with all shadow resources active. These
results do not prove live-provider stability or the newest Agent-panel presentation quality.

Run native suites sequentially because some fixtures share temporary import paths:

```sh
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --preset release -DRELAY_BUILD_TESTS=ON
cmake --build --preset release
ctest --test-dir build/release --output-on-failure
npm --prefix tools/mcp-bridge run check
npm --prefix tools/mcp-bridge test
git diff --check
```

Opt-in sustained bridge test:

```sh
RELAY_SUSTAINED_TEST_MS=130000 node --test --test-isolation=none tools/mcp-bridge/tests/sustained.test.mjs
```

## File map

| Area | Location |
| --- | --- |
| Protocol source | `protocol/relay.protocol.json` |
| Native protocol/session | `src/control/`, `include/relay/control/` |
| Engine and scene | `src/core/`, `src/scene/` |
| Demo project and generator | `examples/demo/`, `tools/generate_demo_project.py`, `tools/demo_project/` |
| Test fixture models | `tests/fixtures/models/` |
| Physics and collision | `src/physics/`, `include/relay/physics/` |
| Rendering/import | `src/render/`, `shaders/` |
| Editor | `src/editor/`, `include/relay/editor/` |
| Agent bridge | `tools/mcp-bridge/src/` |
| Native tests | `tests/engine_tests.cpp`, `tests/editor_*tests.cpp` |
| Bridge tests | `tools/mcp-bridge/tests/` |

When implementation changes, update this present-state summary instead of appending dated logs.
