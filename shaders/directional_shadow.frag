#version 450

layout(location = 0) in vec2 texture_coordinates;
layout(location = 1) flat in uint material_index;

layout(set = 0, binding = 0) uniform sampler2D textures[16];
struct MaterialData {
    vec4 base_color_factor;
    vec4 emissive_metallic;
    vec4 surface_parameters;
    uvec4 texture_indices;
};
layout(std430, set = 0, binding = 1) readonly buffer MaterialBuffer {
    MaterialData materials[];
};
const uint missing_texture = 0xffffffffu;

void main() {
    MaterialData material = materials[material_index];
    uint alpha_mode = (material.texture_indices.w >> 16u) & 0x3u;
    if (alpha_mode != 1u) return;
    float alpha = material.base_color_factor.a;
    if (material.texture_indices.x != missing_texture)
        alpha *= texture(textures[material.texture_indices.x], texture_coordinates).a;
    if (alpha < material.surface_parameters.w) discard;
}
