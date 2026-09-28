#version 450
// The game interface: triangles in view pixels from UiDrawList, placed in the game view.
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(push_constant) uniform Placement {
    vec2 scale;     // View pixels to clip space.
    vec2 translate; // The view's top-left corner in clip space.
    uint linear_output;
} placement;

void main() {
    out_uv = uv;
    out_color = color;
    gl_Position = vec4(position * placement.scale + placement.translate, 0.0, 1.0);
}
