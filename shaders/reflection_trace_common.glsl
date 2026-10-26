// What the reflection trace (reflection_trace.comp) and the per-shader hit kernels Relay generates
// for custom surface shaders share: the ray list and the buffers a hit is rebuilt from, the pixel's
// reflected ray, and the outputs. Included after surface_lighting.glsl.
//
// RELAY_TRACE_SET is the descriptor set the bindings live in: 1 for the trace, 2 for a hit kernel,
// whose set 1 holds its material.

#ifndef RELAY_TRACE_SET
#define RELAY_TRACE_SET 1
#endif

layout(std430, set = RELAY_TRACE_SET, binding = 1) readonly buffer RayList { uint rays[]; };
layout(std430, set = RELAY_TRACE_SET, binding = 2) readonly buffer RayCounter { uint counter[]; };
layout(set = RELAY_TRACE_SET, binding = 3, rgba16f) uniform writeonly image2D radiance_output;
layout(set = RELAY_TRACE_SET, binding = 4, r16f) uniform writeonly image2D variance_output;
layout(set = RELAY_TRACE_SET, binding = 5) uniform sampler2D depth_texture;
layout(set = RELAY_TRACE_SET, binding = 6) uniform sampler2D normal_roughness_texture;
layout(set = RELAY_TRACE_SET, binding = 7) uniform sampler2D blue_noise;
layout(std430, set = RELAY_TRACE_SET, binding = 8) readonly buffer StaticVertices { float static_vertices[]; };
layout(std430, set = RELAY_TRACE_SET, binding = 9) readonly buffer DeformedVertices { float deformed_vertices[]; };
layout(std430, set = RELAY_TRACE_SET, binding = 10) readonly buffer Indices { uint indices[]; };
struct InstanceData {
    uint material;
    uint first_index;
    int vertex_offset;
    // 0 static, 1 deformed, 2 particle sprites.
    uint deformed;
    // x: 1 + the custom surface shader slot that shades hits on this instance, or 0.
    uvec4 extra;
    mat4 model;
};
layout(std430, set = RELAY_TRACE_SET, binding = 11) readonly buffer Instances { InstanceData instances[]; };
// Where a ray hit a custom shader's surface, four words per ray (by position in the ray list):
// the instance's index, the primitive, the barycentrics as two 16-bit fractions and the distance
// as float bits. The trace writes it for every ray when custom shaders are in the scene; the first
// word is no_hit for a ray with nothing for a hit kernel to do.
layout(std430, set = RELAY_TRACE_SET, binding = 16) buffer HitRecords { uint hit_records[]; };
const uint no_hit = 0xFFFFFFFFu;

layout(push_constant) uniform TraceData {
    mat4 inverse_view_projection;
    vec4 camera_position;
    // x, y: target size; z: frame index; w: particle batches, with bit 31 set when custom
    // shaders keep hit records.
    uvec4 extent;
    // x: the custom shader slot a hit kernel shades.
    uvec4 hit;
} trace;

const uint floats_per_vertex = 12u;

// Samples a visible GGX microfacet normal (Heitz 2018, "Sampling the GGX Distribution of Visible
// Normals"). `view` is in the tangent frame with z along the surface normal.
vec3 sample_visible_normal(vec3 view, float alpha, vec2 u) {
    vec3 stretched = normalize(vec3(alpha * view.x, alpha * view.y, view.z));
    float length_squared = dot(stretched.xy, stretched.xy);
    vec3 t1 = length_squared > 0.0 ? vec3(-stretched.y, stretched.x, 0.0) * inversesqrt(length_squared)
                                   : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(stretched, t1);
    float r = sqrt(u.x);
    float phi = 2.0 * pi * u.y;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + stretched.z);
    p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * p2;
    vec3 normal = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * stretched;
    return normalize(vec3(alpha * normal.x, alpha * normal.y, max(0.0, normal.z)));
}

vec3 vertex_vector(InstanceData instance, uint vertex, uint offset) {
    uint base = vertex * floats_per_vertex + offset;
    return instance.deformed != 0u
               ? vec3(deformed_vertices[base], deformed_vertices[base + 1u], deformed_vertices[base + 2u])
               : vec3(static_vertices[base], static_vertices[base + 1u], static_vertices[base + 2u]);
}

vec4 vertex_vector4(InstanceData instance, uint vertex, uint offset) {
    uint base = vertex * floats_per_vertex + offset;
    return instance.deformed != 0u
               ? vec4(deformed_vertices[base], deformed_vertices[base + 1u],
                      deformed_vertices[base + 2u], deformed_vertices[base + 3u])
               : vec4(static_vertices[base], static_vertices[base + 1u], static_vertices[base + 2u],
                      static_vertices[base + 3u]);
}

vec2 vertex_uv(InstanceData instance, uint vertex) {
    uint base = vertex * floats_per_vertex + 3u;
    return instance.deformed != 0u ? vec2(deformed_vertices[base], deformed_vertices[base + 1u])
                                   : vec2(static_vertices[base], static_vertices[base + 1u]);
}

void store(ivec2 pixel, vec4 value) {
    imageStore(radiance_output, pixel, value);
    imageStore(variance_output, pixel, vec4(value.w));
}

// The reflected ray for a listed pixel: about a GGX microfacet normal drawn with this frame's blue
// noise, from just off the surface the pixel shows. Both kernels call it, so a hit kernel finds the
// ray the trace cast.
void reflection_ray(ivec2 pixel, out vec3 origin, out vec3 direction) {
    float depth = texelFetch(depth_texture, pixel, 0).r;
    vec4 normal_roughness = texelFetch(normal_roughness_texture, pixel, 0);
    vec2 uv = (vec2(pixel) + 0.5) / vec2(trace.extent.xy);
    vec4 world = trace.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 position = world.xyz / world.w;
    vec3 normal = normalize(normal_roughness.xyz);
    vec3 view_direction = normalize(trace.camera_position.xyz - position);
    float alpha = normal_roughness.w * normal_roughness.w;

    vec3 helper = abs(normal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);
    vec3 local_view = vec3(dot(view_direction, tangent), dot(view_direction, bitangent),
                           dot(view_direction, normal));
    // Blue noise, shifted by the R2 sequence every frame so temporal accumulation keeps seeing
    // new directions rather than cycling through a few patterns.
    vec2 noise = fract(texelFetch(blue_noise, pixel & 127, 0).rg +
                       float(trace.extent.z & 0xFFFFu) * vec2(0.7548776662, 0.5698402910));
    vec3 local_half = sample_visible_normal(local_view, max(alpha, 1e-4), noise);
    vec3 half_vector = normalize(tangent * local_half.x + bitangent * local_half.y + normal * local_half.z);
    direction = reflect(-view_direction, half_vector);
    if (dot(direction, normal) <= 0.0) direction = reflect(-view_direction, normal);

    // Push the origin off the surface in proportion to its distance from the camera.
    float camera_distance = length(trace.camera_position.xyz - position);
    origin = position + normal * max(0.002, camera_distance * 0.0015);
}
