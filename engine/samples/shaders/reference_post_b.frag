#version 450

layout(set = 1, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(1280.0, 720.0);
    vec3 color = texture(sourceImage, uv).rgb;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    float mask = smoothstep(0.45, 1.25, luminance);
    vec3 bloom = max(color - vec3(0.35), vec3(0.0)) * (0.45 + mask * 0.9);
    outColor = vec4(bloom, 1.0);
}
