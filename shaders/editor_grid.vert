#version 450
layout(push_constant) uniform Grid { mat4 view_projection; mat4 camera; } grid;
layout(location=0) out vec3 world;
layout(location=1) flat out float level_opacity;
void main() {
    const vec2 corners[6] = vec2[](vec2(-1,-1), vec2(1,-1), vec2(1,1),
                                  vec2(-1,-1), vec2(1,1), vec2(-1,1));
    vec2 center = floor(grid.camera[0].xz);
    const float level_gap = 10.0;
    // Ground stays at Y=0 below height 5. Each level holds for the first half
    // of its interval, then crossfades to the next plane by the upper boundary.
    float height = max(grid.camera[0].y, 0.0) / level_gap;
    float level = floor(height) * level_gap;
    float blend = smoothstep(0.5, 1.0, fract(height));
    level_opacity = gl_InstanceIndex == 0 ? 1.0 - blend : blend;
    world = vec3(center.x + corners[gl_VertexIndex].x * 120.0, level + gl_InstanceIndex * level_gap,
                 center.y + corners[gl_VertexIndex].y * 120.0);
    gl_Position = grid.view_projection * vec4(world,1.0);
}
