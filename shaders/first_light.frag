#version 450

layout(location = 0) in vec2 texture_coordinates;
layout(location = 1) in vec3 surface_normal;
layout(location = 2) in vec4 surface_tangent;
layout(location = 3) flat in uint material_index;
layout(location = 4) in vec3 world_position;
layout(location = 0) out vec4 output_color;

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
    vec4 camera_forward;
    vec4 point_shadow_position_far;
    uvec4 shadow_parameters;
    uvec4 point_shadow_parameters;
} lighting;
layout(set=0,binding=3) uniform sampler2DShadow shadow_maps[4];
layout(set=0,binding=4) uniform samplerCubeShadow point_shadow_map;

layout(push_constant) uniform FrameData {
    mat4 model_view_projection;
    mat4 model;
} frame;

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

float projected_visibility(mat4 view_projection, uint map_index, float bias) {
    vec4 position = view_projection * vec4(world_position, 1.0);
    vec3 projected = position.xyz / position.w;
    vec2 uv = projected.xy * 0.5 + 0.5;
    if (projected.z < 0.0 || projected.z > 1.0 ||
        any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    return filtered_shadow(map_index, vec3(uv, projected.z - bias));
}

float cascade_visibility(uint cascade, float n_dot_l) {
    // Bias grows with cascade footprint and grazing angle. Raster depth bias handles the caster;
    // this receiver bias suppresses residual acne without detaching nearby contact shadows.
    float scale = exp2(float(cascade));
    float bias = max(0.00018 * scale * (1.0 - n_dot_l), 0.00004 * scale);
    return projected_visibility(lighting.shadow_view_projections[cascade], cascade, bias);
}

float point_visibility(float n_dot_l) {
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

void main() {
    MaterialData material = materials[material_index];
    vec4 base_color = material.base_color_factor;
    if (material.texture_indices.x != missing_texture) {
        base_color *= texture(textures[material.texture_indices.x], texture_coordinates);
    }
    uint alpha_mode = (material.texture_indices.w >> 16u) & 0x3u;
    if (alpha_mode == 1u && base_color.a < material.surface_parameters.w) discard;

    float metallic = material.emissive_metallic.w;
    float roughness = material.surface_parameters.x;
    if (material.texture_indices.y != missing_texture) {
        vec4 sample_value = texture(textures[material.texture_indices.y], texture_coordinates);
        roughness *= sample_value.g;
        metallic *= sample_value.b;
    }
    roughness = clamp(roughness, 0.045, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    vec3 normal = normalize(surface_normal);
    if (material.texture_indices.z != missing_texture) {
        vec3 tangent = normalize(surface_tangent.xyz);
        vec3 bitangent = normalize(cross(normal, tangent)) * surface_tangent.w;
        vec3 mapped = texture(textures[material.texture_indices.z], texture_coordinates).xyz * 2.0 - 1.0;
        mapped.xy *= material.surface_parameters.y;
        normal = normalize(mat3(tangent, bitangent, normal) * mapped);
    }
    bool double_sided = ((material.texture_indices.w >> 18u) & 0x1u) != 0u;
    if (double_sided && !gl_FrontFacing) normal = -normal;

    vec3 view_direction = normalize(lighting.camera_count.xyz-world_position);
    vec3 direct_color=vec3(0.0);
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
    vec3 f0 = mix(vec3(0.04), base_color.rgb, metallic);
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
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * base_color.rgb / pi;
    float visibility = 1.0;
    if (lighting.shadow_parameters.x != 0u && light_index == lighting.shadow_parameters.y) {
        float view_depth = max(dot(world_position - lighting.camera_count.xyz,
                                   lighting.camera_forward.xyz), 0.0);
        uint cascade = view_depth <= lighting.shadow_splits.x ? 0u :
                       view_depth <= lighting.shadow_splits.y ? 1u : 2u;
        if (view_depth <= lighting.shadow_splits.z) {
            visibility = cascade_visibility(cascade, n_dot_l);
            if (cascade < 2u) {
                float lower = cascade == 0u ? 0.0 : lighting.shadow_splits[cascade - 1u];
                float width = max((lighting.shadow_splits[cascade] - lower) * 0.1, 0.001);
                float blend = smoothstep(lighting.shadow_splits[cascade] - width,
                                         lighting.shadow_splits[cascade], view_depth);
                visibility = mix(visibility, cascade_visibility(cascade + 1u, n_dot_l), blend);
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
        visibility = projected_visibility(lighting.spot_shadow_view_projection, 3u, bias);
    } else if (lighting.point_shadow_parameters.x != 0u &&
               light_index == lighting.point_shadow_parameters.y) {
        visibility = point_visibility(n_dot_l);
    }
    direct_color+=(diffuse+specular)*n_dot_l*radiance*visibility;
    }

    float occlusion = 1.0;
    uint occlusion_texture = material.texture_indices.w & 0xffu;
    if (occlusion_texture != 0xffu) {
        float sampled = texture(textures[occlusion_texture], texture_coordinates).r;
        occlusion = mix(1.0, sampled, material.surface_parameters.z);
    }
    vec3 emissive = material.emissive_metallic.rgb;
    uint emissive_texture = (material.texture_indices.w >> 8u) & 0xffu;
    if (emissive_texture != 0xffu) {
        emissive *= texture(textures[emissive_texture], texture_coordinates).rgb;
    }
    vec3 ambient = base_color.rgb * 0.08 * occlusion;
    vec3 color = ambient + direct_color + emissive;
    color = color / (color + vec3(1.0));
    output_color = vec4(color, base_color.a);
}
