#version 450
// Colors and textures are sRGB, multiplied together as picked; an sRGB swapchain wants linear
// output, which it encodes and blends in linear light.
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 color;
layout(location = 0) out vec4 result;
layout(set = 0, binding = 0) uniform sampler2D image;
layout(push_constant) uniform Placement {
    vec2 scale;
    vec2 translate;
    uint linear_output;
} placement;

vec3 srgb_to_linear(vec3 value) {
    return mix(value / 12.92, pow((value + 0.055) / 1.055, vec3(2.4)), greaterThan(value, vec3(0.04045)));
}

void main() {
    vec4 value = color * texture(image, uv);
    result = vec4(placement.linear_output != 0u ? srgb_to_linear(value.rgb) : value.rgb, value.a);
}
