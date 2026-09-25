#version 450

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

// Per-draw data, indexed by the draw's first instance. The previous frame's transform gives the
// motion vectors that temporal lighting effects reproject with.
struct DrawData {
    mat4 previous_model_view_projection;
    uvec4 material;
};
layout(std430, set = 0, binding = 5) readonly buffer DrawBuffer {
    DrawData draws[];
};

layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) out vec3 surface_normal;
layout(location = 2) out vec4 surface_tangent;
layout(location = 3) flat out uint material_index;
layout(location = 4) out vec3 world_position;
layout(location = 5) out vec4 current_clip;
layout(location = 6) out vec4 previous_clip;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 tangent;

void main() {
    gl_Position = frame.model_view_projection * vec4(position, 1.0);
    current_clip = gl_Position;
    previous_clip = draws[gl_InstanceIndex].previous_model_view_projection * vec4(position, 1.0);
    world_position=(frame.model*vec4(position,1.0)).xyz;
    texture_coordinates = uv;
    mat3 normal_matrix = transpose(inverse(mat3(frame.model)));
    surface_normal = normalize(normal_matrix * normal);
    surface_tangent = vec4(normalize(mat3(frame.model) * tangent.xyz), tangent.w);
    material_index = draws[gl_InstanceIndex].material.x;
}
