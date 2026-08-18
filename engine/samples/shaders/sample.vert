#version 450

layout(location = 0) out vec2 uv;
layout(set = 0, binding = 2) uniform SceneFrame {
    mat4 viewProjection;
    vec4 cameraPositionAndFlags;
} sceneFrame;
layout(set = 0, binding = 3) uniform ObjectFrame {
    mat4 model;
} objectFrame;

void main() {
    const vec2 positions[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0)
    );
    const vec4 localPosition = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    const vec4 worldPosition = objectFrame.model * localPosition;
    gl_Position = sceneFrame.cameraPositionAndFlags.w > 1.5
        ? localPosition
        : sceneFrame.viewProjection * worldPosition;
    uv = positions[gl_VertexIndex] * 0.5 + 0.5;
}
