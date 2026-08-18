#version 450

layout(set = 1, binding = 0) uniform sampler2D sourceImage;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(1280.0, 720.0);
    vec2 texel = vec2(0.0, 1.0 / 720.0);
    vec3 color = texture(sourceImage, uv).rgb * 0.227027;
    color += texture(sourceImage, uv + texel * 1.384615).rgb * 0.316216;
    color += texture(sourceImage, uv - texel * 1.384615).rgb * 0.316216;
    color += texture(sourceImage, uv + texel * 3.230769).rgb * 0.070270;
    color += texture(sourceImage, uv - texel * 3.230769).rgb * 0.070270;
    outColor = vec4(color, 1.0);
}
