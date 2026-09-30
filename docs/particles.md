# Particles

Relay draws sparks, smoke, fire, dust, rain, magic, trails and explosions with **particle
emitters**, much as Unity does with its Particle System and Godot with GPUParticles3D. An emitter
spawns small camera-facing sprites from a shape, gives each one a lifetime, speed, size, rotation
and color, moves them with gravity, drag, turbulence and collisions, and changes their size, speed
and color over their lives. Emitters play in the editor as well as the game, so an effect can be
tuned while it runs.

## Adding an emitter

**Add Node → Particle Emitter** creates a node with a `particle_emitter` component, or add the
component to any node with **+ Add Component** (under **Effects**). The node's position, rotation
and scale place the emitter; its shape emits along the node's **+Y** axis, so rotate the node to aim
it. Selecting an emitter outlines its shape in the viewport, and every emitter has an orange
sparkle icon you can click.

At the top of the Inspector section, **Pause**, **Restart**, **Stop** and **Burst** control the
preview, beside how many particles are alive. In the editor every emitter previews continuously;
a one-shot effect replays a second after it finishes. Stop lets the living particles finish their
lives, and Burst adds 30 particles at once. Particles are not scene data: they are not saved or
undone, and they never block editing.

## Settings

The Inspector groups the settings. Fields that do nothing with the current settings (a box's size
on a cone, collision bounce without collisions) are hidden. Hover a field for what it does.

| Group | Settings |
| --- | --- |
| Emitter | **Play on start** (otherwise a script starts it), **Looping**, **Duration** of one cycle, **Prewarm** (start as if a cycle had run), **Start delay**, **Max particles** (up to 100,000), **Simulation space**, **Simulation speed** and **Seed**. |
| Emission | **Rate** per second, **Rate over distance** per metre the node moves (for trails), and **Bursts**. |
| Shape | **Point**, **Sphere**, **Hemisphere**, **Cone**, **Box**, **Circle** or **Edge**, with its radius, **Radius thickness** (0 emits from the surface or rim only), cone **Angle**, **Arc**, box size, an offset and rotation within the node, **Direction randomness** and **Spherize**. |
| Start | **Lifetime**, **Speed**, **Size** and **Rotation**, each picked at random between a min and a max; **Aspect** (width over height), **Angular velocity**, **Color**, and **Random color** to mix each particle between Color and **Color alt**; **Inherit velocity** from the moving node. |
| Motion | **Gravity** (a multiple of 9.81 m/s² down; negative rises), **Acceleration** in world space (wind), **Velocity** added in simulation space, **Drag**, **Speed over lifetime** and turbulence: **Noise strength**, **frequency** and **scroll**. |
| Over lifetime | **Size over lifetime** (a curve) and **Color over lifetime** (a gradient that multiplies color and alpha). |
| Collision | **None**, **Plane** (a floor at **Plane height**) or **World** (the scene's colliders, during Run Game), with **Bounce**, **Friction**, **Lifetime loss** per hit and a **Collision radius**. |
| Flipbook | A texture split into **Columns** × **Rows** frames played **Over lifetime** (**Cycles** times), at **FPS**, or picked at **Random**; **Random start** and **Blend** between frames. |
| Renderer | **Texture** (a project PNG or JPEG) or a **Built-in texture** (soft dot, dot, ring, star, smoke, spark, square); **Blend**; **Alignment**; **Stretch speed** and **Stretch length**; **Emission** (brightness); **Lit**; **Soft distance**; **Sort**. |
| Sub emitter | A child node's emitter, by name, fired where particles are born, collide or die, with a **Count** and how much velocity the new particles **Inherit**. |

### Curves, gradients and bursts

The curve editor plots a value from birth (left) to death (right). Double-click to add a key, drag
keys to move them, right-click a key to remove it, or right-click the graph for presets (grow,
shrink, grow then shrink, pop in). A curve without keys is 1 throughout.

The gradient bar shows color and alpha over a particle's life over a checkerboard. Click a key
below the bar to edit its color and time underneath, drag it along, double-click the bar to add
one, right-click a key to remove it, or right-click the bar for presets (fade out, fade in and out,
fire, smoke). The gradient multiplies the start color, so a white gradient keeps it as it is.

Each burst emits **Count** particles at **Time** seconds into every cycle, **Cycles** times (0
repeats it all cycle long) every **Interval** seconds, each time with the given **Chance**.

### Spaces, blending and alignment

**World** simulation space leaves particles behind as the node moves: smoke from a moving car, a
trail behind a ball. **Local** carries them with the node, like a jet's flame.

**Alpha** blending covers what is behind: smoke, dust, and fire in daylight, where brightness above 1
keeps its color strong. **Additive** adds light, for sparks, magic and glows; it fades out in fog and
never darkens. **Premultiplied** is for textures whose colors are already multiplied by their alpha.

**Billboard** sprites face the camera and turn by their rotation; **Stretched** sprites streak along
their velocity, longer the faster they go (with the spark texture, for sparks and rain);
**Horizontal** ones lie flat (ripples, decals); **Vertical** ones stand upright and turn to the
camera (grass, flames seen from above); **Local** ones face along the node's Z axis.

**Emission** multiplies the color. Above 1 particles glow, and a Post Process **Bloom** effect
spreads the glow. **Lit** particles take the scene's lights, shadows and sky like rough spheres,
which suits smoke and dust. **Soft distance** fades particles out where they meet surfaces behind
them, so smoke does not cut sharp lines into the ground. Particles fog like transparent surfaces.
**Sort** draws the farthest first (right for alpha blending), or the oldest or youngest first.

## Sub emitters

Give an emitter a child node with its own emitter and name that child in **Sub emitter**. The child
then never emits on its own: it fires **Count** particles from its own shape wherever one of the
parent's particles dies, collides or is born, adding the share of that particle's velocity given by
**Inherit**. The demo's fireworks are a rocket emitter (one particle every 1.4 s) whose child
**Burst** throws out 160 colored stars where each rocket dies.

## Scripts

Scripts control an entity's emitter during Run Game (see the [scripting guide](scripting.md)):

```cpp
explosion.emit_particles(80);         // a burst now, from the emitter's shape
torch.play_particles();               // start its cycle (restart: true also clears it)
torch.stop_particles();               // stop emitting; living particles finish (clear: true removes them)
torch.pause_particles(true);
torch.set_particles("rate", 120.0);   // any field by name, as the protocol names it
torch.set_particles("color", {1.0, 0.4, 0.1, 1.0});
torch.set_particles("blend", "additive");
const auto alive = torch.particle_count();
```

Field values are checked like the Inspector's; a value that does not fit is refused and logged.
Changes last until Stop Game. Curves, gradients and bursts are edited in the scene.

## Agents and the protocol

`scene.set_particle_emitter` adds, edits or removes an emitter with a `values` object of field
names, as undoable scene edits; `component.types` lists every field with its type, group, range,
choices and default. `particles.status` reports each emitter's state and particle count, and
`particles.control` plays, restarts, stops, clears, pauses, resumes or emits a burst, in the editor
or during the game. Check an effect by capturing the viewport; emitters play in the editor, so a
capture shows them.

## Limits

- Particles are simulated on the CPU, one fixed game step at a time, and deterministic for a seed.
  An emitter holds at most 100,000 particles and the scene at most 1,000,000.
- World collisions cast about 4,096 rays per step across all emitters; with more colliding
  particles each is tested every few steps along the path it took since, so fast particles can
  slip through thin colliders. Editor previews collide with planes only.
- Particles are drawn after transparent surfaces, sorted within each emitter and emitters sorted
  by distance; they do not interleave with transparent meshes, cast shadows, write motion vectors,
  appear in ray traced reflections or take part in global illumination.
- There are no trails or ribbons, mesh particles, GPU simulation, attractors or per-particle
  lights yet.
