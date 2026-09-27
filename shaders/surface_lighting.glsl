// Material evaluation and direct lighting shared by the raster pass (first_light.frag) and the
// ray traced reflection hits (reflection_trace.comp), so reflected surfaces light identically.
// The includer defines SURFACE_TEXTURE(index, uv) to sample the bindless texture table: fragment
// shaders use implicit derivatives, compute shaders an explicit level of detail.

#include "scene_bindings.glsl"

float filtered_shadow(uint map_index, vec3 coordinates) {
    vec2 texel = 1.0 / vec2(textureSize(shadow_maps[0], 0));
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sample_coordinates = vec3(coordinates.xy + vec2(x, y) * texel, coordinates.z);
            if (map_index == 0u) visibility += texture(shadow_maps[0], sample_coordinates);
            else if (map_index == 1u) visibility += texture(shadow_maps[1], sample_coordinates);
            else if (map_index == 2u) visibility += texture(shadow_maps[2], sample_coordinates);
            else visibility += texture(shadow_maps[3], sample_coordinates);
        }
    }
    return visibility / 9.0;
}

float projected_visibility(vec3 world_position, mat4 view_projection, uint map_index, float bias) {
    vec4 position = view_projection * vec4(world_position, 1.0);
    vec3 projected = position.xyz / position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z < 0.0 || projected.z > 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return filtered_shadow(map_index, vec3(uv, projected.z - bias));
}

float cascade_visibility(vec3 world_position, uint cascade, float n_dot_l) {
    // Bias grows with cascade footprint and grazing angle. Raster depth bias handles the caster;
    // this receiver bias suppresses residual acne without detaching nearby contact shadows.
    float scale = exp2(float(cascade));
    float bias = max(0.00018 * scale * (1.0 - n_dot_l), 0.00004 * scale);
    return projected_visibility(world_position, lighting.shadow_view_projections[cascade], cascade,
                                bias);
}

float point_visibility(vec3 world_position, float n_dot_l) {
    vec3 delta = world_position - lighting.point_shadow_position_far.xyz;
    float major = max(max(abs(delta.x), abs(delta.y)), abs(delta.z));
    float near_plane = uintBitsToFloat(lighting.point_shadow_parameters.z);
    float far_plane = lighting.point_shadow_position_far.w;
    if (major <= near_plane || major >= far_plane) return 1.0;
    float reference = far_plane / (far_plane - near_plane) -
                      near_plane * far_plane / ((far_plane - near_plane) * major);
    reference -= max(0.001 * (1.0 - n_dot_l), 0.0002);
    vec3 direction = normalize(delta);
    vec3 helper = abs(direction.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, direction));
    vec3 bitangent = cross(direction, tangent);
    float visibility = 0.0;
    const float radius = 2.0 / 1024.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            visibility += texture(point_shadow_map,
                                  vec4(direction + (tangent * x + bitangent * y) * radius,
                                       reference));
    return visibility / 9.0;
}

struct Surface {
    vec4 base_color;
    float metallic;
    float roughness;
    vec3 normal;
    float occlusion;
    vec3 emissive;
};

// Masked materials drop fragments below their alpha cutoff.
bool surface_cut_out(uint material_index, float alpha) {
    MaterialData material = materials[material_index];
    uint alpha_mode = (material.texture_indices.w >> 16u) & 0x3u;
    return alpha_mode == 1u && alpha < material.surface_parameters.w;
}

Surface evaluate_surface(uint material_index, vec2 texture_coordinates, vec3 surface_normal,
                         vec4 surface_tangent, bool front_facing) {
    MaterialData material = materials[material_index];
    Surface surface;
    vec4 base_color = material.base_color_factor;
    if (material.texture_indices.x != missing_texture) {
        base_color *= SURFACE_TEXTURE(material.texture_indices.x, texture_coordinates);
    }
    float metallic = material.emissive_metallic.w;
    float roughness = material.surface_parameters.x;
    if (material.texture_indices.y != missing_texture) {
        vec4 sample_value = SURFACE_TEXTURE(material.texture_indices.y, texture_coordinates);
        roughness *= sample_value.g;
        metallic *= sample_value.b;
    }
    surface.base_color = base_color;
    surface.roughness = clamp(roughness, 0.045, 1.0);
    surface.metallic = clamp(metallic, 0.0, 1.0);

    vec3 normal = normalize(surface_normal);
    if (material.texture_indices.z != missing_texture) {
        vec3 tangent = normalize(surface_tangent.xyz);
        vec3 bitangent = normalize(cross(normal, tangent)) * surface_tangent.w;
        vec3 mapped = SURFACE_TEXTURE(material.texture_indices.z, texture_coordinates).xyz * 2.0 - 1.0;
        mapped.xy *= material.surface_parameters.y;
        normal = normalize(mat3(tangent, bitangent, normal) * mapped);
    }
    bool double_sided = ((material.texture_indices.w >> 18u) & 0x1u) != 0u;
    if (double_sided && !front_facing) normal = -normal;
    surface.normal = normal;

    float occlusion = 1.0;
    uint occlusion_texture = material.texture_indices.w & 0xffu;
    if (occlusion_texture != 0xffu) {
        float sampled = SURFACE_TEXTURE(occlusion_texture, texture_coordinates).r;
        occlusion = mix(1.0, sampled, material.surface_parameters.z);
    }
    surface.occlusion = occlusion;
    vec3 emissive = material.emissive_metallic.rgb;
    uint emissive_texture = (material.texture_indices.w >> 8u) & 0xffu;
    if (emissive_texture != 0xffu) {
        emissive *= SURFACE_TEXTURE(emissive_texture, texture_coordinates).rgb;
    }
    surface.emissive = emissive;
    return surface;
}

// Direct light from every scene light, with shadows: `total` holds diffuse plus specular, and
// `diffuse` the view-independent part.
void direct_lighting(Surface surface, vec3 world_position, vec3 view_direction, out vec3 total,
                     out vec3 diffuse_total) {
    vec3 normal = surface.normal;
    vec3 base_color = surface.base_color.rgb;
    float metallic = surface.metallic;
    float roughness = surface.roughness;
    vec3 direct_color=vec3(0.0);
    vec3 direct_diffuse=vec3(0.0);
    uint count=uint(lighting.camera_count.w);
    for (uint light_index=0;light_index<max(count,1u);++light_index) {
    vec3 light_direction = normalize(vec3(0.45, 0.65, 0.75));
    vec3 radiance=vec3(2.4);
    if (count>0u) {
        LightData light=lighting.lights[light_index];
        radiance=light.color_intensity.rgb*light.color_intensity.w;
        if (light.position_type.w==0.0) light_direction=-light.direction_inner.xyz;
        else {
            vec3 delta=light.position_type.xyz-world_position;
            float distance=length(delta);
            light_direction=delta/max(distance,0.0001);
            radiance/=max(dot(light.attenuation_outer.xyz,vec3(1.0,distance,distance*distance)),0.0001);
            if (light.range.x>0.0) radiance*=pow(clamp(1.0-pow(distance/light.range.x,4.0),0.0,1.0),2.0);
            if (light.position_type.w==2.0) {
                float cosine=dot(-light_direction,light.direction_inner.xyz);
                float inner=light.direction_inner.w,outer=light.attenuation_outer.w;
                radiance*=inner-outer>0.00001 ? clamp((cosine-outer)/(inner-outer),0.0,1.0) : step(outer,cosine);
            }
        }
    }
    vec3 half_direction = normalize(view_direction + light_direction);
    float n_dot_l = max(dot(normal, light_direction), 0.0);
    float n_dot_v = max(dot(normal, view_direction), 0.001);
    float n_dot_h = max(dot(normal, half_direction), 0.0);
    float v_dot_h = max(dot(view_direction, half_direction), 0.0);
    vec3 f0 = mix(vec3(0.04), base_color, metallic);
    vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(pi * denominator * denominator, 0.0001);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float geometry_v = n_dot_v / (n_dot_v * (1.0 - k) + k);
    float geometry_l = n_dot_l / (n_dot_l * (1.0 - k) + k);
    vec3 specular = distribution * geometry_v * geometry_l * fresnel /
                    max(4.0 * n_dot_v * n_dot_l, 0.0001);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base_color / pi;
    float visibility = 1.0;
    if (lighting.shadow_parameters.x != 0u && light_index == lighting.shadow_parameters.y) {
        float view_depth = max(dot(world_position - lighting.camera_count.xyz,
                                   lighting.camera_forward.xyz), 0.0);
        uint cascade = view_depth <= lighting.shadow_splits.x ? 0u :
                       view_depth <= lighting.shadow_splits.y ? 1u : 2u;
        if (view_depth <= lighting.shadow_splits.z) {
            visibility = cascade_visibility(world_position, cascade, n_dot_l);
            if (cascade < 2u) {
                float lower = cascade == 0u ? 0.0 : lighting.shadow_splits[cascade - 1u];
                float width = max((lighting.shadow_splits[cascade] - lower) * 0.1, 0.001);
                float blend = smoothstep(lighting.shadow_splits[cascade] - width,
                                         lighting.shadow_splits[cascade], view_depth);
                visibility = mix(visibility, cascade_visibility(world_position, cascade + 1u, n_dot_l),
                                 blend);
            } else {
                float fade_width = max((lighting.shadow_splits.z - lighting.shadow_splits.y) *
                                           0.1, 0.001);
                float fade = smoothstep(lighting.shadow_splits.z - fade_width,
                                        lighting.shadow_splits.z, view_depth);
                visibility = mix(visibility, 1.0, fade);
            }
        }
    } else if (lighting.shadow_parameters.z != 0u &&
               light_index == lighting.shadow_parameters.w) {
        float bias = max(0.0003 * (1.0 - n_dot_l), 0.00008);
        visibility = projected_visibility(world_position, lighting.spot_shadow_view_projection, 3u,
                                          bias);
    } else if (lighting.point_shadow_parameters.x != 0u &&
               light_index == lighting.point_shadow_parameters.y) {
        visibility = point_visibility(world_position, n_dot_l);
    }
    direct_color+=(diffuse+specular)*n_dot_l*radiance*visibility;
    direct_diffuse+=diffuse*n_dot_l*radiance*visibility;
    }
    total = direct_color;
    diffuse_total = direct_diffuse;
}

// Analytic hemispherical environment from the light reaching up- and downward surfaces: the
// scene's sky, or Relay's default without one. Rough surfaces see a broad reflection while
// polished surfaces retain directional variation.
#define sky_radiance (lighting.ambient_up.rgb)
#define ground_radiance (lighting.ambient_down.rgb)

// sky_gradient_position in sky.cpp: 0 at the horizon and below, 1 straight up.
float sky_gradient_position(float direction_y) {
    float below = 1.0 - clamp(direction_y, 0.0, 1.0);
    return 1.0 - below * below;
}

// The visible sky along a normalized direction without the sun (sky_background in
// scene_render.cpp).
vec3 sky_background(vec3 direction) {
    uint mode = uint(lighting.sky_zenith.w);
    if (mode == 2u) {
        // sky_panorama_uv in sky.cpp. Level 0 avoids the seam a derivative-based level would
        // show where u wraps.
        float u = 0.5 + atan(direction.x, -direction.z) / (2.0 * pi) - lighting.sky_horizon.w;
        float v = 0.5 - asin(clamp(direction.y, -1.0, 1.0)) / pi;
        return textureLod(sky_panorama, vec2(fract(u), v), 0.0).rgb * lighting.sky_tint.rgb;
    }
    if (mode == 1u)
        return mix(lighting.sky_horizon.rgb, lighting.sky_zenith.rgb,
                   sky_gradient_position(direction.y));
    return vec3(0.0);
}

// sun_glow in scene_render.cpp, with the same constants (sun_disc_radius_degrees 0.8 and
// sun_disc_brightness 40).
vec3 sun_glow(vec3 direction) {
    if (lighting.sun_direction.w == 0.0 || uint(lighting.sky_zenith.w) == 0u) return vec3(0.0);
    float angle = acos(clamp(dot(direction, lighting.sun_direction.xyz), -1.0, 1.0));
    float radius = radians(0.8);
    float edge = radius * 0.15;
    float disc = 1.0 - clamp((angle - radius + edge) / (2.0 * edge), 0.0, 1.0);
    float glow = 0.25 * exp(-angle / 0.02) + 0.03 * exp(-angle / 0.15);
    float horizon = smoothstep(0.0, 1.0, clamp((direction.y + 0.03) / 0.04, 0.0, 1.0));
    return lighting.sun_radiance.rgb * (disc * 40.0 + glow) * horizon;
}

// What the sky pass draws: the sky with the sun in it.
vec3 visible_sky(vec3 direction) {
    return sky_background(direction) + sun_glow(direction);
}

// The light arriving from a direction in ambient scale (sky_environment in scene_render.cpp).
// The sun is left out: its directional light already lights the scene.
vec3 sky_environment(vec3 direction) {
    if (uint(lighting.sky_zenith.w) == 0u)
        return mix(ground_radiance, sky_radiance, clamp(direction.y * 0.5 + 0.5, 0.0, 1.0));
    return sky_background(direction) * lighting.sky_tint.w;
}

// fog_amount in scene_render.cpp: how much fog covers a surface this far from the camera.
float fog_amount(float distance_from_camera) {
    if (lighting.fog_parameters.x == 0.0) return 0.0;
    float start = lighting.fog_start_color.w;
    float end = lighting.fog_end_color.w;
    return clamp((distance_from_camera - start) / max(end - start, 1e-3), 0.0, 1.0);
}

vec3 fog_color(float amount) {
    return mix(lighting.fog_start_color.rgb, lighting.fog_end_color.rgb, amount);
}

// The diffuse half of the analytic environment light, before occlusion.
vec3 ambient_diffuse(Surface surface) {
    vec3 environment_diffuse = mix(ground_radiance, sky_radiance,
                                   clamp(surface.normal.y * 0.5 + 0.5, 0.0, 1.0));
    return surface.base_color.rgb * (1.0 - surface.metallic) * environment_diffuse / pi;
}

// The specular half, before occlusion: what ray traced reflections replace on smooth surfaces.
vec3 ambient_specular(vec3 normal, vec3 base_color, float metallic, float roughness,
                      vec3 view_direction) {
    vec3 reflection = reflect(-view_direction, normal);
    vec3 environment_specular = mix(ground_radiance, sky_radiance,
                                    mix(clamp(reflection.y * 0.5 + 0.5, 0.0, 1.0), 0.5, roughness));
    vec3 environment_fresnel = mix(vec3(0.04), base_color, metallic) +
                               (1.0 - mix(vec3(0.04), base_color, metallic)) *
                               pow(1.0 - max(dot(normal, view_direction), 0.0), 5.0);
    return environment_fresnel * environment_specular * (1.0 - roughness * 0.5);
}

vec3 ambient_lighting(Surface surface, vec3 view_direction) {
    return (ambient_diffuse(surface) +
            ambient_specular(surface.normal, surface.base_color.rgb, surface.metallic,
                             surface.roughness, view_direction)) *
           surface.occlusion;
}
