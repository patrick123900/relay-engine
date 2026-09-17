#version 450
layout(push_constant) uniform Selection { mat4 mvp; mat4 parameters; } selection;
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=0) out vec2 texture_coordinates;
layout(location=1) flat out uint material_index;
void main() {
    const vec2 offsets[8] = vec2[](vec2(1,0), vec2(-1,0), vec2(0,1), vec2(0,-1),
        vec2(0.7071,0.7071), vec2(-0.7071,0.7071), vec2(0.7071,-0.7071), vec2(-0.7071,-0.7071));
    gl_Position = selection.mvp * vec4(position,1.0);
    // Eight translated copies dilate the silhouette in screen pixels, including flat meshes.
    gl_Position.xy += offsets[gl_InstanceIndex % 8] * selection.parameters[0].xy * gl_Position.w;
    texture_coordinates = uv;
    material_index = gl_InstanceIndex / 8;
}
