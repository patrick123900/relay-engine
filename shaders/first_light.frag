#version 450

layout(location = 0) in vec3 color;
layout(location = 1) in vec2 texture_coordinates;
layout(location = 0) out vec4 output_color;

layout(set = 0, binding = 0) uniform sampler2D textures[16];

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    vec4 tint;
    uint texture_index;
} frame;

void main() {
    output_color = texture(textures[frame.texture_index], texture_coordinates) * vec4(color, 1.0);
}
