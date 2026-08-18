#version 450

layout(location = 0) in vec3 position;
layout(set = 0, binding = 2) uniform SceneFrame {
    mat4 viewProjection;
    vec4 cameraPositionAndFlags;
} sceneFrame;
layout(set = 0, binding = 4, std430) readonly buffer InstanceModels {
    mat4 models[];
} instanceModels;

void main() {
    gl_Position = sceneFrame.viewProjection * instanceModels.models[gl_InstanceIndex] * vec4(position, 1.0);
}
