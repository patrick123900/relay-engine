#version 450

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) flat out uint material_index;

void main() {
    gl_Position = frame.model_view_projection * vec4(position, 1.0);
    texture_coordinates = uv;
    material_index = gl_InstanceIndex;
}
