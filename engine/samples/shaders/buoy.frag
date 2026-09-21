#version 450

layout(set = 0, binding = 2, std140) uniform BuoyFrame {
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 cameraPosition;
} frame;

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 color;

void main() {
    float y = worldPosition.y;
    vec3 albedo;
    if (y > 2.05) albedo = vec3(0.86, 0.015, 0.008);
    else if (y > 1.62) albedo = vec3(1.0, 0.75, 0.03);
    else if (y > 1.38) albedo = vec3(0.015, 0.02, 0.018);
    else if (y > 0.35) albedo = vec3(0.82, 0.84, 0.80);
    else albedo = vec3(0.02, 0.025, 0.022);

    vec3 normal = normalize(worldNormal);
    vec3 lightDirection = normalize(frame.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(frame.cameraPosition.xyz - worldPosition);
    vec3 halfVector = normalize(lightDirection + viewDirection);
    float diffuse = 0.25 + 0.75 * max(dot(normal, lightDirection), 0.0);
    float specular = pow(max(dot(normal, halfVector), 0.0), 64.0) *
        frame.sunDirectionIntensity.w;
    float rim = pow(1.0 - max(dot(normal, viewDirection), 0.0), 3.0) * 0.16;
    vec3 result = albedo * diffuse + vec3(1.0, 0.86, 0.65) * specular + rim;
    result = result / (vec3(1.0) + result);
    color = vec4(pow(max(result, vec3(0.0)), vec3(1.0 / 2.2)), 1.0);
}
