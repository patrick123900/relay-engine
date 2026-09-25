# Relay Engine — engineering handoff

This file records only the state needed to continue development. User-facing material belongs in
[`README.md`](README.md); protocol details belong in [`docs/protocol.md`](docs/protocol.md). Follow
[`AGENTS.md`](AGENTS.md) for working and verification rules.

## Current snapshot

- C++20 engine/editor with SDL3, Dear ImGui, ImGuizmo, Vulkan, and a deterministic CPU renderer.
- External TypeScript agent bridge using Codex App Server and generated MCP tools.
- Protocol schema v39: 129 native methods. Scene v17, project v2, import manifest v3.
- Linux/RADV is the verified graphics path. The project is experimental and pre-1.0.
- HDR rendering, bounded asynchronous uploads, transform keyframes, box/sphere/capsule/convex/mesh
  colliders, Jolt body simulation with fixed/point/hinge/slider/distance joints, a Unity-style
  component model with derived node types and templates/prefabs shown in the node type tree, native
  C++ gameplay scripts, per-project input mapping, a scripted first person controller template in
  the demo project, portable project export, and FidelityFX global illumination and hardware ray
  traced reflections are implemented. Preserve unrelated working-tree
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
  kinds. Listings and searches leave out `.relayproject` files (`hidden_from_assets`), and the
  `project` kind is gone (protocol v38). The panel's search box and Filter popup switch it to a
  flat result list with folder paths and removable filter chips. The filter menu keeps itself
  open while toggling categories. The chevron button left of the filter collapses every folder,
  or when none is open lists and opens all of them breadth first (`expand_all_asset_folders`,
  at most 256 folders).
- The Hierarchy has the same search bar: a case-insensitive name search and a node type filter
  (from `nodes.types`, listed as a tree; a category matches its subtypes via `type_within`)
  switch it to a flat list of matching nodes with their parent path and type. Double-click or
  **Show in hierarchy** clears the search, opens the node's ancestors (`hierarchy_reveal`),
  scrolls to it and selects it. Ctrl+F focuses the search. Its chevron button applies
  `SetNextItemOpen` to every row with children for one frame (`hierarchy_open_all`); whether any
  row is open is recorded while drawing and decides whether the button collapses or expands.
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
- Scene v14 adds `lock_rotation` to physics bodies: dynamic bodies get Jolt translation-only
  `mAllowedDOFs`, so contacts never rotate them (Inspector "Lock rotation", protocol
  `lock_rotation`).
- Scene v8 adds undoable static/dynamic physics bodies with mass, gravity scale, and restitution;
  scene v9 adds friction plus linear and angular damping. Existing v8 files migrate with Jolt's
  default material values. The game-only Jolt 5.6 world provides gravity, full rigid-body contact
  response, angular motion, friction, restitution, and linear-cast continuous collision detection.
  Colliders without bodies are static. Linear/angular velocities are runtime state, cleared on Run
  Game and Stop Game, and observable through `physics.body_status`; `physics.apply_impulse` accepts
  an optional world-space point to generate torque. Paused games advance only via frame step.
  Dynamic bodies with authored transform keys are kinematic. Jolt contact callbacks record
  ordered entity-pair
  begin/end events in a bounded, sequence-cursor stream exposed by `physics.contact_events`.
  The stream resets on Run Game and Stop Game, and reports the oldest retained sequence for
  detecting missed events.
- Scene v17 adds an optional `joint` component (`Joint` in `scene.hpp`, component id `joint`,
  protocol `scene.set_joint`): type fixed/point/hinge/slider/distance, `connected` entity
  (invalid = world), local `anchor` and `axis`, `connected_anchor` (distance joints; the
  partner's local space, or world space for the world), limits (hinge degrees with min in
  [-180, 0] and max in [0, 180], slider metres with min <= 0 <= max, distance lengths
  0 <= min <= max; a distance joint without limits keeps its starting length), hinge/slider motor
  speed and force, distance spring frequency/damping, `collide_connected` and `enabled`.
  Changing type through the protocol resets limits to that type's defaults. `Scene::destroy`
  disables joints whose partner went and clears the partner; `Scene::duplicate` keeps outside
  partners and remaps inside ones; `copy_selection` disables joints to nodes outside the copy
  (handles do not identify scenes) and `paste_selection` remaps the rest. Loading checks each
  partner exists and differs from its node.
- `JoltState::add_joint` builds Jolt constraints in world space from the authored transforms at
  Run Game (after all bodies), and for spawned trees in `add_bodies`. The partner is Jolt body 1
  (`Body::sFixedToWorld` for the world) and the joint's node body 2, so hinge angles, slider
  travel and positive motor speeds follow the right-hand rule about the node's axis. Joints need
  a body on each end (a collider or physics body) and at least one non-static body; otherwise
  they are skipped. Joined pairs without `collide_connected` are rejected in
  `OnContactValidate`. `remove_missing_bodies` removes constraints before their bodies and wakes
  the bodies they held. `PhysicsWorld::joint_position` and `set_joint_motor` back the script
  functions `Entity::joint_position`, `set_joint_motor` and `stop_joint_motor`.
  `physics.debug_boxes` also returns `joints` (world anchor, axis and partner point), which the
  editor draws with the collider wireframes (cross, axis, line to the partner). The Inspector's
  Joint section edits every field, lists bodies to connect to, clamps limits to what the type
  accepts, and warns when neither body is dynamic.
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

### Components, node types and templates

- A node is a Transform plus optional components. Engine components keep typed storage in
  `EntityRecord` (`std::optional` per kind, for speed); scripts are `std::vector<Script>`, each
  with a behaviour name, enabled flag and property overrides (`ScriptProperty`: boolean, number,
  vector or text). `src/scene/components.cpp` is the catalog (`engine_components()`: id, name,
  category, addable, removable, multiple) and the add/remove rules shared by protocol and editor.
  The Transform and imported model animation (`animator`, which model-node children depend on)
  cannot be removed. A camera added to a scene without an active camera becomes active.
- Node types are a tree in `src/scene/node_types.cpp`: Node > Model, PhysicsBody
  (> RigidBody, StaticBody), Camera, Light (> DirectionalLight, PointLight, SpotLight), StaticMesh. Each type
  adds components to its parent's; `apply_node_type` applies them root first when
  `scene.create` receives a `type`. Light, PhysicsBody and Model are not creatable (Model comes
  from import). `node_type()` walks down the tree taking the first child whose own additions the
  node has, so sibling order is precedence (a lit rigid body is a RigidBody); scripts do not
  affect it. `scene.list`/`scene.inspect` report it; scene files do not store it
  (`Scene::list_json(false)`). `nodes.types` returns the tree with inherited component lists.
- `component.types/add/remove` are the generic protocol; each engine component keeps its own
  setter (`scene.set_camera` and so on). Script components are edited by index with
  `scene.set_script` and `scene.set_script_property`; changing a component's behaviour clears its
  overrides. Scene v13 stores `scripts` arrays; v12's single optional `script` migrates to one
  component.
- There is no native first person controller. Scene v15 briefly had one (a
  `first_person_controller` component, `FirstPersonControllers` in `src/core`, protocol
  `scene.set_first_person_controller`); v16 dropped it, and loading a v15 file turns a controller
  into a `FirstPersonController` script component with the same settings as property overrides
  (`camera` becomes `camera_name`). The demo project provides that behaviour in
  `examples/demo/scripts/FirstPersonController.cpp` and a "First Person Controller" template: a
  0.35 m radius capsule (70 kg, no friction or damping, rotation locked) 1 m up, with a child
  "Camera" 0.7 m above it (75 degree field of view, inactive until the script's `on_start`
  activates it). The script turns the camera for look and sets the body's horizontal velocity,
  jumping only when a raycast that ignores the body finds ground. On `fire` it instantiates the
  "Ball" template 0.5 m along the view from the camera with velocity `ball_speed` (20 m/s) along
  the view plus the body's velocity. Ball is a 0.3-scale gold sphere, a dynamic 0.5 kg body on
  collider layer 2 with `scripts/Projectile.cpp` (destroys it after `lifetime`, 6 s); the
  player's collider mask is 0xFFFFFFFD, so its own balls pass through it. The demo's input map
  sets `lock_mouse`. `tools/demo_project/build_demo_project.cpp` builds the player through the
  protocol, saves it as the template at the origin, and leaves it in the showcase at (0, 1, 8)
  facing -Z, with its camera as the showcase's only and active camera (the template's copy stays
  inactive). The editor viewport renders from its own inspection camera either way. Run Game
  needs the project trusted and its scripts built. The Ball template is built after the showcase
  is saved.
- `src/scene/templates.cpp`: project templates are node trees saved as
  `templates/<name>.relay-template.json` in the ordinary scene format with exactly one root,
  so loading reuses scene validation and migration. Instantiation copies through
  `copy_selection`/`paste_selection` in one undoable transaction, so copies are independent and
  pasted cameras stay inactive. Saving and instantiating keep the root's own name rather than
  paste's "<name> Copy". There is no live prefab link or override tracking. `templates.list`
  reports each template's root node type (`node_type()` of the saved root), its engine component
  ids and its script behaviours.
- The Inspector draws only present components, with a close button and Remove context item on
  removable headers, and script components as "<Behaviour> (Script)" sections with typed property
  editors and per-property Reset. **+ Add Component** opens a modal window: category list,
  component list (present ones disabled and tagged "Added"), description of the selection,
  search, "New C++ script..." (creates the file and attaches it), Add/Enter and double-click.
  **+ Add Node** under the Hierarchy (and the Scene and Hierarchy context menus) opens the Add
  Node window: one tree of node types (categories dimmed) and custom templates, each template a
  leaf under the type its root inherits, after that type's subtypes. A painter's-palette icon
  drawn next to a template's name (and in its details heading) shows a "Custom template"
  tooltip on hover. Searching lists matching types and templates flat. The details pane shows
  the inheritance chain, description and components (for templates, the root's components and
  scripts), then an optional name and "Add as a child of" the selection. The Hierarchy shows the derived type on each row; template files instantiate on
  double-click, or when dragged onto the viewport (ground point) or a row (child).

### Input

- `InputState` (`src/core/input.cpp`, owned by `Engine`) keeps live control state from platform
  events and latches it in `begin_step()` at the start of each fixed game step: per control
  held/pressed/released, a still-down flag so an action with two bindings is not released while
  one stays down, analog values for sticks and triggers (triggers and sticks read as buttons past
  half travel), and per-step mouse movement and wheel. Taps shorter than a step still read as one
  press. `clear_edges()` runs at Run Game.
- Controls are `key:<name>` (SDL scancode names, so physical positions), `mouse:<left|middle|
  right|x1|x2>` and `gamepad:<a|b|x|y|...|leftx|...|left_trigger|right_trigger>`. Platform
  events are produced by `src/platform/sdl_input.cpp` for the Vulkan and CPU windows; it also
  opens gamepads on connect, without which SDL sent no gamepad events at all (previously the
  case). Traces from before this change recorded layout-dependent keycodes (`key:down:32`),
  which no binding matches.
- `InputMap` holds actions (any bound control) and axes (button pairs or scaled analog bindings,
  strongest wins, deadzone rescaled), plus `lock_mouse`. Project files v2 store it as
  `settings.input` (the `relay.input` v1 document, embedded as-is); `Project::input` is empty until
  someone saves a map, and the engine defaults apply meanwhile
  (move_x/move_y/look_x/look_y, jump, interact, fire, sprint). `load_project` still reads v1
  files; a v1 project's `input.relay-input.json` is read into `Project::input` with
  `legacy_input_file` set, and `save_project` removes that file after writing the map into the
  project file. `Engine::set_input_map` saves the project; `sync_input_map` applies the open
  project's map when the project changes. Protocol replies (`project.*`) leave the settings out;
  the file, and so project export, keeps them. Project files may be up to 512 KiB.
- Protocol: `input.map`, `input.set_map` (validated, saved, applied), `input.state`,
  `input.simulate` (Run Game only; holds an action or sets an axis for N steps, taking precedence
  over devices), host-only `input.release` (sends `input:reset`, which traces record), and the
  older `input.recent` event log. Scripts use the appended `RelayHostApi` input functions through
  `relay::input` in the SDK. `RelayHostApi` also gained `child` (first direct child by name) and
  `activate_camera` (a game-time camera switch that Stop Game undoes with the rest of the scene).
- Editor: during Run Game the game gets keyboard and mouse only after a click on the viewport;
  Escape or losing window focus returns input and releases held controls, and a viewport label
  says which state applies. While the game has input, events bypass ImGui entirely, and editor
  camera navigation stands aside and keeps the pointer locked when the map asks for it (it used
  to release the lock every frame it was not flying the editor camera, so the cursor stayed
  free). `EditorUi::game_has_input` and `pointer_locked_for_game` expose the state for tests. **Edit → Game
  Configuration...** is a page-based window (Input now; Graphics, Physics and Audio listed as
  upcoming). The Input page edits a local copy with the engine's own parser and serializer and
  saves every change through `input.set_map`: action and axis tables with renamable names,
  wrapping binding chips, press-to-bind capture (`sdl_binding_control`; mouse clicks bind only
  inside the prompt, clicking elsewhere cancels), key-pair and stick bindings, invert, deadzone,
  live values during Run Game, mouse lock and Reset to defaults.

### Gameplay scripting

- Scripts are C++20 under a project's `scripts/` folder, written against the single SDK header
  `sdk/relay_script.hpp`. Engine and script library share only the versioned C table in
  `sdk/relay_script_abi.h` (`RelayHostApi` in, `RelayScriptModule` out, entry
  `relay_script_module_v1`), so scripts never link against engine internals and C++ exceptions
  never cross the boundary. `RELAY_BEHAVIOUR(Class)` registers a behaviour by class name.
- `ScriptSystem` (`src/script/script_system.cpp`, owned by `Engine`) compiles on a background
  thread with the compiler Relay was built with (`RELAY_SCRIPT_COMPILER` overrides it), up to eight
  files in parallel, through `run_process` (`src/core/process.cpp`, shared with the Blender
  adapter). Object files are keyed by toolchain, SDK, all project headers, path and content, so a
  rebuild only compiles changed files. Output goes to `.relay-cache/scripts/` inside the project
  (hidden, so browsing and export skip it). Each library has a content-derived name, so `dlopen`
  never returns a stale mapping; superseded libraries are deleted after a successful load.
  Compiler output is parsed into file/line diagnostics. Limits: 256 files, 1 MiB each, 120 s per
  compile, 64 KiB of output.
- Behaviours declare properties in `properties(relay::Properties&)`. The SDK reads names, types
  and code defaults once from a fresh instance per behaviour (`property_count`/`property_info`),
  and the host assigns a node's stored values through `set_property` after construction and
  before `on_start`, and again after hot reload. Values whose field was renamed or retyped are
  skipped with a log warning. Instances are per script component; contact callbacks reach every
  script on both entities.
- Script components are undoable scene data (see above). `runtime.play` refuses
  when an enabled script cannot run as authored: no project, untrusted, building, failed build,
  sources changed since the build, or an undefined behaviour. The editor responds to those
  refusals by asking for trust or building first, so the engine stays the only source of truth.
- Frame order: script `on_update`, animation, physics, then contact callbacks from the
  `physics.contact_events` cursor, delivered to both entities. `on_start` runs after every instance
  is created; Stop Game calls `on_stop` before restoring the scene. A build that finishes during
  Run Game destroys instances while the old code is still mapped, recreates them from the new
  library, and calls `on_reload` (default `on_start`). A thrown exception disables that one
  instance and is recorded (bounded to 64) with frame, entity, behaviour and callback.
- Script transform writes teleport the Jolt bodies of the entity and its descendants
  (`PhysicsWorld::sync_transforms`). Script raycasts use `PhysicsWorld::raycast` against the
  running world instead of rebuilding one per query, and can ignore the caller's own collider.
- Scripts change the scene's structure during Run Game through host functions appended to
  `RelayHostApi` (ABI version unchanged; old libraries simply lack them): `create_entity`,
  `instantiate` (a project template, optionally positioned and parented), `clone`, `destroy`,
  `children`, `overlaps` and `overlap_sphere`, plus the `RELAY_CALLBACK_DESTROY` callback
  (`on_destroy`). Spawns take effect at once: `finish_spawn` gives the tree Jolt bodies
  (`PhysicsWorld::add_bodies`) and script instances, which start (`start_pending`) before their
  first update, at the start of `update` or `dispatch_contacts`. Destruction is queued and
  applied by `flush_destroyed` after `start`, `update`, `dispatch_contacts` and hot reload:
  `on_destroy` for each doomed tree's started instances, then `Scene::destroy`, instance removal
  and `PhysicsWorld::remove_missing_bodies`, which ends the removed bodies' contact pairs at once.
  Instances are `unique_ptr`s and every loop that can spawn is index-based, and `call`/`create`
  restore the caller's `active` instance, so callbacks may nest. `templates.cpp` splits
  `load_template` from `place_template` so a run caches each template (misses warn once).
  Spawning is refused past 100000 entities and during `on_stop`. `PhysicsWorld::overlaps` counts
  colliders within Jolt's speculative contact distance, so resting neighbours are touching;
  `overlap_sphere` is exact and filters on the query mask only.
- Trust is per user, outside every project: `trusted-script-projects` in the user config
  directory (`RELAY_SCRIPT_TRUST_PATH` overrides it; tests use temporary files), keyed by the
  canonical project folder. `scripts.trust` is host-only; revoking trust unloads the library.
  Agents can read and write sources (`scripts.read`/`write` are file-scoped on `path`) but cannot
  make them run in an untrusted project.

### Rendering and assets

- Deterministic CPU renderer for headless verification.
- The Vulkan scene renders into viewport-sized targets, one set per frame in flight, recreated
  (after `vkDeviceWaitIdle`) when the editor viewport changes size; the other set is the previous
  frame for temporal effects. The geometry pass draws opaque and masked geometry into six color
  attachments: HDR, world normal plus perceptual roughness (RGBA16F), base color plus metallic
  (RGBA8 sRGB), motion (previous minus current UV) plus occlusion (RGBA16F), depth as R32F, and
  direct diffuse light plus emission (RGBA16F, fed back to GI next frame). The forward pass then
  loads HDR and depth-stencil for the lighting composite, transparent geometry, grid and
  selection. The tone pass samples the scene image at the viewport's offset. Motion vectors come
  from each draw's previous model-view-projection in a per-frame storage buffer (set 0, binding
  5), keyed by entity, mesh and occurrence; draws use their draw index as first instance. Devices
  need six color attachments. `build_render_scene` can keep culled instances (lighting uses the
  whole scene) and reports camera view and projection separately.
- `shaders/surface_lighting.glsl` holds material evaluation, shadowed direct light and the
  analytic sky, shared by `first_light.frag` and the reflection hit shader.
- Lighting effects are per-project `settings.graphics` (`GraphicsSettings`: `global_illumination`,
  `reflections`, both default on; `graphics.settings` also reports the live renderer's status
  through the host inspection handler, kind `lighting`). The host applies them each frame with
  `VulkanWindow::set_global_illumination` and `set_reflections`; `RELAY_GLOBAL_ILLUMINATION` and
  `RELAY_REFLECTIONS` (0 or 1) override them in `relay_demo`. The editor's Game Configuration has
  a Graphics page. Effects start lazily, need Vulkan 1.3 features checked in
  `create_logical_device` (`lighting_features`, `ray_query_features`), and on failure record an
  error, turn themselves off and leave the analytic sky light. `lighting_status_json()` reports
  requested, supported, active and errors.
- Global illumination (`src/platform/vulkan_lighting.cpp`, `GlobalIllumination`): FidelityFX
  Brixelizer builds a sparse distance field (8 cascades, 0.1 m voxels doubling, centred on the
  camera) from the shared mesh buffers, registered by handle and size. Instances unchanged for 30
  frames are static; moved or deformed ones are dynamic and submitted every frame. Brixelizer
  rebuilds one cascade per update: cascade 0 every 2 frames, each further one half as often.
  Brixelizer GI runs at native resolution (at 50%, moving objects left streaks along their path:
  the downsample gives outline texels the object's depth, and history drags them along). It reads depth, normals, motion, the previous frame's G-buffer and
  diffuse light, blue noise, and the sky as a 16-pixel cube at 1/pi (the analytic ambient's scale)
  and writes diffuse and specular GI. FidelityFX takes Direct3D clip space, so the projection's
  y is flipped for it. The lighting buffer's `camera_forward.w` flags (1 GI, 2 reflections) tell
  the geometry pass which analytic sky terms to leave out; `shaders/gi_composite.frag` adds
  diffuse GI times base color, and specular light weighted by an analytic split-sum BRDF.
- Ray traced reflections (`Reflections` in `vulkan_lighting.cpp`, `vulkan_ray_tracing.cpp`,
  `shaders/reflection_*.comp`), following AMD's Hybrid Reflections sample without its
  screen-space path: acceleration structures are rebuilt each frame (a cached bottom level per
  mesh, per-frame ones for deformed instances, a top level per frame slot, each in its own
  allocation); the FidelityFX Classifier lists pixels below a GGX alpha of 0.25; a compute pass
  turns its counters into indirect arguments; `reflection_trace.comp` samples a GGX visible
  normal with blue noise shifted by R2 each frame, traces a ray query, and shades hits with
  `surface_lighting.glsl` (misses see the sky at 1/pi); the FidelityFX reflection denoiser
  filters the result, reading R32F depth as mip 0 of its depth hierarchy. The composite blends
  from the rough specular (GI or the analytic sky) to traced reflections over the last fifth
  of the threshold. Materials are treated as opaque by rays.
- `relay_demo --vulkan-scene-capture <project> <png> [frames]` opens a project and captures it;
  with `SDL_VIDEODRIVER=offscreen` (Mesa's `VK_EXT_headless_surface`) the real renderer runs
  without a window, which is how the lighting was verified and how
  `tests/lighting_render_tests.py` runs.
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
- Never build or load project scripts without the user's per-project trust, and never let agents
  grant it. Keep the script ABI a plain C table: bump `RELAY_SCRIPT_ABI_VERSION` and the entry
  symbol for incompatible changes, and append new host functions at the end of `RelayHostApi`.

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

7. Templates copy; they have no live prefab link, nested template references or per-instance
   override tracking. Built-in meshes are only a flat triangle and quad, so Static Mesh nodes start
   with a quad until a cube primitive exists; physics body types carry no mesh (as in Godot). The
   Add Component window, script property editors and template dialog are covered headlessly or by
   protocol tests, not by a desktop review. The Add Node window's template tree and palette icon
   have been seen on a Linux desktop.
8. The demo's first person controller is a dynamic rigid body driven by velocity from a script,
   not a kinematic character controller: it does not step up stairs, has no slope limit, crouch
   or coyote time, and pushes other dynamic bodies with its full 70 kg. Jolt's CharacterVirtual
   would handle those if exposed to scripts. It reads move_x, move_y, look_x, look_y, jump and
   sprint, and shoots on fire. It has simulated-input coverage through the compiled script
   (landing upright, walking, mouse turn, sprint, grounded-only jump, shooting, camera switch and
   restore) and has been play-tested with mouse and keyboard on a Linux desktop, including
   shooting balls. The first viewport click, which gives the game input, also counts as fire.
9. Input has headless, protocol and compiled-script coverage, and viewport focus and mouse lock
   with real mouse and keyboard events were exercised by the desktop play-test of the demo
   controller. Gamepad input has not been tried with hardware, and press-to-bind capture from
   real SDL events has not been checked on a desktop. Multiple gamepads are merged rather than assigned to
   players. There is no text input or on-screen cursor API for scripts yet.
10. Native scripts cannot be contained: a crash or endless loop in a script takes the editor down,
   and in a trusted project agent-written code runs with the user's privileges. Windows script
   loading is not implemented (builds report unsupported), and the macOS `.dylib` path has never
   been run. Scripts cannot add or remove components or reparent entities yet. A script-driven scale
   change does not rebuild collider shapes until the next Run Game. The trust prompt and Scripts
   diagnostics view are covered headlessly only.
11. Joints have headless and simulated coverage, not a desktop review of the Inspector section,
   the wireframe gizmos or the demo playground. Anchors and axes are captured when Run Game
   starts, so moving a jointed body with a script teleports it against its constraint. There are
   no breakable joints, cone or swing-twist limits, or target-angle servos, and anchors are edited
   as numbers rather than with a viewport gizmo.
12. Global illumination and reflections have been checked in offscreen captures of the demo and
   its editor viewport (static views, a moving object, animated skinned models), plus the
   automated offscreen render test. Under the Khronos validation layer, core and synchronization
   validation report no errors in any combination of the two, in scene captures, the model and
   async smokes, the editor and the headless effect test. The only warning is
   `Undefined-Value-ShaderOutputNotConsumed`: the transparent pipeline shares the geometry pass
   shaders and ignores their extra outputs. They have been checked on a Linux desktop, and have
   only run on RADV (RX 9070 XT). Known limits: Brixelizer GI
   learns radiance from the screen, so light from off-screen surfaces arrives through its cache
   only; GI around a moving object lags it slightly, since the coarse cascades and caches
   update less often; a small dark fleck was seen once on the demo
   floor and did not reproduce. Reflection hits use the analytic sky instead of GI, masked
   materials trace as opaque, transparent objects are not in the ray tracing scene, rough
   metals near the threshold keep some denoiser blotching, and each acceleration structure is a
   separate allocation (no sub-allocation yet). The FidelityFX build is only tested on Linux
   with GCC; the Windows build of the vendored SDK has not been tried. The Graphics page has
   headless coverage only.

## Next priorities

1. Extend the script API further where games need it: adding and configuring components,
   reparenting, and shape casts. More example controllers (third-person, orbit) can follow the
   demo's scripted first person controller.
2. Grow joints where games need them: breakable joints, cone/swing-twist limits for ragdolls,
   hinge target angles (servo motors), and editing joint anchors with a viewport gizmo.

Deferred:

- Load scripts on Windows (`LoadLibraryW`, `.dll`, MSVC/clang-cl and MinGW compiler flags). The
  stubs are in `src/script/script_system.cpp` (`load`, `build`, `status`). It can be developed on
  Linux: MinGW and Wine can run the script tests end to end, but the MSVC/clang-cl flags need the
  MSVC CRT and Windows SDK (for example through `xwin`) or a Windows machine to verify.

## Verification baseline

The current implementation was verified with development and release builds, all seven native
CTest suites (including `relay_fidelityfx_tests`, which creates every FidelityFX effect on a
headless Vulkan device, and `relay_lighting_render_tests`, which renders the demo offscreen with
each combination of global illumination and reflections and checks their status and pixels;
`relay_script_tests`, which compiles real scripts with the configured compiler,
including property overrides of every type, scripts reading simulated and raw input, a play-test of
the demo's First Person Controller template and script (including shooting balls along the view and
past the player), and spawning: templates at a position under a parent, a missing template warned
once, clones, empty nodes, children, deferred start and self-destruction with `on_destroy`, spawned
bodies falling, `overlaps` on a resting body, `overlap_sphere` with an ignored entity, a destroyed
body leaving raycasts and ending its contacts, and Stop Game undoing it all, plus a script driving a
hinge motor and reading its angle), generated-protocol checks, and 26 ordinary bridge tests. The
workflow suite covers component add/remove rules, derived node types, type inheritance and creation
of every node type, prefab save/instantiate/undo, v12 script migration, joints (protocol validation
and undo, type defaults, partner removal and undo, copy/paste/duplicate remapping, v17 save/load,
and simulated point, limited hinge, motor hinge with runtime reversal, fixed, slider, distance,
ignored and colliding joined pairs, a partner destroyed mid-game, and joints spawned mid-game), the
demo's joints playground, v15 native controller migration to a script component, the demo's
controller template (tree placement, components, script, mouse lock, refusal to run untrusted), the
showcase starting with the player (trust required; the script suite plays it, walking forward from
its start through the player's camera), and input state (taps within a step, overlapping bindings,
deadzones, inversion, triggers, mouse motion, reset, map validation, saving in the project file and
reloading per project, migrating a v1 project's input.relay-input.json, simulation). The headless
editor suite drives the Game Configuration input page (press-to-bind actions and key-pair axes,
Escape cancel, new actions, reset, wrapping that keeps buttons clear of Delete), the Add Component
window (categories, add, disabled duplicates), section removal, the Add Node tree (indentation,
typed creation under the selection, non-creatable categories, templates nested under their type with
the palette icon and its "Custom template" tooltip), the Joint section (choosing the connected body
and the type), the Hierarchy search (name search, type filters with subtypes, reveal in the tree,
collapse and expand all) and the Assets collapse/expand-all button, and the Run Game → trust prompt
→ build → play flow. The demo's First Person Controller, including shooting, was also play-tested by
hand on a Linux desktop. The workflow suite covers graphics settings (defaults, partial updates,
saving, per-project reloading, rejecting unknown or mistyped settings) and the headless editor
suite the Game Configuration Graphics page. With both lighting effects off, offscreen captures of
the demo and of the editor viewport match the renderer before the G-buffer rework within one
level per channel; the model, resize and asynchronous capture smokes also pass offscreen with
both effects on. Every offscreen Vulkan mode (scene captures with each effect combination, the
model and async smokes, the editor with both effects on, and `relay_fidelityfx_tests`) runs
without errors under the Khronos validation layer with synchronization validation enabled. The desktop smokes below predate the asset browser, layout persistence, and
frame-pacing changes and were not rerun for them; those changes are covered by headless and protocol
tests. The live Vulkan shadow and visual smokes pass, as does `tests/editor_hdr_upload_smoke.py`:
exposure changes captured pixels, keyframe scrubbing changes the image, a model import submits
another GPU upload, and a package containing the saved keys and imported asset opens as a tar. The
legacy coordinate-based `editor_interaction_smoke.py` misses targets on this desktop's 2560x1410
layout; its input assertions are inconclusive until recalibrated. Socket-dependent tests were run
with loopback access after the sandbox refused local socket creation. Headless editor tests cover
the current hierarchy spacing, chat selection/wrapping, attachments, media viewer, composer layout,
usage meters, camera controls, authorization, collider zero-extent input, and round collider
outlines. Convex and mesh colliders have headless physics coverage but no real-desktop visual check.
A separate sustained fixture has exercised more than two minutes of editing/capture work with
injected disconnects. The Vulkan directional-shadow path also has real-desktop coverage that
compares captured receiver pixels with and without an occluder for cascaded directional, perspective
spot, and cubemap point shadows; the general visual smoke covers capture isolation and swapchain
resizing with all shadow resources active. These results do not prove live-provider stability or the
newest Agent-panel presentation quality.

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
| Components and templates | `src/scene/components.cpp`, `src/scene/templates.cpp` |
| Demo project and generator | `examples/demo/`, `tools/generate_demo_project.py`, `tools/demo_project/` |
| Test fixture models | `tests/fixtures/models/` |
| Physics and collision | `src/physics/`, `include/relay/physics/` |
| Gameplay scripting | `src/script/`, `include/relay/script/`, `sdk/`, `docs/scripting.md` |
| Rendering/import | `src/render/`, `shaders/` |
| Lighting (GI, reflections, acceleration structures) | `src/platform/vulkan_lighting.cpp`, `src/platform/vulkan_ray_tracing.cpp`, `shaders/gi_composite.frag`, `shaders/reflection_*.comp`, `third_party/fidelityfx/` |
| Editor | `src/editor/`, `include/relay/editor/` |
| Agent bridge | `tools/mcp-bridge/src/` |
| Input | `src/core/input.cpp`, `src/platform/sdl_input.cpp` |
| Native tests | `tests/engine_tests.cpp`, `tests/script_tests.cpp`, `tests/editor_*tests.cpp` |
| Bridge tests | `tools/mcp-bridge/tests/` |

When implementation changes, update this present-state summary instead of appending dated logs.
