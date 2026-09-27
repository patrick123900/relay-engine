// The scene's set 0: the bindless texture table and material buffer, the lighting buffer, shadow
// maps and the sky panorama. Included by surface_lighting.glsl and by shaders Relay generates
// from project .relay-shader files, whose vertex stage needs only these declarations.

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
    // The scene's sky (RenderSky in scene_render.hpp). sky_zenith.w is 0 without a sky, 1 for the
    // gradient and 2 for a panorama; sky_horizon.w turns the panorama, in whole turns.
    vec4 sky_horizon;
    vec4 sky_zenith;
    // xyz: the panorama's tint. w: the scale from visible sky radiance to ambient light.
    vec4 sky_tint;
    // The light reaching surfaces facing straight up and down, in ambient (irradiance) scale.
    vec4 ambient_up;
    vec4 ambient_down;
    // Fog colors, with the start and end distances in w. fog_parameters.x is 1 when fog is on.
    vec4 fog_start_color;
    vec4 fog_end_color;
    vec4 fog_parameters;
    // The sun disc (RenderSky): the direction towards the sun, with w 1 when there is one, and its
    // light's color times intensity.
    vec4 sun_direction;
    vec4 sun_radiance;
    // x: seconds for animated shaders (TIME).
    vec4 frame_time;
} lighting;
layout(set=0,binding=3) uniform sampler2DShadow shadow_maps[4];
layout(set=0,binding=4) uniform samplerCubeShadow point_shadow_map;
// The sky material's equirectangular panorama; a 1x1 placeholder without one.
layout(set=0,binding=6) uniform sampler2D sky_panorama;

const uint missing_texture = 0xffffffffu;
const float pi = 3.14159265359;
