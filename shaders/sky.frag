#version 450
#extension GL_GOOGLE_include_directive : require

// Draws the scene's sky behind everything and fogs opaque surfaces. A fullscreen pass in the
// forward render pass, after the composite has completed opaque light and before transparent
// geometry, blending premultiplied: the sky replaces empty pixels, fog covers surfaces by distance.

layout(location = 0) out vec4 output_color;

#define SURFACE_TEXTURE(index, uv) texture(textures[index], uv)
#include "surface_lighting.glsl"

layout(set = 1, binding = 3) uniform sampler2D depth_texture;

// The composite pass's push constants, of which the sky reads the first three.
layout(push_constant) uniform SkyData {
    mat4 inverse_view_projection;
    vec4 camera_position;
    vec4 extent;
} view;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depth_texture, pixel, 0).r;
    vec2 ndc = (vec2(pixel) + 0.5) / view.extent.xy * 2.0 - 1.0;
    vec4 far_point = view.inverse_view_projection * vec4(ndc, depth >= 1.0 ? 1.0 : depth, 1.0);
    vec3 position = far_point.xyz / far_point.w;
    if (depth >= 1.0) {
        // From the near plane rather than the camera, so orthographic views look along the view.
        vec4 near_point = view.inverse_view_projection * vec4(ndc, 0.0, 1.0);
        vec3 direction = normalize(position - near_point.xyz / near_point.w);
        output_color = vec4(visible_sky(direction), 1.0);
        return;
    }
    float amount = fog_amount(length(position - view.camera_position.xyz));
    if (amount <= 0.0) discard;
    output_color = vec4(fog_color(amount) * amount, amount);
}
