// Material evaluation and direct lighting shared by the raster pass (first_light.frag) and the
// ray traced reflection hits (reflection_trace.comp), so reflected surfaces light identically.
// The includer defines SURFACE_TEXTURE(index, uv) to sample the bindless texture table: fragment
// shaders use implicit derivatives, compute shaders an explicit level of detail.

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
struct LightData { vec4 position_type; vec4 direction_inner; vec4 color_intensity; vec4 attenuation_outer; vec4 range; };
layout(std430,set=0,binding=2) readonly buffer LightingBuffer {
    vec4 camera_count;
    LightData lights[16];
    mat4 shadow_view_projections[3];
    mat4 spot_shadow_view_projection;
    mat4 point_shadow_view_projections[6];
    vec4 shadow_splits;
    // w holds indirect light flags: 1, global illumination replaces the analytic sky; 2, ray
    // traced reflections replace its specular half. See first_light.frag.
    vec4 camera_forward;
    vec4 point_shadow_position_far;
    uvec4 shadow_parameters;
    uvec4 point_shadow_parameters;
} lighting;
layout(set=0,binding=3) uniform sampler2DShadow shadow_maps[4];
layout(set=0,binding=4) uniform samplerCubeShadow point_shadow_map;

const uint missing_texture = 0xffffffffu;
const float pi = 3.14159265359;

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

// Analytic hemispherical environment. The sky and ground are linear radiances; rough surfaces see
// a broad reflection while polished surfaces retain directional variation.
const vec3 sky_radiance = vec3(0.20, 0.31, 0.48);
const vec3 ground_radiance = vec3(0.055, 0.047, 0.039);

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
