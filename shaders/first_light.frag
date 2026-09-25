#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 texture_coordinates;
layout(location = 1) in vec3 surface_normal;
layout(location = 2) in vec4 surface_tangent;
layout(location = 3) flat in uint material_index;
layout(location = 4) in vec3 world_position;
layout(location = 5) in vec4 current_clip;
layout(location = 6) in vec4 previous_clip;
layout(location = 0) out vec4 output_color;
// The opaque geometry pass also writes the G-buffer that screen-space lighting effects read. The
// forward pass for transparent geometry has only the color attachment, so these writes are dropped.
layout(location = 1) out vec4 output_normal_roughness;
layout(location = 2) out vec4 output_albedo_metallic;
layout(location = 3) out vec4 output_motion_occlusion;
layout(location = 4) out float output_depth;
// Direct diffuse light and emission: the view-independent radiance that global illumination
// reprojects next frame to light the surfaces its rays hit.
layout(location = 5) out vec4 output_diffuse_light;

// True for the opaque geometry pass.
layout(constant_id = 0) const bool geometry_pass = false;

#define SURFACE_TEXTURE(index, uv) texture(textures[index], uv)
#include "surface_lighting.glsl"

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

void main() {
    MaterialData material = materials[material_index];
    vec4 base_color = material.base_color_factor;
    if (material.texture_indices.x != missing_texture) {
        base_color *= texture(textures[material.texture_indices.x], texture_coordinates);
    }
    if (surface_cut_out(material_index, base_color.a)) discard;
    Surface surface = evaluate_surface(material_index, texture_coordinates, surface_normal,
                                       surface_tangent, gl_FrontFacing);

    vec3 view_direction = normalize(lighting.camera_count.xyz-world_position);
    vec3 direct_color;
    vec3 direct_diffuse;
    direct_lighting(surface, world_position, view_direction, direct_color, direct_diffuse);
    vec3 emissive = surface.emissive;
    // Bit 0 of camera_forward.w: global illumination provides all indirect light. Bit 1: ray
    // traced reflections replace the environment's specular half. Both apply to opaque geometry,
    // which the composite pass later completes; transparent geometry keeps the analytic sky.
    uint indirect = geometry_pass ? uint(lighting.camera_forward.w) : 0u;
    vec3 ambient = (indirect & 1u) != 0u ? vec3(0.0)
                 : (indirect & 2u) != 0u ? ambient_diffuse(surface) * surface.occlusion
                                         : ambient_lighting(surface, view_direction);
    vec3 color = ambient + direct_color + emissive;
    output_color = vec4(color, surface.base_color.a);
    // Motion is the offset from this pixel to where the surface was last frame, in UV units.
    vec2 current_uv = current_clip.xy / current_clip.w * 0.5 + 0.5;
    vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
    output_normal_roughness = vec4(surface.normal, surface.roughness);
    output_albedo_metallic = vec4(surface.base_color.rgb, surface.metallic);
    output_motion_occlusion = vec4(previous_uv - current_uv, surface.occlusion, 0.0);
    output_depth = gl_FragCoord.z;
    output_diffuse_light = vec4(direct_diffuse + emissive, 1.0);
}
