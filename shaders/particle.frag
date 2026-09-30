#version 450
#extension GL_GOOGLE_include_directive : require

// Particle sprites in the forward pass, after transparent geometry. Output is premultiplied, so
// one blend state serves alpha (covering), additive (alpha 0 adds light) and premultiplied
// textures. Particles fade out near surfaces behind them (soft particles) and into the fog; lit
// particles take the scene's lights, shadows and sky like a rough sphere.

#define SURFACE_TEXTURE(index, uv) texture(textures[index], uv)
#include "surface_lighting.glsl"

layout(set = 1, binding = 3) uniform sampler2D depth_texture;

layout(set = 2, binding = 0) uniform ParticleFrame {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 camera_position;
    vec4 camera_right;
    vec4 camera_up;
    vec4 camera_forward;
    vec4 extent;
} frame;

layout(set = 3, binding = 0) uniform sampler2D sprite_texture;

layout(push_constant) uniform Batch {
    vec4 local_x;
    vec4 local_y;
    vec4 sheet;
    vec4 mode;
} batch;

layout(location = 0) in vec2 frame_uv;
layout(location = 1) in vec2 next_frame_uv;
layout(location = 2) in float frame_blend;
layout(location = 3) in vec4 sprite_color;
layout(location = 4) in vec3 world_position;
layout(location = 5) in vec3 sprite_right;
layout(location = 6) in vec3 sprite_up;
layout(location = 7) in vec2 corner;

layout(location = 0) out vec4 output_color;

void main() {
    vec4 texel = texture(sprite_texture, frame_uv);
    if (frame_blend > 0.0) texel = mix(texel, texture(sprite_texture, next_frame_uv), frame_blend);
    uint blend = uint(batch.mode.x + 0.5);
    // Straight color and coverage; premultiplied textures already carry their alpha in rgb.
    vec3 color = sprite_color.rgb * texel.rgb;
    float alpha = sprite_color.a * texel.a;
    if (alpha <= 0.002 && blend != 2u) discard;

    if (batch.mode.y > 0.5) {
        // A sphere's normal across the sprite, so light wraps round each puff.
        vec3 facing = normalize(cross(sprite_right, sprite_up));
        vec3 view_direction = normalize(frame.camera_position.xyz - world_position);
        if (dot(facing, view_direction) < 0.0) facing = -facing;
        float edge = clamp(dot(corner, corner), 0.0, 1.0);
        Surface surface;
        surface.base_color = vec4(blend == 2u ? color / max(alpha, 0.001) : color, 1.0);
        surface.metallic = 0.0;
        surface.roughness = 1.0;
        surface.normal = normalize(sprite_right * corner.x + sprite_up * corner.y + facing * sqrt(1.0 - edge));
        surface.occlusion = 1.0;
        surface.emissive = vec3(0.0);
        vec3 total, diffuse;
        direct_lighting(surface, world_position, view_direction, total, diffuse);
        vec3 lit = total + ambient_diffuse(surface);
        color = blend == 2u ? lit * alpha : lit;
    }

    // Soft particles: fade out as the surface behind comes within the soft distance.
    float soft = batch.sheet.w;
    if (soft > 0.0) {
        float depth = texelFetch(depth_texture, ivec2(gl_FragCoord.xy), 0).r;
        if (depth < 1.0) {
            vec2 ndc = gl_FragCoord.xy / frame.extent.xy * 2.0 - 1.0;
            vec4 behind = frame.inverse_view_projection * vec4(ndc, depth, 1.0);
            vec3 surface_position = behind.xyz / behind.w;
            float gap = dot(surface_position - world_position, frame.camera_forward.xyz);
            alpha *= clamp(gap / soft, 0.0, 1.0);
            if (blend == 2u) color *= clamp(gap / soft, 0.0, 1.0);
        }
    }

    float fog = fog_amount(length(world_position - frame.camera_position.xyz));
    if (blend == 1u) {
        // Added light fades out into the fog.
        output_color = vec4(color * alpha * (1.0 - fog), 0.0);
    } else if (blend == 2u) {
        output_color = vec4(mix(color, fog_color(fog) * alpha, fog), alpha);
    } else {
        output_color = vec4(mix(color, fog_color(fog), fog) * alpha, alpha);
    }
}
