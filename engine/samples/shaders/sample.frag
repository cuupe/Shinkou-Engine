#version 450

layout(set = 0, binding = 0) uniform texture2D gTextures[1];
layout(set = 0, binding = 1) uniform sampler gSampler;
layout(set = 0, binding = 2) uniform SceneFrame {
    mat4 viewProjection;
    vec4 cameraPositionAndFlags;
} sceneFrame;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    float sceneTint = 0.9 + 0.1 * (0.5 + 0.5 * sin(sceneFrame.cameraPositionAndFlags.z));
    color = texture(sampler2D(gTextures[0], gSampler), uv) * sceneTint;
}
