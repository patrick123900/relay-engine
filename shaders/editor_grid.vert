#version 450
layout(push_constant) uniform Grid { mat4 view_projection; mat4 camera; } grid;
layout(location=0) out vec3 world;
layout(location=1) flat out float level_opacity;
void main() {
    const vec2 corners[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1),
                                  vec2(-1,-1), vec2(1,1), vec2(-1,1));
    vec2 center = floor(grid.camera[0].xz);
    const float level_gap = 10.0;
    // Preserve the original shader crossfade; mirror only the plane heights below zero.
    float height = abs(grid.camera[0].y) / level_gap;
    float level = floor(height) * level_gap;
    float blend = smoothstep(0.5, 1.0, fract(height));
    float direction = grid.camera[0].y < 0.0 ? -1.0 : 1.0;
    level_opacity = gl_InstanceIndex == 0 ? 1.0 - blend : blend;
    world = vec3(center.x + corners[gl_VertexIndex].x * 120.0,
                 direction * (level + gl_InstanceIndex * level_gap),
                 center.y + corners[gl_VertexIndex].y * 120.0);
    gl_Position = grid.view_projection * vec4(world,1.0);
}
