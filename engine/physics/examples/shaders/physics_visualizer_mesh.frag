#version 450

layout(set = 0, binding = 0) uniform texture2D gTextures[1];
layout(set = 0, binding = 1) uniform sampler gSampler;
layout(set = 0, binding = 2) uniform SceneFrame {
    mat4 viewProjection;
    vec4 cameraPositionAndFlags;
} sceneFrame;

layout(location = 0) in vec2 uv;
layout(location = 1) in vec3 worldPosition;
layout(location = 0) out vec4 color;

void main() {
    vec3 dx = dFdx(worldPosition);
    vec3 dy = dFdy(worldPosition);
    vec3 normal = normalize(cross(dx, dy));
    vec3 lightDirection = normalize(vec3(0.35, 0.65, 0.75));
    float diffuse = 0.22 + 0.78 * clamp(dot(normal, lightDirection), 0.0, 1.0);
    vec4 albedo = texture(sampler2D(gTextures[0], gSampler), uv);
    color = vec4(albedo.rgb * diffuse, albedo.a);
}
