#version 450
layout(push_constant) uniform Selection { mat4 mvp; mat4 parameters; } selection;
layout(location=0) in vec2 texture_coordinates;
layout(location=1) flat in uint material_index;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform sampler2D textures[16];
struct MaterialData { vec4 base_color_factor; vec4 emissive_metallic; vec4 surface_parameters; uvec4 texture_indices; };
layout(std430,set=0,binding=1) readonly buffer MaterialBuffer { MaterialData materials[]; };
void main() {
    MaterialData material = materials[material_index];
    float alpha = material.base_color_factor.a;
    if (material.texture_indices.x != 0xffffffffu)
        alpha *= texture(textures[material.texture_indices.x], texture_coordinates).a;
    uint alpha_mode = (material.texture_indices.w >> 16u) & 3u;
    if (alpha_mode == 1u && alpha < material.surface_parameters.w) discard;
    if (alpha_mode == 2u && alpha < 0.05) discard;
    color = vec4(selection.parameters[1].rgb,1.0);
}
