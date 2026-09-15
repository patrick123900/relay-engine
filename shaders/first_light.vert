#version 450

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) out vec3 surface_normal;
layout(location = 2) out vec4 surface_tangent;
layout(location = 3) flat out uint material_index;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 tangent;

void main() {
    gl_Position = frame.model_view_projection * vec4(position, 1.0);
    texture_coordinates = uv;
    mat3 normal_matrix = transpose(inverse(mat3(frame.model)));
    surface_normal = normalize(normal_matrix * normal);
    surface_tangent = vec4(normalize(mat3(frame.model) * tangent.xyz), tangent.w);
    material_index = gl_InstanceIndex;
}
