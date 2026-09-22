#version 450

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

layout(location = 0) out vec2 texture_coordinates;
layout(location = 1) out vec3 surface_normal;
layout(location = 2) out vec4 surface_tangent;
layout(location = 3) flat out uint material_index;
layout(location = 4) out vec3 world_position;
layout(location = 5) out vec4 shadow_position;
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 normal;
layout(location = 3) in vec4 tangent;

struct LightData { vec4 position_type; vec4 direction_inner; vec4 color_intensity; vec4 attenuation_outer; vec4 range; };
layout(std430,set=0,binding=2) readonly buffer LightingBuffer {
    vec4 camera_count;
    LightData lights[16];
    mat4 shadow_view_projection;
    uvec4 shadow_parameters;
} lighting;

void main() {
    gl_Position = frame.model_view_projection * vec4(position, 1.0);
    world_position=(frame.model*vec4(position,1.0)).xyz;
    texture_coordinates = uv;
    mat3 normal_matrix = transpose(inverse(mat3(frame.model)));
    surface_normal = normalize(normal_matrix * normal);
    surface_tangent = vec4(normalize(mat3(frame.model) * tangent.xyz), tangent.w);
    material_index = gl_InstanceIndex;
    shadow_position = lighting.shadow_view_projection * vec4(world_position, 1.0);
}
