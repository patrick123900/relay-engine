# Relay Engine — engineering handoff

This file records only the state needed to continue development. User-facing material belongs in
[`docs/editor.md`](docs/editor.md) (the editor guide); keep [`README.md`](README.md) a short
overview for visitors. Protocol details belong in [`docs/protocol.md`](docs/protocol.md). Follow
[`AGENTS.md`](AGENTS.md) for working and verification rules.

## Current snapshot

- C++20 engine/editor with SDL3, Dear ImGui, ImGuizmo, Vulkan, and a deterministic CPU renderer.
- External TypeScript agent bridge using Codex App Server and generated MCP tools.
- Protocol schema v51: 167 native methods. Scene v26, project v2, import manifest v3.
- Linux/RADV is the verified graphics path. The project is experimental and pre-1.0.
- HDR rendering, bounded asynchronous uploads, transform keyframes, box/sphere/capsule/convex/mesh
  colliders, Jolt body simulation with fixed/point/hinge/slider/distance joints, a Unity-style
  component model with derived node types and templates/prefabs shown in the node type tree, native
  C++ gameplay scripts, per-project input mapping, a scripted first person controller template in
  the demo project, portable project export, and FidelityFX global illumination and hardware ray
  traced reflections, a frame profiler, and audio (sources, listeners, spatialisation and mixer
  buses, bus effects, reverb zones, occlusion, streaming, music players and headphone output),
  a Sky node (gradient or panorama sky material, sky lighting, sun and distance fog), and custom
  shaders (a GLSL-based language for surfaces and post processing, shader materials, a Shader
  Editor), and a game interface (Godot-style canvases, anchored controls, containers and widgets
  drawn over the game view during Run Game only, an Interface editor panel, `on_ui` for scripts),
  and particle emitters (a ParticleEmitter node: shapes, rate, distance and burst emission, forces,
  turbulence, plane and world collisions, sub emitters, curves, gradients and flipbooks, drawn as
  sorted, soft, optionally lit sprites and previewed live in the editor) are implemented. Preserve unrelated working-tree
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
  game viewport uses the active scene camera rather than the editor inspection camera. In the
  editor, F5 runs the game and F8 stops it, even while the game has input; the run controls sit
  at the centre of the toolbar and the frame rate in the viewport's top-right corner.
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
- Asset references and the Asset Browser (`src/editor/editor_ui.cpp`, from `asset_field` to
  `draw_asset_browser`). Inspector fields that name an asset or a node (renderer mesh and material,
  material shader and image parameters, collision mesh, audio clip, skybox, panorama, joint
  partner, and script text properties whose default or value has an asset extension) draw the
  kind's icon and the name without folders or extension (`asset_display_name`; built-in names are
  title-cased, imported meshes and materials use their new registry `label`), with the path in the
  tooltip. Clicking the field opens the browser (right-click → Show in Assets reveals the file).
  Drops are typed by the same pick: while a `relay.asset` or `relay.entity` payload is dragged,
  each field builds its `AssetPick` and `drop_verdict` accepts files of its kinds passing
  `accepts`, nodes among its `@scene` choices, and a model file standing for its one imported
  mesh or material (several open the browser inside the model); fields that would take the drag
  are outlined, and refusals replace the drag tooltip with what the field takes (`AssetPick::what`
  or one derived from kinds). Hierarchy rows now select on an undragged release
  (`hierarchy_pressed`), so dragging a node leaves the Inspector on the current one. Fields, the Music player's **+ Add track...** and Post Process
  **+ Add effect...** buttons open the browser with an `AssetPick`: accepted kinds, an extra file test, `AssetChoice`s for non-file items
  (group `""` = footer button such as None or Gradient; `@builtin`, `@ready`, `@scene`, or the source
  model's path for imported meshes and materials, which makes that model a folder-like tile),
  actions (New material...) and a `choose` callback. Picks are chosen frames later, so every
  callback captures by value. Tile activation and tree navigation are deferred to the end of the
  window (`browser.activate`, `go_to`) because both replace listings the frame's tiles point into.
  The window is non-modal and undocked, keeps its size in the layout ini, relists the open folder
  every 2 s, and searches through `assets.search` with the new multi-word `query`, `folder` scope
  and `paths` matching (protocol v49); imported and built-in choices are matched locally.
  Material type, shader and panorama come from `assets.material`/`assets.sky_material`, cached 4 s
  per file (`material_facts_of`). Its menus close on Escape themselves (the editor does not enable
  ImGui keyboard navigation, so ImGui never closes popups on Escape), and the editor's shortcuts
  stand aside while it has focus. Tools → Asset browser opens it without a pick. Headless keys:
  `asset_browser:item:<path>`, `:choice:<value>`, `:button:<value>`, `:action:<label>`,
  `:tree:<folder>`, `:choose`, `:cancel`, `:search`, `:filter[:kind]`, `:options`, `:view`.
- Thumbnails (`AssetThumbnails`, `src/editor/asset_thumbnails.cpp`): images and models are drawn on
  worker threads by `image_thumbnail` and `model_thumbnail` (engine, `src/render/asset_thumbnail.cpp`:
  box-filtered image shrink; a private import of the model with the `static_mesh` preset drawn by a
  small CPU rasterizer at 2x with Lambert, sky and rim light, base colors and color textures, or one
  mesh chosen by its `.mesh.<index>` suffix, since the content id differs by preset), at most two
  jobs, visible tiles first, 128 px, rechecked against file time and size every 1.5 s, at most 320
  textures. Surface materials use the window's material preview: when the Inspector does not need
  it, `build` asks for the thumbnail cache's next material, and every delivered preview is offered
  to the cache (the renderer does not redraw an unchanged material, so the last delivery is kept
  in `last_preview`). Sky materials show their panorama; `.blend` models, post materials, shaders
  and other kinds show their icon. ImGui textures are UNORM on the sRGB swapchain, so pixels are
  linearised before upload (`srgb_rows_to_linear`); the Inspector's material preview was shown too
  bright before this and is converted too.
- Editor widgets (`src/editor/editor_widgets.cpp`): `editor_checkbox` replaces every
  `ImGui::Checkbox` (a box about 0.86 of the font size, centred in a normal row); `editor_slider`
  replaces the horizontal sliders (Inspector `slider_scalar`, mixer effect parameters, bus volume,
  axis deadzone): a thin track filled to the value, a round knob, and a value box that takes typed
  values on click (Ctrl+click and double-click anywhere also type), optionally logarithmic, rounding
  to the displayed precision. `slider_format` picks decimals from a hint's step or the range (two up
  to 10, one up to 100, none beyond). `AssetIcon` has one drawn icon and colour per kind (folder,
  model, mesh, scene, template, image, surface/sky/post material, shader, script, text, sound,
  video, node, other), also used by the Assets panel. The mixer's vertical faders and the agent
  reasoning slider are unchanged.
- `render.assets` reports each mesh's and material's `label` (the model file's own name, or "Mesh 2")
  and `source` (project-relative model), set by the importer (`MeshAsset`/`MaterialAsset::label`,
  `source`) and not part of content hashing.
- Inspector labels go through `field_display_name` (in `inspector_field_label` and `drag_vector3`):
  "##id" suffixes dropped, underscores become spaces, first letter capitalised, so script and shader
  names read "Wave height". Every Inspector checkbox (`inspector_checkbox`) and script text property
  puts its name in the left label column like other fields.
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
- The live editor advances the fixed 60 Hz game step from real elapsed time (at most four steps
  per frame), so frame rate and game speed are independent of the monitor. Presentation follows
  the project's `settings.graphics.vsync` (default off; Game Configuration → Graphics,
  `graphics.set_settings`, `RELAY_VSYNC=0/1` overrides): on is FIFO; off prefers immediate
  (may tear), then mailbox, then FIFO. `graphics.settings` reports the mode in use under
  `renderer.presentation`. A change rebuilds the swapchain after the next present. Mailbox with a
  fixed 16 ms loop sleep once dropped a frame about 2.5 times per second on 60 Hz displays, which
  is why it is only the fallback. The offscreen SDL driver on RADV offers mailbox and FIFO.
- Render interpolation: before each game step `Engine` remembers every entity's local transform,
  animator time and transform-animation time (`RenderInterpolation::remember`). Presented frames
  pass `Engine::render_interpolation(unsimulated / step)` to the window, and `WorldResolver`
  blends previous and current: position and scale linearly, rotation by quaternion slerp along
  the short arc, and looping clip times forward through a wrap. The display therefore runs up to
  one step (16.7 ms) behind the simulation. It is off in Editor mode, while paused and before the
  first step, and captures (`capture_buffer`) and every other `build_render_scene` caller draw the
  exact state, so deterministic output is unchanged. Without it, a 360 Hz display showed each
  60 Hz step six times and the game looked like 60 FPS.
- Loop pacing (`run_live_editor_session`): in Editor mode each iteration lasts at least 4 ms
  (250 FPS). During Run Game the project's `settings.graphics.frame_rate_limit` (0–1000, default
  0 = unlimited, Game Configuration → Graphics, `graphics.set_settings`) paces the loop instead,
  with deadlines advanced by whole periods so the average rate is exact; unlimited leaves pacing
  to presentation (the refresh rate with vsync, none without). When the last draw presented nothing (a minimized window) the 4 ms floor
  applies in both modes. The profiler names the waits "Frame pacing" and "Frame rate limit".
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

### Frame profiler

- `relay::profiler()` (`include/relay/observe/profiler.hpp`) is a process-wide main-loop
  profiler. The live editor loop in `apps/relay_demo/main.cpp` brackets each iteration with
  `begin_frame`/`end_frame`, so a frame's time is the displayed frame time. `RELAY_PROFILE_SCOPE`
  and `RELAY_PROFILE_WAIT` time a block; waits (GPU fence, swapchain acquire, present, frame
  pacing) are kept apart from work so the report can tell CPU-bound, GPU-bound and
  display-limited frames apart (`bottleneck`: busy side at 80% or more of the frame, otherwise
  `display`). Scopes record only on the thread that opened the frame and only inside a frame;
  headless engines and tests record nothing unless they open frames themselves.
- Repeated scopes with one name under one parent merge and count calls (fixed game steps, script
  instances: each behaviour's `on_update` is one scope). Frames keep at most 4,096 nodes and the
  ring holds 1,200 frames. Instrumented: input, agent requests, simulation → game step → scripts,
  animation, physics (build, Jolt update, write-back), contacts, the deterministic software
  renderer (only when a capture reads it) and video recording; render → uploads, fence wait, acquire, editor UI build, render
  scene build, frame data upload, command recording (GI, reflections, editor UI), submit, present.
- The deterministic CPU frame (`SoftwareRenderer`) is a pure function of the frame index and
  elapsed time. Game steps only record those; the pixels are drawn when a deterministic capture,
  CPU-source video sample or the SDL fallback window reads the frame. Drawing it on every step
  was the largest CPU cost of the demo game in the live editor (4.5 ms per frame in the Debug
  build).
- GPU passes use up to 16 timestamps per frame in flight (`mark_gpu_pass` in
  `vulkan_window.cpp`), read back when the frame's fence completes and attached to the profiler
  frame that recorded them, two frames late.
- `profiler.read` aggregates the most recent frames (optionally game frames only or one frame)
  into frame statistics, a depth-first call tree with parent indices, hotspots merged by name
  across call paths, GPU passes and a frame-time history. `profiler.set` pauses, resumes or
  clears. The editor's Profiler panel (Tools → Profiler, docked with Diagnostics) polls
  `profiler.read` four times a second while visible, pauses the profiler when the game stops and
  resumes it when the next game starts; clicking a graph bar pauses and inspects that frame.

### Components, node types and templates

- A node is a Transform plus optional components. Engine components keep typed storage in
  `EntityRecord` (`std::optional` per kind, for speed); scripts are `std::vector<Script>`, each
  with a behaviour name, enabled flag and property overrides (`ScriptProperty`: boolean, number,
  vector or text). `src/scene/components.cpp` is the catalog (`engine_components()`: id, name,
  category, addable, removable, multiple) and the add/remove rules shared by protocol and editor.
  The Transform and imported model animation (`animator`, which model-node children depend on)
  cannot be removed. A camera added to a scene without an active camera becomes active.
- Node types are a tree in `src/scene/node_types.cpp`: Node > Model, PhysicsBody
  (> RigidBody, StaticBody), Camera, Sky, Light (> DirectionalLight, PointLight, SpotLight),
  StaticMesh, AudioSource, ReverbZone, MusicPlayer, PostProcess, ParticleEmitter. Each type
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

### Audio

- `src/audio/` (engine library, no UI): `AudioClip`/`AudioClipCache` decode .wav, .flac, .mp3 and
  .ogg with miniaudio 0.11.25 (public domain / MIT-0, fetched by CMake; Ogg through its bundled
  stb_vorbis, compiled once in `miniaudio_impl.c`) to float PCM at the file's own rate, stereo at
  most. Files are capped at 256 MiB and decoded clips at 256 MiB of samples, though anything over
  10 s streams instead (phase 3, below); the cache holds 1 GiB, evicting unused clips least recently used first, and reloads
  a file whose size or time changes. Clips resolve under the project folder, or `./assets` without
  a project, exactly where the asset browser lists files. Assets of kind `audio` (new) are those
  four extensions; `media` is now video only.
- `AudioMixer` is a deterministic software mixer with no thread or device: voices resampled
  (linear) into a bus tree, rendered in 256-frame blocks to interleaved stereo float at 48 kHz.
  Voice gains, bus gains, pauses and stops ramp over one block so nothing clicks; voices start at
  full level so attacks stay sharp. Buses fold into their parents children first. Each bus runs
  its effect chain (below) before its fader; Master's output is then clamped to [-1, 1] as a
  guard, which its default limiter keeps from mattering. Solo: while any bus is soloed, only soloed buses,
  their ancestors and descendants are heard. Meters are post-fader peak per channel and RMS with a
  300 ms release, read without consuming them. At most 256 voices.
- `AudioSettings` (`settings.audio` in the project file, parsed strictly like graphics): buses
  with name, parent, volume (-80 to +24 dB, -80 is silent), mute and solo, normalised parents first;
  exactly one parentless Master. Defaults: Master > Music, SFX, Ambience, Voice. Engine
  `audio_settings`/`set_audio_settings` save them like the input map. Protocol `audio.settings`,
  `audio.set_bus` (creates, renames keeping children, reparents, volume, mute, solo) and
  `audio.remove_bus` (children move up).
- Components: `AudioSource` (clip, bus by name — unknown buses play into Master — volume dB, pitch,
  pan for flat sources, loop, play_on_start, spatial, min/max distance, rolloff inverse /
  inverse_square / linear, doppler 0-5) and `AudioListener` (no fields). Scene v18 stores both
  (`audio_source`, `audio_listener`; stable ids 0x0f and 0x10). The creatable node type
  `AudioSource` ("Audio Source") adds a source; `scene.set_audio_source` edits it (undoable, with
  the inspector's drag gestures).
- `AudioSystem` (owned by `Engine`, `Engine::audio()`): the listener is the first node with a
  listener, else the active camera, else the origin. Spatial sources use distance gain (full inside
  min_distance, silent from max_distance; the curved models fade out over the last tenth of the
  range so crossing max_distance does not cut off), equal-power pan from the listener's right
  vector, stereo clips mixed to mono, and doppler from position changes per game step (speed of
  sound 343 m/s, ratio clamped to 0.5-2). Flat sources use a balance pan. play_on_start sources
  start on the first game step (and when spawned); each node starts once per game. Voices stop
  when their node, source or clip goes. Pause pauses game voices; Stop Game stops everything.
  In the editor nothing plays by itself: `audio.play` previews a source flat, `audio.stop` stops one
  or all. `audio.status` reports the device, listener, voices (gains, position, preview) and bus
  levels; `audio.clip` reports a file's format and min/max peak pairs for waveforms.
- Output: `EngineConfig::audio_output` opens the default device through miniaudio (f32 stereo 48
  kHz) whose callback renders the mixer under a mutex held only for mixing; clips decode before the
  lock. Windowed `relay_demo` sessions turn it on unless `RELAY_AUDIO=0`; tests and headless modes
  never open a device. Without a device each game step renders its own duration of audio and throws
  it away, so voices and meters advance deterministically with the simulation.
- Bus effects (`src/audio/audio_effects.cpp`, `AudioEffect` in `audio_settings.hpp`): up to 8
  per bus, saved in `settings.audio` with only each type's own parameters. Reverb is Jezar's
  Freeverb (public domain; 8 damped combs and 4 allpasses per channel, tunings scaled to 48 kHz,
  pre-delay up to 250 ms); delay (up to 2 s, damped feedback); 3-band EQ (RBJ cookbook shelves
  at 200 Hz and 5 kHz and a peak at mid_frequency); stereo-linked peak compressor; instant-attack
  peak limiter without lookahead; low-pass and high-pass biquads. `configure` keeps state so tails
  ring through parameter changes; a bus that keeps its name keeps its processors across layout
  changes. Default Master: a limiter at -1 dBFS. Protocol `audio.set_effect` (append without
  index, or change by index; a type change keeps shared parameters), `audio.remove_effect`,
  `audio.move_effect`. `audio.set_bus` and `audio.set_effect` take `preview`: `Engine`
  keeps previewed settings in memory (reported by `audio.settings` with `previewing`) until the
  next saved change or a project switch, so faders are heard live and the project file is written
  once on release.
- Reverb zones: `ReverbZone` component (sphere radius or box half extents in the node's space,
  fade in metres, preset room / small_room / bathroom / hall / cathedral / cave / arena / forest or
  custom, room_size, damping, wet_db, pre_delay_ms; stable id 0x11, creatable node type
  `ReverbZone`; `scene.set_reverb_zone`, where a preset copies its values and any hand-set
  parameter makes the zone custom). `reverb_zone_weight` is 1 inside the shape (box: clamped in
  local space and measured back in world space, so rotation and scale are exact; sphere: radius
  times the largest axis scale) falling linearly to 0 at `fade` outside; 0.1 mm of round-off
  counts as inside. Only during the game, each zone near the listener or a playing placed sound
  gets one of 8 mixer reverb slots (Freeverb, width 1; a zone keeps its slot while it matters,
  and extra zones beyond 8 are dry). A placed sound sends to the zones it is in, weighted by how
  far inside; outside every zone it sends to the zones the listener is in, by the listener's
  weights. Sends pass through the same bus faders as the dry sound and each slot returns into
  Master before its effects at `wet_db`; a slot let go rings out, then clears so an old tail cannot
  return with a new zone.
- Sources: `AudioSource` gained `occlusion` (default on) and `reverb_send` (0-1, default 1).
  Spatial game voices send `volume * distance gain * reverb_send`; flat sources and editor previews
  send nothing. Occlusion casts a ray from the listener to each occluding spatial source every game
  step (at most 64 per step, taking turns), skipping the nearest collider-bearing ancestor of the
  listener (the player's capsule) and hits on the source itself, its ancestors or descendants. The
  amount moves to 0 or 1 over 0.15 s; fully occluded is -12 dB with a one-pole low-pass sweeping
  from 20 kHz to 800 Hz. The send is not occluded, so a muffled sound still fills the room.
  `audio.status` adds each voice's send (all slots summed), cutoff and occlusion, and `reverb`
  (each slotted zone with its parameters and the listener's weight). `audio.debug_shapes` reports zones (centre,
  box half-edge vectors, world radius, fade) and spatial sources (centre, min/max distance).
- Editor: the Inspector's Audio source section (clip picker listing the project's sound files, also
  a drop target for sound files from Assets; waveform with a playhead; Play/Stop preview; bus picker
  warning about unknown buses; volume, pitch, loop, play on start, spatial with distances, rolloff
  and doppler, or pan) and Audio listener section (warns when several nodes have one). **Game
  Configuration → Audio** shows the output device and a bus table (tree indent, double-click
  rename, parent, volume slider saved when released, mute/solo, level meters, remove, add). audio
  status is polled only while a source is selected or the page is open.
  Phase 2 added: Occlusion and Reverb send on spatial sources; a Reverb zone section (shape, radius
  or half size, fade, preset, room size, damping, level, pre-delay, and during the game how much of
  the zone the listener hears); viewport outlines from `audio.debug_shapes` (every zone faintly,
  the selected one bright with its fade margin, and the selected source's min/max spheres, drawn
  by the new `ViewportPen` helper; the older collider and joint overlays still project on their
  own); and a dockable **Mixer** panel (Tools → Audio mixer, joining Diagnostics on first open, or
  the Audio page's Open the Mixer button). Its strips fill the panel height (the fader takes what
  is left) with name, mute/solo, parent, fader and stereo meter, the effect chain and a + Effect
  picker; the selected effect's settings open beside the strips (enable, move, remove, sliders,
  logarithmic for frequencies). Faders and sliders preview while held and save on release.
- Scripts: `RelayHostApi` gained `audio_play(entity, volume_db, pitch)`, `audio_stop` and
  `audio_playing` (appended; ABI version unchanged), wrapped as `entity.play_sound`, `stop_sound`
  and `sound_playing`. The volume and pitch adjust that playing only, on top of the authored source.
- Demo sound tests (built by `tools/demo_project/build_demo_project.cpp`; the sounds are
  synthesised by `tools/generate_demo_project.py`, so they carry no third-party licence):
  `sounds/thump.wav`, `tone.wav` and a seamlessly looping `hum.wav`. Every dynamic body in the
  physics and joints playgrounds, and the Ball template, has a spatial thump source on SFX and
  `scripts/ImpactSound.cpp`, which plays it on contact begin with volume from the body's velocity
  change over that step (silent below 0.6 m/s, full at 8 m/s, -30 dB at the quietest), a
  deterministic pitch spread of 8% and a 60 ms cooldown; heavier bodies have lower source pitches.
  The spinning torus hums (Ambience, looping, play on start). A "Hall reverb" box zone (16 x 6 x 7
  m, 3 m fade, hall preset) surrounds the joints playground. A "Tone button" (glowing cap on a
  chrome stand at (1.5, 1.05, 6.5), front right of the player's start) runs
  `scripts/ToneButton.cpp`: looking at it within 3 m and pressing interact, or hitting it with
  anything but the player, plays the next note of a major pentatonic scale (pitch ratios on a C5
  bell) and dips the cap. The player's camera (and the template's) has the audio listener. The
  script suite plays the showcase and checks thumps from at least five bodies, the hum, the
  listener and a button press.

Phase 3:
- Streaming: files over 10 s (`audio_stream_threshold_seconds`, decided from the decoder's length,
  or over 4 MiB when it cannot say; cached per file version) play through `AudioStream`, a
  four-second ring the game thread refills every update (and between 4096-frame slices when
  rendering headless) outside the mixer's lock and commits inside it. The mixer reads streams by
  running frame index, waits in silence rather than skipping when the ring runs dry, and loops by
  seeking the decoder. `audio.clip` on a long file summarises it in one pass
  (`summarize_audio_file`) instead of keeping it, and reports `streams`.
- `AudioSystem` keeps one table of playbacks by handle: node sources, one-shots and music tracks.
  One-shots (`play_clip`, protocol `audio.play_clip`, which returns `sound` for `audio.stop`) carry
  their own settings: SFX by default, spatial when given a position (and only during the game),
  occluded and sending to zones like sources, no doppler.
- Mixer voices can start `delay_frames` late and run a sample-accurate scheduled fade
  (`fade(voice, to, delay, duration, stop)`), which music uses.
- Music: `MusicPlayer` component (tracks, bus Music, volume, crossfade 0-30 s, shuffle,
  loop_playlist, play_on_start, bpm, beats_per_bar, first_beat_seconds, sync immediate / beat / bar
  / track_end; scene v20, stable id 0x12, creatable node type `MusicPlayer`,
  `scene.set_music_player`). During the game the first track starts at full volume; a change
  (`music_play`, protocol `audio.music` play/next/stop) computes the next beat or bar from the
  current track's position, starts the new track that many frames later and fades it in while the
  old one fades out and stops, all in mixer frames. About 0.25 s before a track's crossfade is due
  the next is scheduled the same way (track_end), so playlists crossfade into each other; a
  one-track looping playlist is one looping voice. Shuffle is a repeatable order per pass.
- Script bus changes (`set_bus_volume` with a fade in game time, `set_bus_mute`,
  `set_bus_effect`) are overrides on top of the saved (or previewed) mixer, applied only during
  the game and cleared by Stop Game.
- Headphones: `AudioSettings::spatialization` (`settings.audio.spatialization`, stereo or
  binaural; `audio.set_spatialization`). Binaural voices mix to mono and pass through a
  Brown–Duda head model per ear: a fractional interaural delay (Woodworth, up to about 0.66 ms)
  and a one-pole/one-zero head-shadow filter set from the angle between the ear and the sound,
  with a low-pass towards 6 kHz for sounds behind as a front/back cue. Elevation only enters
  through that angle; there is no pinna model or measured HRTF.
- Scripts: `RelayHostApi` appended `audio_position`, `audio_play_clip`, `audio_sound_stop`,
  `audio_sound_playing`, `audio_sound_position`, `audio_bus_volume`, `audio_get_bus_volume`,
  `audio_bus_mute`, `audio_bus_effect`, `music_play`, `music_stop` and `music_track`, wrapped in
  the SDK as `relay::audio::play` / `play_flat` (returning `relay::audio::Sound`),
  `set_bus_volume`, `bus_volume`, `set_bus_mute`, `set_bus_effect`, and `entity.sound_position`,
  `play_music`, `stop_music`, `music_track` with `relay::audio::Sync`.
- Editor: dropping a sound file into the viewport creates an Audio Source node (named after the
  file, 1 m above the drop point) playing it; double-clicking a sound in Assets previews it flat,
  and its context menu has Preview and Stop previews; a Music player section (playlist with
  play-this-track during the game, move up and remove, an add picker that also takes dropped
  files, Next and Stop during the game, bus, volume, crossfade, play on start, shuffle, loop,
  BPM, beats per bar, first beat and when changes land); and Speakers / Headphones on the Audio
  page.
- Demo: `music/daylight.wav` and `music/dusk.wav` (16 s each at 24 kHz, 120 BPM in 4/4:
  pad, bass on one and three, eighth-note arpeggios; generated, so no licence) stream through a
  "Soundtrack" music player (-8 dB, 1 s crossfade, changes on the bar). A "Music switch" pad on a
  stand at (-1.5, 1.05, 6.5), mirroring the tone button, runs `scripts/MusicSwitch.cpp`, moving the
  soundtrack on at the next bar. The tone button now ducks the Music bus 10 dB for 0.8 s under
  each note. The First Person Controller plays `sounds/shot.wav` as a one-shot from the muzzle on
  each shot (`shot_sound` property). The script suite checks the soundtrack, the switch landing a
  new track, the shot one-shot and the duck and its recovery.

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
- Scripts reshape nodes during Run Game through more appended host functions: `set_parent`
  (keeping the world pose through `editor_inverse_affine`, in single precision, so a turned
  non-uniform parent's shear is lost; or keeping the local transform), `has_component`,
  `add_component`/`remove_component` (the shared `components.cpp` rules; scripts go through
  `add_script`, which creates the instance at once, and `remove_script`, queued in
  `pending_removal` and applied by `flush_destroyed` before destruction: `on_destroy`, then the
  component, then `shift_components` renumbers the entity's later instances), component fields
  (`component_get/set_numbers/text` over `src/scene/component_fields.cpp`, a table of
  "<component>.<field>" names for camera, mesh renderer, light, collider, physics body, joint,
  audio source and keyframes, set through the scene's validating setters; renderer and collider
  meshes and materials are checked against the asset registry as the protocol does),
  `joint_connect`, `shape_cast` and `overlap_shape` (`RelayShape`: sphere, box or capsule).
  Collider, physics body, joint and keyframe presence changes, a renderer change under a collider,
  scale writes and reparenting call `PhysicsWorld::rebuild_bodies`, which recreates the bodies from
  the scene, keeps dynamic velocities, recreates joints on or to them from the current poses, and
  moves their touching pairs to `ContactFilter::held`: the next step drops a held pair's new begin,
  or ends it if it no longer touches, so a rebuild neither ends nor restarts a live contact. Mesh
  collider triangles are returned to the world's budget when their body goes. Shape casts use
  `CastShape` with a scene body filter and back faces on for convex shapes, so a shape starting
  inside a collider hits it at distance zero.
- Trust is per user, outside every project: `trusted-script-projects` in the user config
  directory (`RELAY_SCRIPT_TRUST_PATH` overrides it; tests use temporary files), keyed by the
  canonical project folder. `scripts.trust` is host-only; revoking trust unloads the library.
  Agents can read and write sources (`scripts.read`/`write` are file-scoped on `path`) but cannot
  make them run in an untrusted project.

### Custom shaders and materials

- Language (`src/render/shader_language.cpp`, reference in `docs/shaders.md`): `.relay-shader`
  files start with `shader_type surface;` or `shader_type post_process;`, may declare
  `render_mode transparent, unshaded;` (surfaces) and `uniform` parameters (float, vec2-4, int,
  bool, sampler2D; hints source_color, hint_range(min, max[, step]), hint_normal/black/white;
  defaults), and define `void vertex()` / `void fragment()` over built-ins (VERTEX, ALBEDO,
  COLOR...). The parser blanks Relay's declarations (keeping lines), records top-level function
  spans, lays uniforms out by std140 (images get set 1 bindings 1-8), and checks names, hints,
  defaults and the stage function signatures. `generate_shader_glsl` wraps the code: a vertex stage
  (fragment() blanked, so fragment-only built-ins like dFdx stay out; spec constant 1 marks the
  shadow pass), a fragment stage built on `surface_lighting.glsl` that fills `Surface` from the
  outputs and writes the G-buffer like `first_light.frag` (spec constant 0 = geometry pass;
  unshaded writes black, fully metallic G-buffer albedo so GI and reflections add nothing), a
  shadow fragment stage (alpha scissor only), and for post shaders one fragment stage over the
  tone pass's full-screen triangle. User code follows `#line 1`; Relay's code is numbered from
  100000 (main) and 200000 (preamble), and errors there are reported for line 0. glslang (system
  package, optional: `RELAY_HAS_GLSLANG`) compiles to SPIR-V 1.0; glslang's "compilation
  terminated" note is dropped. `shaders/scene_bindings.glsl` (set 0, split out of
  `surface_lighting.glsl`) and `surface_lighting.glsl` are embedded at build time
  (`tools/embed_shader_sources.cmake`) and expanded in place of `#include`. The lighting buffer
  gained `frame_time` (TIME): the host's clock (`VulkanWindow::set_shader_time`, the live editor
  passes wall-clock seconds) in presented frames, the frame's elapsed time in captures.
- `ShaderLibrary` caches compiles by file stamp and holds previews (in-memory text per path,
  `shaders.preview`). Materials (`src/render/materials.cpp`): `.relay-material` with type surface
  or post_process, a shader path and parameters (number arrays or image paths). `MaterialLibrary`
  resolves one into a `ResolvedShaderMaterial` (compiled shader, packed std140 block, decoded
  images, error and warnings, revision), reloading when the file, shader revision or an image
  changes. `Engine::sync_materials` (tick, and `sync_render_assets` in scene captures) resolves every
  mesh renderer material path and the active post processing's enabled effects into
  `AssetRegistry::set_shader_materials`, apart from the asset revision.
- Scene v22: `MeshRenderer::material` may be a surface `.relay-material` path; component
  `post_process` (stable id 0x14, `PostProcess` of `PostEffect{material, enabled}`, at most 16,
  first node in entity order is used, creatable node type `PostProcess`). v23 adds
  `PostEffect::editor` (default true; v22 files load with it on): `build_render_scene` leaves
  effects with it off out when given a `ViewOverride` (the editor's own camera), so they show only
  through the scene camera. `build_render_scene`
  gives instances with material paths `shader_material` (and `builtin.grey`, a new built-in
  appended after the others, as their material-buffer stand-in for ray traced hits); transparent
  shaders make instances alpha blended. `RenderScene::post_effects` lists drawable enabled effects.
- Protocol: `shaders.read`, `shaders.write` (create from a template of a type, clears the preview),
  `shaders.preview` (text, or none to stop), `assets.material`, `assets.set_material` (create with a
  type; change the shader), `assets.set_material_parameter` (value, texture, reset; checked against
  the shader when it compiles), `scene.set_post_process` (effects that stay in a replaced list keep
  their enabled flag), `scene.set_post_effect`; `scene.set_renderer` accepts surface material
  paths. `.relay-shader` files are asset kind `shader`.
- Post shaders' set 0 also has the G-buffer motion (binding 3) and a per-frame `PostCamera` uniform
  buffer (binding 4: inverse view-projection and the previous frame's view-projection, the current
  one on the first frame). `scene_motion(uv)`/`MOTION` read motion vectors on surfaces and reproject
  the far plane for the sky; `DELTA_TIME` (push constants' w) is measured between live frames and
  1/60 in captures. The demo scene has a Post Process node with `materials/MotionBlur.relay-material`
  (editor view off), added by hand like its Sky; `tools/generate_demo_project.py` does not create
  either. `relay_demo --vulkan-scene-capture <project> <png> [frames] [turn-degrees]` turns the
  active camera before the capture so it holds one frame of camera motion.
- Vulkan (`vulkan_window.cpp`): set 1 layout (UBO + 8 samplers), `custom_pipeline_layout` (set 0 +
  set 1, the draw push constants for both stages, so set 0 stays bound across pipeline switches),
  `post_pipeline_layout` (post inputs + set 1, 80 bytes of push constants). Pipelines per shader
  revision (surface: geometry or forward pass + shadow; post: full-screen), made on first use and
  destroyed with the swapchain. Per material path: parameter UBO, images uploaded synchronously
  with mipmaps (`create_sky_texture`, generalized), a pool and set. `sync_shader_materials` builds
  new ones each frame before recording and, when a material is replaced or more than 16 things
  are unused, waits for the device and destroys the stale ones. `custom_binding` picks an
  instance's pipeline and set, or the built-in error shader's (magenta checkerboard, unshaded)
  when its material cannot be drawn. Geometry, forward and all shadow passes switch pipelines per
  instance. Post processing: after transparent geometry the forward pass ends; each effect draws
  from one HDR image of a pair (the scene's HDR image and a second one, `targets.post`) into the
  other with `post_render_pass`; an odd count adds a copy (a built-in post shader) so the result
  is back in the scene image; a barrier returns it to attachment layout and the forward pass runs
  again for the grid and selection. The forward pass now stores depth and stencil for that.
- Shader graphs (`src/render/shader_graph.cpp`): people edit shaders as nodes; the language is the
  saved form. `shader_to_graph` parses each stage body (comments masked) with a GLSL-subset
  expression parser and builds SSA dataflow: declarations and assignments (plain, compound and
  component-wise) of locals and output built-ins; nodes are input (built-in read), parameter
  (uniform), binary, unary, select, swizzle, set (component replace), call (GLSL functions,
  constructors, the shader's own functions) and one output per stage; constant sub-expressions
  become pin values; a local's name becomes its node id. Control flow, blocks, indexing,
  increments, bare calls and field access keep the stage as code (`code_only`, with a reason). Other
  top-level code is the graph's `functions`. `update_graph_types` infers types (widest-type rules,
  a function table, helper signatures). `graph_to_shader` writes canonical code (one typed local
  per node in dependency order, `set` as declare-then-assign, unknown types inlined), then outputs,
  then `// relay-graph {stage:{id:[x,y]}}` sorted; it records which line each node wrote, for
  errors. Round trips are exact after the first write; positions missing from the layout come from
  `layout_graph_stage` (columns by depth).
- Editor: **Shader Editor** panel (index 11; Tools → Shader editor; double-click a shader in Assets;
  Edit shader in the Inspector; first opens as a tab beside the Viewport) showing each tab's graph
  on a canvas (`src/editor/shader_graph_canvas.cpp`): pan, zoom, box select, drag nodes (the view
  keeps a draw order; nodes carry invisible covers so the top node takes the pointer), drag wires
  both ways with loop refusal, detach by dragging an input, values on free pins, split/set
  components, add-node menu (right-click, Space, or a wire dropped on empty space) listing inputs,
  parameters, operators, GLSL functions and the shader's functions, node menu, Delete, F to frame,
  errors outlined on nodes via the line map. Side panel: parameters (add by kind, rename
  everywhere, remove, put on canvas), render modes, and the functions as a code box
  (ImGuiColorTextEdit v1.92.9, MIT, fetched by CMake). Code-only stages and files whose
  declarations do not parse are edited as code in place. Undo/redo are text snapshots; live preview
  follows committed edits after 0.35 s (moving nodes changes only the layout line and does not
  recompile). While the panel has focus the editor's global shortcuts stand aside. The Mesh renderer section lists surface
  materials (and New material, drops) and shows the material's shader picker, Edit shader, errors,
  warnings and typed parameter fields (sliders for ranges, sRGB color pickers, image pickers with
  drops, Reset), all saved to the file. The Post Process section lists effects with on/off, order,
  remove, each effect's parameters, and an Add effect picker. Assets Create has Surface shader,
  Post-processing shader, Material and Post-processing material.

- Later additions (details in `docs/shaders.md`): scene v24 `MeshRenderer::parameters` (per-object
  uniform values; protocol `scene.set_renderer_parameter`; script ABI `set/get/clear_material_parameter`;
  Inspector "This object only"). Material parameter blocks now live in a per-frame buffer bound as a
  dynamic UBO, so parameter changes rebuild nothing (`write_frame_parameters`). Material preview:
  `builtin.sphere` drawn with the material's pipelines into small per-frame targets after the shadow
  passes, read back and handed to `EditorOverlay::material_preview_ready`
  (`relay_demo --vulkan-material-preview <project> <material> <png>`). Graph copy/paste/duplicate
  (`copy_graph_nodes`, `paste_graph_nodes`). Ready-made effects in `shaders/effects/` (Bloom, Color
  Grading, Vignette; `shaders.write` `effect`). Post images have mip chains built before effects that
  call `scene_color_lod`. Shader Editor tabs check their file once a second and reload (or, with
  unsaved edits, offer to) when an agent changes it. Checked on a desktop (preview, per-object
  values, Add effect, bloom and vignette in the live viewport, graph shortcuts, reload).
  The demo scene gained a "Shader showcase" group (three plasma orbs on pedestals at z 4.8, per-object
  colors) and Bloom ahead of motion blur, added by editing the scene file; the demo generator does
  not create them.

### Rendering and assets

- Sky (scene v21, component `sky`, stable id 0x13, `Sky` in `scene.hpp`): `material` (a
  `.relay-material` path, empty for the gradient), `horizon_color` (the horizon and below),
  `zenith_color` (straight up), `intensity` (visible sky, 0-100), `ambient_intensity` (0-10), and
  linear fog (`fog`, `fog_start`, `fog_end` > start, `fog_start_color`, `fog_end_color`). Colors
  are linear, 0-1000. `Scene::active_sky()` is the first node in entity order with one. The
  creatable node type `Sky` sits before Light among Node's children, so a sky with its sun is a Sky;
  it adds `sky` and a directional light (`default_sun_color`, `default_sun_intensity` 2.5) and
  turns a new node to (-50, 30, 0) degrees. The `Sky` defaults (horizon and zenith colors,
  intensity 2) and the sun's intensity are the demo's daylight sky; the fog colors keep their own
  pale blue. Protocol `scene.set_sky` (colors as 3-number arrays). The gradient position is
  `1 - (1 - max(y, 0))^2` (`sky_gradient_position`, mirrored in `surface_lighting.glsl`).
- Sky materials (`src/render/sky.cpp`): `.relay-material` JSON (`format` `relay.material`,
  `version` 1, `type` `sky`, `panorama`, `tint`, `intensity`, `rotation_degrees`), asset kind
  `material`. Paths go through `workspace_file`; writes are atomic. Protocol
  `assets.sky_material` (with the image size or why it cannot load) and
  `assets.set_sky_material` (`create` refuses to replace). Panoramas are PNG or JPEG, at most
  128 MiB and 8192 x 4096, equirectangular with -Z at the centre (`sky_panorama_uv`).
  `SkyMaterialCache` reloads on file size or time changes and keeps the decoded image when only
  the material changed; `Engine::sync_sky` (every tick, and in `--vulkan-scene-capture`) puts the
  active sky's material into `AssetRegistry::set_sky_material`, apart from the asset revision.
- `build_render_scene` fills `RenderScene::sky` (`RenderSky`): visible radiance (colors times
  intensity, or the panorama times tint and both intensities), and the analytic hemisphere
  (`ambient_up`/`ambient_down`, irradiance scale): pi * ambient_intensity times the cosine-weighted
  upper average (gradient: horizon + 5/6 of the way to the zenith; panorama: averages computed at
  load) and the horizon color or the panorama's lower average. Without a sky the defaults
  (`default_ambient_up/down`, the old shader constants) apply and nothing else changes.
  `sky_radiance`, `sky_environment` and `fog_amount` are the CPU versions of the shader code.
- The sun: when the active sky's node has a directional light with intensity above zero,
  `RenderSky::sun` is set with the direction towards it and its color times intensity.
  `sun_glow` (mirrored in `surface_lighting.glsl`) draws a disc of 0.8 degrees radius at 40 times
  that radiance with a soft edge, two exponential glows, and a fade below the horizon. It is part
  of `sky_radiance` and the sky pass only: `sky_environment` (GI's cube, reflected rays, ambient)
  leaves it out, because the light already lights the scene. Panoramas get the disc too.
- Vulkan: `GpuLighting` gained sky, ambient and fog vectors, which replace the shaders' constant
  sky and ground; `CompositeConstants` carries the ambient pair (128 bytes). Set 0 binding 6 is
  the panorama (a 1x1 placeholder without one), uploaded synchronously after `vkDeviceWaitIdle`
  when its revision changes (`sync_sky_panorama`), before recording. `shaders/sky.frag` runs in
  the forward pass after the GI composite, using set 0 and the composite's depth: premultiplied,
  it draws the sky where depth is clear and fog over opaque surfaces by camera distance.
  Transparent surfaces fog themselves in `first_light.frag`. Reflection misses and the GI
  environment cube (3 x 3 samples per texel, rebuilt when `sky_environment_key` changes) use
  `sky_environment`, so both see the sky times its ambient intensity.
- Editor: the Inspector's Sky section (Skybox picker with Gradient, material files and New sky
  material, drop target; sRGB color pickers storing linear values with a gradient bar; the
  material's panorama, tint, brightness and rotation, saved to its file as they change and not
  undoable; intensity, ambient light; Sun status with Add sun or Make it the sun; Fog). Assets
  gains Create → Sky material and a Materials filter.

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

### Game interface

- Data (`include/relay/scene/ui.hpp`, `src/scene/ui.cpp`): `EntityRecord::ui` (`UiComponents`)
  holds optional `UiCanvas`, `UiControl` (the rectangle: `anchor_min`/`anchor_max` fractions of the
  parent plus `offset_min`/`offset_max` in canvas units, Godot style; pivot, rotation, scale,
  visible, opacity, clip_contents, mouse_filter stop/pass/ignore, sibling `order`, and container
  hints `min_size`, `expand_x/y`, `size_x/y`), and widgets `UiPanel`, `UiLabel`, `UiImage`,
  `UiButton`, `UiToggle`, `UiSlider`, `UiProgressBar`, `UiContainer`. Widgets need the control; a
  canvas is never a control; at most one of button/toggle/slider/progress bar per node.
  `ui_components()` is a reflection table (field name, type, range, choices, asset kinds, typed
  get/set) that drives scene files (`"ui"` per entity, scene v25: an object keyed by component with
  every field; fields left out load as defaults, unknown ones are errors), the protocol, script
  field access and the Inspector. Colors are sRGB RGBA 0-1. `apply_ui_anchor_preset` implements
  Godot's 16 presets keeping a size. Component ids `ui_canvas` ... `ui_container` join the catalog
  (category UI; adding a widget adds the control; removing the control while widgets remain is
  refused). Node types Canvas, Control > Button, CheckBox, Slider, ProgressBar, Container (>
  VBoxContainer, HBoxContainer, GridContainer by layout), Panel, Label, Image, with styled
  defaults; a new control's order is one past its siblings'.
- Fonts (`src/ui/ui_font.cpp`): stb_truetype (FetchContent, pinned commit, compiled in
  `relay_stb_truetype`); Inter Regular and Bold (OFL, `third_party/inter/`) embedded by
  `tools/embed_binary_files.cmake`; project fonts by path, rechecked once a second. Glyphs are
  rasterized at their on-screen pixel size (rounded) into up to eight 1024² RGBA pages (page 0 has
  a white block for solid shapes); a full atlas is cleared between frames. Kerning from `kern`
  only; missing characters fall back to Inter; no complex shaping.
- Layout and drawing (`src/ui/ui_render.cpp`): `ui_sources` lists every entity with its parent
  and UI; `UiPainter::layout` builds canvases (sorted by sort_order; controls without a canvas
  ancestor use a screen layer at scale 1), places controls by anchors or by their container
  (minimum sizes from text, check boxes and children), and composes affine transforms (pivot,
  rotation, scale) plus clip rects. `draw` tessellates anti-aliased rounded rectangles (paired
  inner/outer paths; shadows are a wide fringe), borders as rings, check marks as strokes, images
  in six modes including nine-slice, and text quads (unrotated text snaps to pixels; outline is
  8/16 offset passes) into a `UiDrawList` of sRGB vertices, commands per texture and clip.
  `rasterize_ui` is the CPU twin (bilinear, top-left rule, blending in linear light).
  `UiImageCache` decodes PNG/JPEG (64 MiB, 8192²) and reloads changed files keeping texture ids.
- Runtime (`src/ui/ui_system.cpp`, `Engine::ui()`): each game step, after `InputState::begin_step`
  and before scripts, `UiSystem::update` lays out for the view size (`set_view_size`, set by the
  host each drawn frame; the engine config size headless), finds the topmost reachable control
  under the pointer (interactive widgets take it, stop blocks, pass continues), and turns left
  presses/releases into pressed/released/clicked/toggled/value_changed events, flipping toggles,
  dragging sliders (step snapped) and playing click sounds flat on SFX. Presses over a stopping or
  interactive control are consumed (`InputState::consume`: hidden from the game until released;
  `raw_control` still sees them). The pointer only acts while `InputState::mouse_locked()` is
  false (the map's lock_mouse unless a script overrode it for the run; cleared at Run and Stop).
  `simulate_click` presses and releases at a control's centre over two steps even when locked.
  Events reset at Run and Stop Game.
- Pointer coordinates: `VulkanWindow::poll_quit` rewrites mouse motion into game-view pixels
  (pixel density, minus the editor viewport region), so `InputState::mouse_x/y` and scripts'
  `mouse_position` are game-view pixels.
- Rendering (`vulkan_window.cpp`, `shaders/ui.vert/frag`): `set_game_ui_source` gives a callback
  asked each frame with the view size; `connect_game_ui` in `main.cpp` returns the engine's draw
  list only in Game mode and logs painter warnings. The window skips it whenever the overlay has a
  view override (the editor camera), so the interface never shows in the editor's view. Before the
  display pass, `prepare_game_ui` copies changed textures (per-frame staging, barriers in the
  frame's command buffer; images retire after a frame-slot round trip; unused for 600 frames go)
  and this frame's vertices/indices; `draw_game_ui` draws after the tone pass inside
  `draw_tone`, so it is in presented frames and in captures, with scissors per command. Vertex
  colors and textures are sRGB, converted to linear for an sRGB swapchain. Scene captures
  (`--vulkan-scene-capture`) run in Game mode without scripts, so they show the authored interface.
- Scripts: `RelayHostApi` appends `ui_get_numbers`, `ui_set_numbers`, `ui_get_text`,
  `ui_set_text` ("component.field" names, validated like the Inspector, logged refusals),
  `ui_state`, `ui_event`, `set_mouse_locked`, `mouse_locked`; callback `RELAY_CALLBACK_UI` (7)
  delivered by `ScriptSystem::dispatch_ui` before `on_update`, to scripts on the control and every
  ancestor. SDK: `Behaviour::on_ui(const ui::Event&)`, `Entity::set_ui/ui_numbers/ui_text`,
  `set_text`, `set_visible`, `set_value`, `set_checked`, `hovered`, `held`,
  `relay::input::set_mouse_locked`.
- Protocol: `scene.set_ui` (component, attached, `values` object, `anchor_preset` keeping the
  current laid-out size, `preset_margin`, gesture), `ui.layout` (rectangles in view pixels, local
  position/size, visibility, hover/held, the latest events, pointer), `ui.render` (CPU PNG of the
  interface alone in `captures/`, checker/black/transparent background, font/image warnings),
  `ui.click` (Run Game). The generator gained an `object` parameter type (validated natively;
  zod record; bridge JSON schema `additionalProperties`). `component.types` lists interface
  fields with types, ranges, choices and defaults. `scene.create` types include the UI types.
  Asset kind `font` (.ttf/.otf) is new.
- Editor: the Inspector shows UI sections (generated from the table; colors with alpha, text
  typed live as one undo step, asset pickers, choices) and replaces the Transform for canvases and
  controls; the Control section has the anchor preset grid, position/size or margins per axis,
  raw anchors and offsets, and order arrows. The gizmo is skipped for interface nodes. The
  Hierarchy lists sibling controls by order. Add Node wraps a control made outside any interface
  in a new Canvas. The **Interface** panel (index 12, Tools → Interface editor, opens beside the
  viewport) parses `ui` from `scene.list` (`rebuild_interface_sources`), lays out with its own
  `UiPainter` at a chosen resolution (game view size, presets, custom), draws through
  `InterfacePreview` (`src/editor/interface_preview.cpp`: UiTexture → `ImTextureData`, colors
  linearised; use `GetTexRef()`, since `ImTextureRef(ImTextureData*)` silently picks ImGui's legacy
  `void*` constructor), and selects, moves and resizes controls through `scene.set_ui` with one
  gesture per drag (Ctrl snaps to 8, arrows nudge). Controls in containers are not dragged.
  `EditorUi::set_game_cursor_locked` is a host seam: the loop passes the engine's lock each frame,
  and freeing the cursor warps it to the viewport centre.
- Demo: a HUD canvas (hints, crosshair, "Balls" counter) and a hidden Tab menu (Resume button,
  hints switch, music volume slider) run by `scripts/GameMenu.cpp`; the First Person Controller
  stands still while a script has freed the cursor it started with locked.
  `relay_build_demo_project --add-interface [root]` adds or replaces the HUD in an existing demo;
  the full generator builds it too.

### Particles

- Data (`include/relay/scene/particles.hpp`, `src/scene/particles.cpp`): `EntityRecord::particle_emitter`
  (`ParticleEmitter`, scene v26 key `particle_emitter`, stable id 0x1f, component id
  `particle_emitter` in category Effects, creatable node type `ParticleEmitter` before Canvas).
  About 70 fields in groups (emitter, emission, shape, particle, motion, collision, lifetime, sheet,
  renderer, sub_emitter), each described once in `particle_fields()` (name, `ParticleFieldType`,
  group, range, choices, description) with typed accessors, like `ui_components()`: scene files
  (every field; left out loads as default, unknown is an error), `scene.set_particle_emitter`
  `values`, `component.types` (type, group, range, choices, default), script field access and the
  Inspector all go through it. Value types add `ParticleRange` ([min, max] or one number),
  `ParticleCurve` ([[time, value]], empty is 1, linear, sorted on read), `ParticleGradient`
  ([[time, r, g, b, a]] sRGB, empty is white) and bursts ({time, count, cycles 0 = all cycle,
  interval, probability}); curves and gradients hold at most 8 keys, bursts at most 8.
  `color_alt` is used only with `random_color` (a white default mixed in washed out every color).
- Simulation (`include/relay/particles/particle_system.hpp`, `src/particles/particle_system.cpp`,
  `Engine::particles()`): per emitter state keyed by entity (first-seen order), copying the
  component every update so edits apply live. `Engine::tick` in Editor mode calls
  `update(scene, step, false)` (every non-sub emitter previews; one-shots replay 1 s after they
  finish unless stopped by hand); game steps call it with `game` true after contact callbacks, with
  `PhysicsWorld::raycast` for world collisions. Run Game and Stop Game `reset()`. Emission: rate over
  the step's slice of the clock (carry kept, a 1e-9 nudge so whole counts are not lost), bursts by
  whole cycle index (a float cycle index once stalled on 1.4 s cycles), rate over distance along the
  node's path; new particles are placed between the previous and current world transform and live
  only their share of the birth step, so streams from moving nodes stay even. PCG32 seeded from the
  seed (or the node index for 0) and play count; Perlin turbulence as a displacement velocity.
  Forces act in world space (turned into local space for local emitters), drag is exponential.
  Plane and world collisions reflect with bounce/friction and lifetime loss; with more colliding
  particles than 4096 rays per step each is tested every `stride` steps along the path since.
  Sub emitters: a named direct child with an emitter; it never emits by itself and spawns its count
  from its own shape at death/collision/birth events, processed after all emitters. Destroyed
  nodes' particles finish during the game (orphans, at most 60 s) and vanish in the editor. Caps:
  100,000 per emitter, 1,000,000 total (oldest of the largest trimmed). Prewarm simulates the last
  min(duration, lifetime max, 30 s) of a cycle at 30 Hz.
- Draw list: `render_list(scene, view, alpha)` builds world-space `ParticleSprite`s (64 bytes,
  std430) per `ParticleBatch` (texture, blend, alignment, lit, soft distance, flipbook, local axes),
  positions blended between steps by `alpha` (`Engine::particle_render_list` passes 1 while paused),
  sizes and colors from the life curves (colors linear times `emission`), sorted within the batch
  (distance, oldest, youngest) and batches far to near. Textures: `UiImageCache` for project files
  (warnings once each), `builtin_particle_texture` for seven textures drawn in code (128²).
- Vulkan (`vulkan_window.cpp`, `shaders/particle.vert/frag`): `VulkanWindow::set_particle_source`
  (called in every view with the camera position and forward; `connect_particles` in `main.cpp`,
  with the live loop's step fraction). `prepare_particles` runs before the forward pass: sRGB
  textures with CPU mip chains (alpha-weighted linear box filter) through a per-frame staging
  buffer, a per-frame sprite storage buffer and frame UBO (view-projection, its inverse, camera
  axes from the view matrix rows, extent). `draw_particles` draws after transparent geometry and
  before post processing: set 0 scene bindings, set 1 the composite set (depth for soft particles;
  `update_composite_descriptors` now also runs for particles), set 2 frame/sprites, set 3 the
  texture; six vertices per instance with `firstInstance` as the batch offset; push constants carry
  alignment, local axes, flipbook, soft distance, blend and lit. One premultiplied blend state
  serves alpha (covering), additive (alpha 0) and premultiplied textures; depth test on, no depth
  writes. Lit particles use `direct_lighting` and `ambient_diffuse` with a sphere normal across the
  sprite; all fog like transparent surfaces (additive fades out). `--vulkan-scene-capture` steps
  particles once per captured frame.
- Protocol: `scene.set_particle_emitter` (values, attached, gesture), `particles.status`
  (read-only; mode, total and per emitter playing/paused/sub/orphan/particles/time/cycle),
  `particles.control` (play, restart, stop, clear, pause, resume, emit with count; works in the
  editor and during the game, not undoable). Scripts: `RelayHostApi` appends `particles_play`,
  `_stop`, `_pause`, `_emit`, `_count`, `_playing`, `_get_numbers`, `_set_numbers`, `_set_text`;
  SDK `Entity::play_particles`, `stop_particles`, `pause_particles`, `emit_particles`,
  `particle_count`, `particles_playing`, `set_particles`, `particle_numbers`.
- Editor: the Particle emitter Inspector section (Pause/Resume, Restart, Stop, Burst and a status
  line from `particles.status`, polled with the other refreshes; tree groups, the first ones open;
  fields hidden when irrelevant via `particle_field_shown`; a curve editor, a gradient editor with a
  selected key's color and time, a bursts table, sub emitter picker of named children, texture
  asset field), a selected emitter's shape outline and an orange sparkle marker in
  `draw_scene_nodes`. Add Component's categories gained Effects, Audio and UI (the last two were
  missing from its list).
- Demo: `relay_build_demo_project --add-particles [root]` (also part of the full build) adds a
  Campfire at (-3.4, 0, 3.9) under the warm fill light (stones, crossed logs, glowing coals,
  alpha-blended HDR flames, stretched additive embers, lit smoke drifting on the wind), a Fireworks
  launcher at (7, 0.25, -6) (a rocket every 1.4 s bursting through the `Burst` sub emitter) and a
  gold `Trail` (rate over distance) under the Ball template. Additive flames washed out over the
  bright ground in daylight, hence alpha blending with emission 3.5; thin spark sprites vanish at
  20 m, hence star sprites for the burst.

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
   players. There is no text input API for scripts yet; scripts lock and free the cursor with
   `relay::input::set_mouse_locked`, and interface controls take the pointer while it is free.
10. Native scripts cannot be contained: a crash or endless loop in a script takes the editor down,
   and in a trusted project agent-written code runs with the user's privileges. Windows script
   loading is not implemented (builds report unsupported), and the macOS `.dylib` path has never
   been run. Script component fields do not cover the sky, reverb zones, music players or post
   processing, and scripts cannot reach another behaviour's instance (there is no
   GetComponent-style lookup). Rebuilding a body re-anchors its joints where the bodies are then,
   and every scale write rebuilds the subtree's bodies, so animating scale from a script every step
   is costly. The trust prompt and Scripts diagnostics view are covered headlessly only.
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
13. GPU timestamps bracket whole passes on the graphics queue; the panel's GPU numbers do not
   separate work that overlaps inside a pass.
14. Audio phases 1 to 3 are built. Not in it: portals or diffraction (occlusion is one ray, on or
   off, so a sound behind a thin post is fully muffled for a moment; partial occlusion was
   deliberately left out), a measured HRTF (the headphone mode is a head model, weak on elevation),
   a limiter lookahead, effect presets, and more than 8 zones heard at once. Zones only colour Run
   Game, not editor previews. Short clips still decode whole the first time they play (at most
   10 s, so the stall is small); streams read their files from disk as they play. Music timing is
   sample-accurate headless; with a device it is exact within the mixer, and the request itself
   is taken at the next game step. Audio has headless, protocol, compiled-script and
   headless-editor coverage only: sound through a real device has not been heard, and none of the
   audio UI has been looked at on a desktop. Resampling is linear. Dropping a sound into the
   viewport makes three undo steps (create, place, clip) rather than one.
15. The Sky has headless, protocol and offscreen-render coverage (gradient, fog and a panorama,
   under the validation layer with synchronization checks), not a desktop review of the Inspector
   section or of switching materials in the live editor, whose panorama upload waits for the
   device. Panoramas are 8-bit PNG or JPEG only (no HDR or EXR, no mipmaps, so very large images
   alias when minified) and sampled at level 0. Only the first sky is used, and skies do not blend.
   Ambient light from the sky is a two-colour hemisphere (plus GI's cube and reflected rays); there
   is no image-based specular convolution, the sun disc has a fixed size and is drawn over
   panoramas that may already show a sun, reflections show no sun disc, and fog is linear distance fog without
   height falloff or scattering. The analytic rough specular term keeps its old scale, pi times
   GI's.
16. Custom shaders have headless, protocol and offscreen-render coverage (a custom surface, the
   error surface, shadows and a post effect, under the validation layer with synchronization
   checks), not a desktop review of the Shader Editor, the material fields or live preview on a
   real display. Ray traced reflections draw shader-material surfaces with `builtin.grey`; motion
   vectors ignore vertex movement since the last frame; selection outlines ignore vertex movement;
   shadows of transparent shaders are not cast. Compiling and swapping pipelines run on the engine
   thread and wait for the device (preview keystrokes after a pause, so at most a few per second).
   Shader materials are not yet usable for skies, and imported models' materials cannot be
   converted to shader materials. Graphs: no per-node previews, comments inside converted stages
   are dropped, and stages with control flow stay code. The canvas has been looked at only through
   the headless test's rasterized snapshots (`RELAY_UI_SNAPSHOT_DIR`), not on a desktop.
17. The Asset Browser, reference fields, thumbnails, checkbox and slider have headless coverage and
   were looked at only in `RELAY_UI_SNAPSHOT_DIR` snapshots, not on a desktop. Thumbnails are 8-bit
   linear textures, so very dark gradients band slightly. Material thumbnails render one per frame
   through the preview pass. Models are imported once per thumbnail (and once per mesh tile); very
   large models take a while on the worker. Only project folders and a pick's choices are listed:
   meshes of models not yet imported cannot be chosen until the model is imported (import it from
   Assets). Escape still does not close menus outside the browser.

18. The game interface has engine, workflow, compiled-script and headless-editor coverage, plus
   offscreen Vulkan captures of the demo HUD and a sample menu on RADV; it has not been used on a
   desktop (real mouse over controls in the live editor, the Interface editor by hand, cursor
   warping when a menu frees it). Not implemented: world-space interfaces, keyboard/gamepad focus
   navigation, text input fields, scroll containers, rich text, interface animation (scripts
   only), multiple pointers or touch, and OpenType shaping (GPOS kerning, right-to-left and
   complex scripts). Translucent controls blend in linear light (lighter than design tools show).
   Hidden controls do not draw in the Interface editor. Rotated controls resize along their
   parent's axes in the Interface editor. Layout and draw lists are rebuilt from the scene every
   frame and step (fine for HUD-sized interfaces; nothing is cached by revision yet).

19. Particles have engine, workflow, compiled-script and headless-editor coverage and offscreen
   Vulkan checks on RADV; the Inspector section and live editor preview have not been used on a
   desktop. Particles simulate on the CPU; they are drawn after all transparent meshes (no
   interleaving), cast no shadows, write no motion vectors (motion blur smears them with what is
   behind), and are not in GI or ray traced reflections. Editor previews do not collide with the
   world. There are no ribbons or trails, mesh particles, GPU simulation, attractors, per-burst
   colors or particle lights, and emitter transforms are not interpolated between steps for
   local-space particles. The demo effects were tuned from offscreen captures only.

## Next priorities

1. Give scripts a kinematic character controller (Jolt's CharacterVirtual: stepping up stairs,
   slope limits, crouching) and add more example controllers (third-person, orbit) beside the
   demo's scripted first person one. Scripts can now add, remove and configure components,
   reparent nodes and shape cast.
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
hinge motor and reading its angle, and a builder script that makes a falling crate from components,
sets and reads fields with refusals, reparents under a turned and scaled parent, sphere, box and
capsule queries, adds and self-removes a behaviour, rescales a crate that then rests higher, and
retunes a resting body without ending its contact), generated-protocol checks, and 26 ordinary bridge tests. The
engine suite covers the profiler (scope merging, self and total time, waits, GPU attachment,
game-only and single-frame reports, pausing, clearing and the protocol), and the headless editor
suite the Profiler panel (hotspots, pause and resume, inspecting a frame from the graph). The
profiler was also run on the demo game in the offscreen live editor, where it reported every
scope and GPU pass on RADV; the panel has not been looked at on a desktop. The
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
hand on a Linux desktop. The workflow suite covers graphics settings (defaults, partial updates, the frame rate limit's
range and saving,
saving, per-project reloading, rejecting unknown or mistyped settings) and the headless editor
suite the Game Configuration Graphics page (including frame rate presets, typed limits and the
Vsync checkbox). Toggling vsync in the offscreen live editor switched the swapchain between FIFO
and mailbox. The
limiter held 60.00 and 144.01 FPS in the offscreen live editor running the demo game. With both lighting effects off, offscreen captures of
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

Audio is covered offline, never through a sound device: the workflow suite renders the mixer
(bus gain, mute, solo, meters, loops, every effect, zone reverb sends, occlusion's low-pass, the
binaural head model's delay and shadow), zone weights and blending, streaming, one-shots,
sample-accurate beat and bar music changes and playlist crossfades, script bus fades, and the
protocol and saving for sources, zones, music players and the mixer (including in-game
occlusion behind a real collider and live previews saved on release). The script suite compiles
the demo's audio scripts and plays the showcase: thumps from falling bodies, the hum, the tone
button's note and music duck, the music switch landing a new track, and the shot one-shot. The
headless editor suite drives the Audio source, Reverb zone and Music player sections, the Audio
page (buses, speakers and headphones) and the Mixer panel (mute, effects, a parameter dragged live
and saved on release). The release build caught an editor bug the dev build hid (a panel reading
settings it had just replaced), so run both.

The Sky is covered by the workflow suite (component and node type, `scene.set_sky` validation
and undo, the first sky winning, v21 save and load and v20 migration, sky material files and
their protocol, the render description of gradients, panoramas, lighting and fog, and a missing
image falling back), the headless editor suite (fog toggle, New sky material, choosing a panorama,
back to the gradient) and `relay_lighting_render_tests` (an offscreen demo copy with a gradient
sky, fog and a panorama material, checked by pixel).

Custom shaders are covered by the workflow suite (the language: templates, parsing, std140
layout, hints, refusals, errors at user lines, stage isolation; the protocol: write, read,
preview, materials and parameters, renderers with material paths, broken shaders, post
processing with reorder and undo, v22 save and load; graphs: agent code to nodes with types,
exact round trips, edits that compile, positions kept, loops refused, code-only stages), the
headless editor suite (material fields; the graph editor: add-node menu, wiring by drag, pin
values, Ctrl+S, errors on nodes, undo, parameters, render modes, delete, stages, revert, code
stages; Post Process add and toggle; `RELAY_UI_SNAPSHOT_DIR` saves rasterized pictures of it) and
`relay_lighting_render_tests` (a demo copy with an unshaded custom ground and a post effect,
checked by pixel; the demo's motion blur keeps a still camera sharp and blurs one turning 3 degrees
in a frame, by counting sharp edges).

The Asset Browser is covered by the engine suite (`image_thumbnail`, whole-model and one-mesh
`model_thumbnail`, refusals, import labels in `render.assets`), the workflow suite (multi-word
search, `paths`, `folder` scope) and the headless editor suite (`asset_browser_ui`: fields, surface
materials only, material thumbnails from a delivered preview, built-in materials, imported meshes
inside their model with per-mesh thumbnails, Tools → Asset browser, folders, image and sky
thumbnails, word order, folder-name matching, type filters, list view, the slider's typed value and
track drag; the joint, audio, music, sky and post-processing tests choose through the browser).
`tests/ui_snapshot.hpp` now samples each draw's own texture and encodes sRGB like the swapchain, so
snapshots show thumbnails and the real brightness.

Particles are covered by the engine suite (every field's JSON round trip, refusals, curves and
gradients, scene v26 files, the node type and component, rate, max particles, bursts including
1.4 s cycles stepped at 60 Hz, one-shots, sphere/box/shell/cone shapes, gravity, plane collisions
and lifetime loss, world and local space, rate over distance, death sub emitters, editor preview
versus play_on_start, play/emit/stop/clear, prewarm, seeds, the draw list's order, curves and
brightness, built-in textures, and the engine's preview, Run Game reset, orphans and Stop Game),
the workflow suite (`scene.set_particle_emitter`, refusals, undo and gestures, `component.types`
fields, `particles.status` and `particles.control` in the editor and the game, save and load), the
script suite (a compiled script playing, bursting, retuning and stopping an emitter, refusals,
Stop Game restoring it), the headless editor suite (`particles_ui`: shape fields, a Rate drag as
one undo step, curve and gradient keys, Add burst, Stop, Burst, Restart; a
`particle-inspector.png` snapshot) and `relay_lighting_render_tests` (additive, alpha and lit
particles in front of the demo camera, compared with the same view without them). With the demo's
committed player position the whole script suite, including the play-test that throws trailed
balls, passes with the demo's particle effects.

The game interface is covered by the engine suite (reflection round trips, validation, presets,
node types and ordering, scene v25 files, canvas scaling, anchors, a vertical box with ordering,
expansion and placement, hit testing, container minimum sizes, CPU-rasterized panel fill, border,
text and check box pixels, and pointer interaction: hover, click, consumption from game actions
while held, one-step taps, toggles, slider drags, a locked cursor and simulated clicks), the
workflow suite (`scene.set_ui` values, presets, refusals, undo, removal rules, `component.types`
fields, `ui.layout` pixels, `ui.render`, `ui.click` during Run Game), the script suite (a menu
script reading and writing fields, refusals logged, `on_ui` bubbling to the canvas and the button's
own script, check boxes, cursor unlock, Stop Game restoring) and the headless editor suite
(`interface_ui`: Add Node's automatic Canvas, the Inspector's Control and Label sections, typing
text, anchor presets, the Interface panel, dragging as one undo step, nudging). The motion blur
check in `relay_lighting_render_tests` hides the demo's canvas in its copy, since text never blurs.

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
| Audio | `src/audio/`, `include/relay/audio/`, Audio source Inspector section and Audio page in `src/editor/editor_ui.cpp` |
| Shaders and materials | `src/render/shader_graph.cpp`, `src/editor/shader_graph_canvas.cpp`, `src/render/shader_language.cpp`, `src/render/materials.cpp`, `include/relay/render/shader_language.hpp`, `include/relay/render/materials.hpp`, `shaders/scene_bindings.glsl`, `tools/embed_shader_sources.cmake`, custom pipelines and post processing in `src/platform/vulkan_window.cpp`, Shader Editor in `src/editor/editor_ui.cpp`, `docs/shaders.md` |
| Sky and fog | `src/render/sky.cpp`, `include/relay/render/sky.hpp`, `RenderSky` in `scene_render.hpp`, `shaders/sky.frag`, Sky section in `src/editor/editor_ui.cpp` |
| Asset Browser and reference fields | `asset_field`…`draw_asset_browser` in `src/editor/editor_ui.cpp`, `src/editor/asset_thumbnails.cpp`, `src/render/asset_thumbnail.cpp`, `src/editor/editor_widgets.cpp` (icons, checkbox, slider) |
| Frame profiler | `src/observe/profiler.cpp`, `include/relay/observe/profiler.hpp`, Profiler panel in `src/editor/editor_ui.cpp` |
| Particles | `include/relay/scene/particles.hpp`, `src/scene/particles.cpp`, `src/particles/`, `include/relay/particles/`, `shaders/particle.*`, the particle pass in `src/platform/vulkan_window.cpp`, the Particle emitter section in `src/editor/editor_ui.cpp`, `docs/particles.md` |
| Game interface | `include/relay/scene/ui.hpp`, `src/scene/ui.cpp`, `src/ui/`, `include/relay/ui/`, `shaders/ui.*`, the UI pass in `src/platform/vulkan_window.cpp`, `src/editor/interface_preview.cpp`, Inspector UI sections and the Interface panel in `src/editor/editor_ui.cpp`, `third_party/inter/`, `docs/interface.md` |
| Native tests | `tests/engine_tests.cpp`, `tests/script_tests.cpp`, `tests/editor_*tests.cpp` |
| Bridge tests | `tools/mcp-bridge/tests/` |

When implementation changes, update this present-state summary instead of appending dated logs.
