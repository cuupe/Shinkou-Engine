#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

layout(set = 0, binding = 2, std140) uniform BuoyFrame {
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 cameraPosition;
} frame;

layout(set = 0, binding = 3, std140) uniform BuoyObject {
    mat4 model;
} objectFrame;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;

void main() {
    vec4 world = objectFrame.model * vec4(position, 1.0);
    gl_Position = frame.viewProjection * world;
    worldPosition = world.xyz;
    worldNormal = normalize(mat3(objectFrame.model) * normal);
}
