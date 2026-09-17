#version 450
layout(push_constant) uniform Grid { mat4 view_projection; mat4 camera; } grid;
layout(location=0) in vec3 world;
layout(location=1) flat in float level_opacity;
layout(location=0) out vec4 color;
float lines(vec2 p) {
    vec2 width = max(fwidth(p), vec2(0.0001));
    vec2 d = abs(fract(p - 0.5) - 0.5) / width;
    return 1.0 - clamp(min(d.x,d.y),0.0,1.0);
}
void main() {
    float distance_fade = 1.0 - smoothstep(35.0,90.0,length(world-grid.camera[0].xyz));
    // Fade subpixel minor lines as the plane approaches the horizon, avoiding shimmer.
    float minor_fade = 1.0 - smoothstep(0.5,2.0,max(fwidth(world.x),fwidth(world.z)));
    float opacity = max(lines(world.xz)*0.24*minor_fade,lines(world.xz/10.0)*0.4)*distance_fade*level_opacity;
    if (opacity < 0.002) discard;
    color = vec4(vec3(0.15),opacity);
}
