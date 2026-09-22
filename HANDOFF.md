# Relay Engine — engineering handoff

This file records only the state needed to continue development. User-facing material belongs in
[`README.md`](README.md); protocol details belong in [`docs/protocol.md`](docs/protocol.md). Follow
[`AGENTS.md`](AGENTS.md) for working and verification rules.

## Current snapshot

- C++20 engine/editor with SDL3, Dear ImGui, ImGuizmo, Vulkan, and a deterministic CPU renderer.
- External TypeScript agent bridge using Codex App Server and generated MCP tools.
- Protocol schema v19: 90 native methods. Scene v6, project v1, import manifest v3.
- Linux/RADV is the verified graphics path. The project is experimental and pre-1.0.
- HDR rendering, bounded asynchronous uploads, transform keyframes, and portable project export
  are implemented. Preserve unrelated working-tree edits and inspect `git diff` before changing them.

## Product intent

Relay is a shared editor for humans and agents. Anything important that a person can inspect or
change should have a typed, observable control-protocol equivalent. The native engine owns scene
state, validation, authorization, and mutations. Provider integrations stay outside the engine.
Agents should be able to execute long tasks autonomously, inspect rendered results, and iterate
without blocking simultaneous human editing.

## Implemented systems

### Runtime, scene, and editor

- Fixed deterministic timestep; generation-checked entities; hierarchy and reflected components.
- Transactional undo/redo, multi-selection, clipboard workflows, strict loading, migration, and
  atomic saves.
- Folder projects with contained `scenes/`, `assets/`, `captures/`, and traces.
- Dockable hierarchy, inspector, viewport, transform gizmos, asset browser, animation timeline,
  history, diagnostics, and project/save workflows.
- Human editor mutations use the same versioned control protocol as tests and agents.
- Scene-owned transform keys interpolate position, Euler rotation, and scale. The inspector and
  protocol can add, edit, delete, scrub, play, and loop keys with undo/redo. Scene v6 saves keys;
  older scenes load without them. Imported animation channels remain read-only.
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

## Next priorities

1. Expand authoring to additional editable animation tracks and richer export formats.

## Verification baseline

The current implementation was verified with development and release builds, all four native
CTest suites, generated-protocol checks, and 26 ordinary bridge tests. The live Vulkan shadow and
visual smokes pass, as does `tests/editor_hdr_upload_smoke.py`: exposure changes captured pixels,
keyframe scrubbing changes the image, a model import submits another GPU upload, and a package
containing the saved keys and imported asset opens as a tar. The legacy coordinate-based
`editor_interaction_smoke.py` misses targets on this desktop's 2560x1410 layout; its input
assertions are inconclusive until recalibrated. Socket-dependent tests were run with loopback
access after the sandbox refused local socket creation. Headless editor tests cover
the current hierarchy spacing, chat selection/wrapping, attachments, media viewer, composer layout,
usage meters, camera controls, and authorization. A separate sustained fixture has exercised more
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
| Rendering/import | `src/render/`, `shaders/` |
| Editor | `src/editor/`, `include/relay/editor/` |
| Agent bridge | `tools/mcp-bridge/src/` |
| Native tests | `tests/engine_tests.cpp`, `tests/editor_*tests.cpp` |
| Bridge tests | `tools/mcp-bridge/tests/` |

When implementation changes, update this present-state summary instead of appending dated logs.
