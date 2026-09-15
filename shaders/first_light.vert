#version 450

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    vec4 tint;
    uint texture_index;
} frame;

layout(location = 0) out vec3 color;
layout(location = 1) out vec2 texture_coordinates;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;

void main() {
    gl_Position = frame.model_view_projection * vec4(position, 1.0);
    const vec3 vertex_accent[3] = vec3[](vec3(1.0, 0.88, 0.78),
                                         vec3(0.72, 1.0, 1.0),
                                         vec3(0.92, 0.78, 1.0));
    color = frame.tint.rgb * vertex_accent[gl_VertexIndex % 3];
    texture_coordinates = uv;
}
