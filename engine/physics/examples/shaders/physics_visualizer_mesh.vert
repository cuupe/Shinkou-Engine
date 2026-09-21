#version 450

layout(location = 0) in vec3 position;
layout(location = 0) out vec2 uv;
layout(location = 1) out vec3 worldPosition;

layout(set = 0, binding = 2) uniform SceneFrame {
    mat4 viewProjection;
    vec4 cameraPositionAndFlags;
} sceneFrame;

layout(set = 0, binding = 3) uniform ObjectFrame {
    mat4 model;
} objectFrame;

void main() {
    worldPosition = (objectFrame.model * vec4(position, 1.0)).xyz;
    gl_Position = sceneFrame.viewProjection * vec4(worldPosition, 1.0);
    uv = position.xy * 0.5 + 0.5;
}
