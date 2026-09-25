#version 450

// Completes the opaque scene's indirect light after the geometry pass. Runs as a fullscreen pass
// over the HDR target with additive blending. Global illumination supplies diffuse light and
// specular light on rough surfaces; ray traced reflections supply specular light on smooth ones.
// Without global illumination, the geometry pass already added the analytic sky's diffuse half,
// and rough surfaces get its specular half here.

layout(location = 0) out vec4 output_color;

layout(set = 0, binding = 0) uniform sampler2D normal_roughness_texture;
layout(set = 0, binding = 1) uniform sampler2D albedo_metallic_texture;
layout(set = 0, binding = 2) uniform sampler2D motion_occlusion_texture;
layout(set = 0, binding = 3) uniform sampler2D depth_texture;
layout(set = 0, binding = 4) uniform sampler2D diffuse_gi_texture;
layout(set = 0, binding = 5) uniform sampler2D specular_gi_texture;
layout(set = 0, binding = 6) uniform sampler2D reflection_texture;

layout(push_constant) uniform CompositeData {
    mat4 inverse_view_projection;
    vec4 camera_position;
    // xy: scene target size in pixels. z: 1 global illumination, 2 reflections, 3 both.
    // w: the GGX alpha below which traced reflections apply.
    vec4 extent;
} composite;

const vec3 sky_radiance = vec3(0.20, 0.31, 0.48);
const vec3 ground_radiance = vec3(0.055, 0.047, 0.039);

// Split-sum environment BRDF, approximated analytically (Karis, "Physically Based Shading on
// Mobile"): scale and bias applied to F0.
vec2 environment_brdf(float roughness, float n_dot_v) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// The analytic sky's specular half, as first_light.frag's ambient_specular computes it.
vec3 analytic_specular(vec3 normal, vec3 base_color, float metallic, float roughness,
                       vec3 view_direction) {
    vec3 reflection = reflect(-view_direction, normal);
    vec3 environment_specular = mix(ground_radiance, sky_radiance,
                                    mix(clamp(reflection.y * 0.5 + 0.5, 0.0, 1.0), 0.5, roughness));
    vec3 environment_fresnel = mix(vec3(0.04), base_color, metallic) +
                               (1.0 - mix(vec3(0.04), base_color, metallic)) *
                               pow(1.0 - max(dot(normal, view_direction), 0.0), 5.0);
    return environment_fresnel * environment_specular * (1.0 - roughness * 0.5);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(depth_texture, pixel, 0).r;
    if (depth >= 1.0) discard;
    uint flags = uint(composite.extent.z);
    bool global_illumination = (flags & 1u) != 0u;
    bool reflections = (flags & 2u) != 0u;
    vec4 normal_roughness = texelFetch(normal_roughness_texture, pixel, 0);
    vec4 albedo_metallic = texelFetch(albedo_metallic_texture, pixel, 0);
    float occlusion = texelFetch(motion_occlusion_texture, pixel, 0).z;

    vec2 uv = (vec2(pixel) + 0.5) / composite.extent.xy;
    vec4 world = composite.inverse_view_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec3 position = world.xyz / world.w;
    vec3 normal = normalize(normal_roughness.xyz);
    vec3 view_direction = normalize(composite.camera_position.xyz - position);
    float n_dot_v = clamp(dot(normal, view_direction), 0.001, 1.0);
    float roughness = normal_roughness.w;

    vec3 base_color = albedo_metallic.rgb;
    float metallic = albedo_metallic.a;
    vec3 f0 = mix(vec3(0.04), base_color, metallic);
    vec2 brdf = environment_brdf(roughness, n_dot_v);
    vec3 specular_weight = f0 * brdf.x + brdf.y;

    vec3 diffuse = global_illumination
                       ? texelFetch(diffuse_gi_texture, pixel, 0).rgb * base_color * (1.0 - metallic)
                       : vec3(0.0);
    vec3 rough_specular = global_illumination
                              ? texelFetch(specular_gi_texture, pixel, 0).rgb * specular_weight
                              : analytic_specular(normal, base_color, metallic, roughness, view_direction);
    vec3 specular = rough_specular;
    if (reflections) {
        // Traced reflections take over below the threshold, fading in over its last fifth.
        float alpha = roughness * roughness;
        float traced = 1.0 - smoothstep(0.8 * composite.extent.w, composite.extent.w, alpha);
        vec3 reflected = texelFetch(reflection_texture, pixel, 0).rgb * specular_weight;
        specular = mix(rough_specular, reflected, traced);
    } else if (!global_illumination) {
        specular = vec3(0.0);
    }
    output_color = vec4(max((diffuse + specular) * occlusion, vec3(0.0)), 0.0);
}
