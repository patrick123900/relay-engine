# Shaders and materials

Relay's shading language is GLSL with a few declarations Relay understands. A shader lives in a
project file ending in `.relay-shader`; a material (`.relay-material`) names a shader and sets its
parameters. Relay wraps your functions in complete shaders, so custom surfaces are lit, shadowed,
fogged and take part in global illumination and reflections like every other surface.

People edit shaders as node graphs in the **Shader Editor** (Tools → Shader editor, or
double-click a `.relay-shader` file in Assets); agents write the same files as code with
`shaders.write`. The code is the graph's saved form, so both always work on one file: see
[Shader graphs](#shader-graphs) below.

## Shader types

Every shader starts with its type:

```glsl
shader_type surface;       // how a mesh's surface looks
shader_type post_process;  // a full-screen effect on the lit scene, before tone mapping
```

Surface shaders may add render modes:

```glsl
render_mode transparent;   // blended over what is behind, using ALPHA; drawn after opaque surfaces
render_mode unshaded;      // shows ALBEDO + EMISSION as they are, without lighting
```

## Parameters

Uniforms become fields of every material that uses the shader, edited in the Inspector:

```glsl
uniform float speed = 1.0;
uniform float amount : hint_range(0.0, 1.0) = 0.5;        // a slider; an optional third value is the step
uniform int count = 3;
uniform bool flip = false;
uniform vec2 offset = vec2(0.0);
uniform vec3 tint : source_color = vec3(1.0, 0.5, 0.2);  // a color picker
uniform vec4 glow : source_color = vec4(1.0);
uniform sampler2D albedo_map : source_color;             // a color image, read as sRGB
uniform sampler2D bumps : hint_normal;                    // a normal map, flat until chosen
uniform sampler2D mask : hint_black;                      // black until chosen (hint_white: white)
```

Colors are linear inside shaders; the Inspector shows them as they look. Images are PNG or JPEG
files from the project, with mipmaps, repeating in both directions. A shader may have 64 uniforms
and 8 images. Names in capitals and names starting with `gl_` or `relay_` are reserved.

## Surface shaders

`void vertex()` runs for each vertex, in the object's own space:

| Variable | | |
| --- | --- | --- |
| `VERTEX` | `vec3` | position; move it for waves, wind or swelling |
| `NORMAL` | `vec3` | normal |
| `TANGENT` | `vec4` | tangent, with the bitangent's sign in `w` |
| `UV` | `vec2` | texture coordinates |
| `TIME` | `float` | seconds (read only) |
| `MODEL_MATRIX` | `mat4` | object to world (read only) |

`void fragment()` runs for each pixel of the surface and sets how it looks:

| Output | Default | |
| --- | --- | --- |
| `ALBEDO` | `vec3(1.0)` | base color |
| `ALPHA` | `1.0` | opacity (transparent shaders) |
| `METALLIC` | `0.0` | 0 to 1 |
| `ROUGHNESS` | `0.5` | 0 (mirror) to 1 |
| `EMISSION` | `vec3(0.0)` | light the surface gives off |
| `AO` | `1.0` | ambient occlusion |
| `NORMAL_MAP` | `vec3(0.5, 0.5, 1.0)` | tangent-space normal, as a normal map stores it |
| `NORMAL_MAP_DEPTH` | `1.0` | strength of `NORMAL_MAP` |
| `ALPHA_SCISSOR_THRESHOLD` | `0.0` | above 0, pixels with less `ALPHA` are cut out (and cast no shadow) |

It can read `UV`, `NORMAL` (world space, facing the camera on both sides), `TANGENT`,
`WORLD_POSITION`, `VIEW` (towards the camera), `CAMERA_POSITION`, `TIME`, `FRAGCOORD`,
`FRONT_FACING` and `MODEL_MATRIX`. Surfaces are drawn from both sides.

```glsl
shader_type surface;

uniform vec3 color : source_color = vec3(0.1, 0.4, 0.6);
uniform sampler2D ripples : hint_normal;

void vertex() {
    VERTEX.y += sin(VERTEX.x * 2.0 + TIME) * 0.05;
}

void fragment() {
    ALBEDO = color;
    ROUGHNESS = 0.1;
    NORMAL_MAP = texture(ripples, UV * 4.0 + TIME * 0.05).rgb;
}
```

Moved vertices cast matching shadows. A mesh only bends where it has vertices, so waves need a
finely divided mesh.

## Post-processing shaders

`void fragment()` runs for each pixel of the view and changes `COLOR`, the lit scene's color
there (linear, before exposure and tone mapping). It can read:

| | | |
| --- | --- | --- |
| `SCREEN_UV` | `vec2` | this pixel's position, 0 to 1 |
| `SCREEN_PIXEL_SIZE` | `vec2` | one pixel in `SCREEN_UV` units |
| `DEPTH` | `float` | distance from the camera plane to the surface here, in metres |
| `NORMAL` | `vec3` | world-space normal here; zero on the sky |
| `MOTION` | `vec2` | how far what is seen here moved on screen since the last frame (where it was minus where it is, in `SCREEN_UV` units): surfaces by their motion, the sky by how the camera turned |
| `TIME` | `float` | seconds |
| `DELTA_TIME` | `float` | seconds since the last frame (1/60 in captures) |
| `FRAGCOORD` | `vec4` | pixel coordinates |
| `scene_color(uv)`, `scene_depth(uv)`, `scene_normal(uv)`, `scene_motion(uv)` | | the same at another position |
| `scene_color_lod(uv, lod)` | `vec3` | the scene's color blurred: `lod` 0 is sharp and each level up is twice as blurry (a copy at half the size), up to about 7; fractions blend between levels |
| `SCREEN_TEXTURE` | `sampler2D` | the scene's colors, filtered, with those blurred levels as mip levels |

```glsl
shader_type post_process;

uniform float strength : hint_range(0.0, 1.0) = 0.5;

void fragment() {
    float grey = dot(COLOR, vec3(0.2126, 0.7152, 0.0722));
    COLOR = mix(COLOR, vec3(grey), strength);
}
```

Add effects to a **Post Process** node (Add Node → Post Process): its effects run in order before
the grid and selection outlines are drawn. Only the first Post Process node in a scene is used;
each effect can be turned off without removing it, and its **Editor view** switch decides whether
it also shows while you look around the editor. The scene camera's view (Run Game, captures)
always shows it.

Relay's blurred levels are made only for effects whose code calls `scene_color_lod`, just before
they run (a few blits, a small fraction of a millisecond).

### Ready-made effects

**Add effect** in a Post Process node's Inspector offers Relay's ready-made effects. Choosing one
copies its shader into the project's `shaders` folder and makes a material for it in `materials`
(reusing both when they are already there), so it can be opened, read and changed like any other
shader. Agents do the same with `shaders.write` (`create` and `effect`) and `assets.set_material`.
The originals are in `shaders/effects/` in the repository.

| Effect | Parameters | |
| --- | --- | --- |
| **Bloom** | `intensity`, `spread`, `threshold`, `tint` | Light spreads from bright parts into their surroundings, as in a lens. It moves light rather than adding it: `intensity` is the share that spreads, `spread` how far (wider blurs count more as it rises), and a `threshold` above 0 spreads only light brighter than it (1 is white). It blurs with `scene_color_lod` at seven levels. |
| **Color Grading** | `exposure` (stops), `contrast`, `saturation`, `temperature`, `shadows`, `highlights` | The picture's look. Contrast works in stops around mid grey; `temperature` warms (positive) or cools; `shadows` and `highlights` tint dark and bright parts. Neutral at its defaults. |
| **Vignette** | `amount`, `size`, `softness`, `color` | The picture darkens, or fades to `color`, towards its edges. `size` is where it starts, 1 being the top and bottom edges. |

The demo's **Shader showcase** has three hovering plasma orbs in front of the player's start, all
using `materials/PlasmaOrb.relay-material` (`shaders/PlasmaOrb.relay-shader`): swirling
domain-warped noise glowing inside a dark glassy shell, a bright rim, a pulse, a rippling surface
and a hover in `vertex()`. The noise helpers are in the shader's functions, so both stages open as
nodes. The green and pink orbs set their own `core_color` and `glow_color` under **This object
only**; the demo's Post Process node runs Bloom (threshold 1.5) so they glow.

The demo project's Post Process node also runs `shaders/MotionBlur.relay-shader` (through
`materials/MotionBlur.relay-material`) for the player's camera only. It smears each pixel along
`MOTION`, scaled to how long a 60 fps camera's shutter stays open (`shutter`, 0 to 1, so the look
does not change with frame rate) and capped at `max_blur` screen widths; the sampling loop is in
the shader's functions, so the rest opens as nodes.

## Materials

A material file names its shader and sets uniforms; uniforms it leaves out keep the shader's
defaults:

```json
{
  "format": "relay.material",
  "version": 1,
  "type": "surface",
  "shader": "shaders/Water.relay-shader",
  "parameters": {
    "color": [0.02, 0.2, 0.3],
    "ripples": "textures/ripples.png"
  }
}
```

Choose a surface material as a Mesh renderer's **Material** in the Inspector (or drop the file
onto it); a preview sphere, its shader and its parameters appear below. The preview shows the
material under Relay's default sky and sun (no shadows or fog), over a checkerboard so
transparency shows, and is redrawn a frame after the material or its shader changes. **Create → Material** and
**Create → Post-processing material** in Assets make new ones, as do the "New material" entries in
the pickers. Agents use `assets.set_material`, `assets.set_material_parameter` and
`assets.material`, and `scene.set_renderer` / `scene.set_post_process` to use them.

A surface whose material cannot be drawn (no shader, a shader that does not compile, or one of the
wrong type) shows a magenta checkerboard; the Inspector says why.

### Values for one object

Material parameters are shared by every object using the material. To give one object its own
value (a different tint for one crate, a door that fades out), open **This object only** under
the material's parameters in its Inspector and change the value there: it is stored with the
object in the scene (the Mesh renderer's `parameters`), undoable, and shown in full color with a
**Reset** button, while values the object takes from the material are dimmed. Numbers, vectors,
colors and booleans can be set per object; images cannot (use another material).

Scripts change the same values during the game, and Stop Game restores the authored ones:

```cpp
void on_contact_begin(relay::Entity) override {
    self().set_material_parameter("tint", relay::Vec3{1.0, 0.1, 0.1}); // this enemy only
    flash = 1.0;
}
void on_update(double dt) override {
    flash = std::max(0.0, flash - dt * 3.0);
    self().set_material_parameter("flash", flash);
}
```

`material_parameter(name)` reads the current value (the object's own, else the material's) and
`clear_material_parameter(name)` goes back to the material's. A wrong name or number of values
returns false and logs why. Agents use `scene.set_renderer_parameter` (with `clear` to remove a
value). Objects with their own values cost a few bytes each per frame; nothing is rebuilt when
they change, so animating them every frame is fine.

## Shader graphs

Opening a shader in the Shader Editor turns its code into nodes. Each built-in a stage reads (UV,
TIME, NORMAL...) and each parameter is a node; arithmetic, comparisons, `?:`, swizzles (Split),
component assignments (Set components) and function calls (GLSL's and the shader's own) are nodes
wired by what they use; numbers and other constants become values on unconnected pins; what the
stage assigns (ALBEDO, VERTEX, COLOR...) goes into its output node. Local variable names become node
names.

- **Add nodes**: right-click empty space, or press Space, and search. Dropping a wire on empty space
  opens the same menu and connects the new node.
- **Wire**: drag from an output (right) to an input (left), or back. Dragging a connected input
  picks its wire up to move or drop it. Wires that would make a loop are refused.
- **Values**: type a GLSL constant (`0.5`, `vec3(1.0, 0.2, 0.1)`, `true`) in an unconnected pin;
  output pins left empty keep the built-in's default.
- **Select and move**: click or box-select, Ctrl adds; drag to move; Delete removes; F frames the
  graph; the middle or right button pans and the wheel zooms.
- **Copy, paste and duplicate**: Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste the selected nodes
  (also between shaders, adding any parameters the other shader lacks; inputs it does not have are
  left out); Ctrl+D duplicates them just below the originals. Copies keep the wires between themselves; a
  duplicate also keeps the wires coming into it, a paste does not. A node's right-click menu and
  the add-node menu have the same commands. Calls to the other shader's own functions need those
  functions copied into its side panel too.
- **Stages**: Fragment (surface) and Vertex (shape) are separate graphs; post-processing shaders
  have one.
- **Side panel**: parameters (add, rename, remove, put on the canvas), render modes, and the
  shader's functions: GLSL helper functions and constants that nodes call by name.
- **Undo and redo** (Ctrl+Z, Ctrl+Y), **Save** (Ctrl+S) and **Revert**; every change previews in
  the viewport a moment later. Compile errors outline the node they come from.
- **Changes from elsewhere**: when an agent (or another program) rewrites an open shader, a tab
  without unsaved edits shows the new version at once (the old one stays in its undo history); a
  tab with unsaved edits keeps them and offers **Load the new version** or **Keep mine** (saving
  then replaces the other version).

Relay writes the graph back as one local variable per node, then the stage's outputs, and keeps
node positions in a last line, `// relay-graph {...}`, which the compiler ignores. Agents' code
converts as long as each stage is straight-line assignments; a stage that uses `if`, loops,
`return` or arrays stays code and is edited as code in its place, and a file whose declarations do
not parse is edited as text until they do. Comments inside a converted stage are not kept.

## Limits

- Ray traced reflections show custom surfaces in plain grey; their shaders only run on screen.
- Per-object values cover numbers, vectors, colors and booleans, not images; the material preview
  shows the material's own values.
- Effects run one after another over the whole image; there is no per-camera stack beyond the
  Editor view switch.
- Motion vectors ignore vertex movement, so fast `vertex()` animation can smear slightly with
  global illumination.
- The selection outline follows the mesh without its vertex movement.
- Shaders compile on the engine thread when their file (or preview) changes, in about a tenth of
  a second, and the renderer waits for the GPU once to swap in the new pipelines.
- Builds without glslang cannot compile shaders and say so.
