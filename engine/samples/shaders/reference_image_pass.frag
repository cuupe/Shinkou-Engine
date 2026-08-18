#version 450

layout(set = 1, binding = 0) uniform sampler2D colorImage;
layout(set = 1, binding = 1) uniform sampler2D bloomImage;
layout(location = 0) out vec4 outColor;

vec3 tonemap(vec3 color) {
    color = max(color, vec3(0.0));
    color = color / (color + vec3(1.0));
    return pow(color, vec3(1.0 / 2.2));
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(1280.0, 720.0);
    vec3 color = texture(colorImage, uv).rgb;
    vec3 bloom = texture(bloomImage, uv).rgb;
    outColor = vec4(tonemap(color + bloom * 0.22), 1.0);
}
