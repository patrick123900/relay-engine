#version 450

// Particle sprites: six vertices per particle, expanded here from one storage-buffer entry into
// a quad facing the camera, stretched along the particle's travel, lying flat, standing upright or
// facing along the emitter's axis. See ParticleSprite and ParticleBatch in particle_system.hpp.

layout(set = 2, binding = 0) uniform ParticleFrame {
    mat4 view_projection;
    mat4 inverse_view_projection;
    vec4 camera_position;
    vec4 camera_right;
    vec4 camera_up;
    vec4 camera_forward;
    vec4 extent;
} frame;

struct Sprite {
    vec4 position_width;
    vec4 axis_height;
    vec4 color;
    vec4 rotation_frame_age;
};
layout(std430, set = 2, binding = 1) readonly buffer Sprites {
    Sprite sprites[];
};

// x/y/z of local_x and local_y: the emitter's axes for local alignment. local_x.w is the
// alignment (0 billboard, 1 stretched, 2 horizontal, 3 vertical, 4 local); sheet is columns,
// rows, whether frames blend and the soft distance; mode.x the blend mode, mode.y lit.
layout(push_constant) uniform Batch {
    vec4 local_x;
    vec4 local_y;
    vec4 sheet;
    vec4 mode;
} batch;

layout(location = 0) out vec2 frame_uv;
layout(location = 1) out vec2 next_frame_uv;
layout(location = 2) out float frame_blend;
layout(location = 3) out vec4 sprite_color;
layout(location = 4) out vec3 world_position;
layout(location = 5) out vec3 sprite_right;
layout(location = 6) out vec3 sprite_up;
layout(location = 7) out vec2 corner;

const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                               vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

vec2 cell_uv(float index, vec2 uv) {
    float columns = batch.sheet.x;
    float rows = batch.sheet.y;
    float column = mod(index, columns);
    float row = floor(index / columns);
    return (vec2(column, row) + uv) / vec2(columns, rows);
}

void main() {
    Sprite sprite = sprites[gl_InstanceIndex];
    vec3 center = sprite.position_width.xyz;
    vec3 to_camera = normalize(frame.camera_position.xyz - center);
    if (any(isnan(to_camera))) to_camera = -frame.camera_forward.xyz;
    uint alignment = uint(batch.local_x.w + 0.5);
    vec3 right = frame.camera_right.xyz;
    vec3 up = frame.camera_up.xyz;
    bool turns = true;
    if (alignment == 1u) {
        // Streaks lie along the velocity as it looks from the camera.
        vec3 along = sprite.axis_height.xyz - to_camera * dot(sprite.axis_height.xyz, to_camera);
        up = dot(along, along) > 1e-8 ? normalize(along) : frame.camera_up.xyz;
        right = normalize(cross(up, to_camera));
        turns = false;
    } else if (alignment == 2u) {
        right = vec3(1.0, 0.0, 0.0);
        up = vec3(0.0, 0.0, -1.0);
    } else if (alignment == 3u) {
        up = vec3(0.0, 1.0, 0.0);
        vec3 side = cross(up, to_camera);
        right = dot(side, side) > 1e-8 ? normalize(side) : frame.camera_right.xyz;
    } else if (alignment == 4u) {
        right = batch.local_x.xyz;
        up = batch.local_y.xyz;
    }
    if (turns) {
        float c = cos(sprite.rotation_frame_age.x), s = sin(sprite.rotation_frame_age.x);
        vec3 turned_right = c * right + s * up;
        up = -s * right + c * up;
        right = turned_right;
    }
    corner = corners[gl_VertexIndex];
    world_position = center + right * (corner.x * sprite.position_width.w * 0.5) +
                     up * (corner.y * sprite.axis_height.w * 0.5);
    sprite_right = right;
    sprite_up = up;
    gl_Position = frame.view_projection * vec4(world_position, 1.0);

    vec2 uv = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    float frames = batch.sheet.x * batch.sheet.y;
    float current = floor(sprite.rotation_frame_age.y);
    frame_uv = cell_uv(current, uv);
    next_frame_uv = cell_uv(mod(current + 1.0, frames), uv);
    frame_blend = batch.sheet.z > 0.5 ? fract(sprite.rotation_frame_age.y) : 0.0;
    sprite_color = sprite.color;
}
