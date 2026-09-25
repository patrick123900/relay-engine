#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D scene_hdr;
// The scene image covers only the viewport, whose top-left pixel and size are given here.
layout(push_constant) uniform DisplaySettings {
    float exposure;
    uint encode_srgb;
    vec2 viewport_offset;
    vec2 viewport_size;
} settings;

vec3 aces_fitted(vec3 value) {
    // ACES filmic fit in linear display primaries, with a bounded output for SDR surfaces.
    return clamp((value * (2.51 * value + 0.03)) /
                 (value * (2.43 * value + 0.59) + 0.14), 0.0, 1.0);
}

vec3 linear_to_srgb(vec3 value) {
    return mix(value * 12.92, 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055,
               greaterThan(value, vec3(0.0031308)));
}

void main() {
    vec2 scene_uv = (gl_FragCoord.xy - settings.viewport_offset) / settings.viewport_size;
    vec3 mapped = aces_fitted(max(texture(scene_hdr, scene_uv).rgb, vec3(0.0)) *
                              settings.exposure);
    color = vec4(settings.encode_srgb != 0u ? linear_to_srgb(mapped) : mapped, 1.0);
}
