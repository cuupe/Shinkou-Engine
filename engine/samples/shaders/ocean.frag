#version 450

layout(set = 0, binding = 0) uniform texture2D noiseTexture;
layout(set = 0, binding = 1) uniform sampler noiseSampler;

layout(set = 0, binding = 2, std140) uniform OceanFrame {
    vec4 cameraPosition;
    vec4 cameraForwardTan;
    vec4 cameraRightTime;
    vec4 cameraUpAspect;
    vec4 windParameters;
    vec4 lightingParameters;
    vec4 renderParams;
    vec4 waterParameters;
    vec4 resolutionMouse;
    mat4 reflectionViewProjection;
} frame;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec2 reference_noise(vec3 position) {
    vec3 p = floor(position);
    vec3 f = fract(position);
    f = f * f * (3.0 - 2.0 * f);
    vec2 noiseUv = p.xy + vec2(37.0, 17.0) * p.z;
    vec4 rg = textureLod(sampler2D(noiseTexture, noiseSampler), (noiseUv + f.xy + 0.5) / 256.0, 0.0);
    return mix(rg.yw, rg.xz, f.z);
}

vec2 reference_noise_precise(vec3 position) {
    vec3 p = floor(position);
    vec3 f = fract(position);
    f = f * f * (3.0 - 2.0 * f);
    vec2 noiseUv = p.xy + vec2(37.0, 17.0) * p.z;
    vec4 rg = mix(
        mix(
            textureLod(sampler2D(noiseTexture, noiseSampler), (noiseUv + 0.5) / 256.0, 0.0),
            textureLod(sampler2D(noiseTexture, noiseSampler), (noiseUv + vec2(1.0, 0.0) + 0.5) / 256.0, 0.0), f.x),
        mix(
            textureLod(sampler2D(noiseTexture, noiseSampler), (noiseUv + vec2(0.0, 1.0) + 0.5) / 256.0, 0.0),
            textureLod(sampler2D(noiseTexture, noiseSampler), (noiseUv + 1.5) / 256.0, 0.0), f.x), f.y);
    return mix(rg.yw, rg.xz, f.z);
}

vec4 reference_noise_2(vec2 position) {
    vec2 p = floor(position);
    vec2 f = fract(position);
    f = f * f * (3.0 - 2.0 * f);
    return textureLod(sampler2D(noiseTexture, noiseSampler), (p + f + 0.5) / 256.0, 0.0);
}

float reference_noise_integer(vec2 position) {
    return textureLod(sampler2D(noiseTexture, noiseSampler), (floor(position) + 0.5) / 256.0, 0.0).x;
}

float reference_waves(vec3 position) {
    position *= 0.2;
    const int octaves = 5;
    float value = 0.0;
    position += frame.cameraRightTime.w * vec3(0.0, 0.1, 0.1);
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * vec3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 2.0 + abs(reference_noise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    value /= exp2(float(octaves));
    return 0.5 - value;
}

float reference_waves_detail(vec3 position) {
    position *= 0.2;
    const int octaves = 8;
    float value = 0.0;
    position += frame.cameraRightTime.w * vec3(0.0, 0.1, 0.1);
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * vec3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 2.0 + abs(reference_noise_precise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    value /= exp2(float(octaves));
    return 0.5 - value;
}

float reference_waves_smooth(vec3 position) {
    position *= 0.2;
    const int octaves = 2;
    float value = 0.0;
    position += frame.cameraRightTime.w * vec3(0.0, 0.1, 0.1);
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * vec3(1.0, -1.0, 1.0)) / sqrt(2.0);
        float noise = reference_noise_precise(position).x - 0.5;
        value = value * 2.0 + sqrt(noise * noise + 0.01) * 2.0;
        position *= 2.0;
    }
    value /= exp2(float(octaves));
    return 0.5 - value;
}

float reference_wave_crests(vec3 inputPosition, vec2 fragCoord) {
    vec3 position = inputPosition * 0.2;
    const int octaves1 = 6;
    const int octaves2 = 16;
    float value = 0.0;
    position += frame.cameraRightTime.w * vec3(0.0, 0.1, 0.1);
    vec3 largePosition = position;
    for (int index = 0; index < octaves1; ++index) {
        position = (position.yzx + position.zyx * vec3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 1.5 + abs(reference_noise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    position = largePosition * exp2(float(octaves1));
    position.y = -0.05 * frame.cameraRightTime.w;
    for (int index = octaves1; index < octaves2; ++index) {
        position = (position.yzx + position.zyx * vec3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 1.5 + pow(abs(reference_noise(position).x - 0.5) * 2.0, 1.0);
        position *= 2.0;
    }
    value /= 1500.0;
    value -= reference_noise_integer(fragCoord) * 0.01;
    return pow(smoothstep(0.4, -0.1, value), 6.0);
}

float reference_ocean_distance(vec3 position) { return position.y - reference_waves(position); }
float reference_ocean_distance_detail(vec3 position) { return position.y - reference_waves_detail(position); }

vec3 reference_ocean_normal(vec3 position) {
    float delta = 0.01 * length(position);
    vec3 normal;
    normal.x = reference_ocean_distance_detail(position + vec3(delta, 0.0, 0.0)) - reference_ocean_distance_detail(position - vec3(delta, 0.0, 0.0));
    normal.y = reference_ocean_distance_detail(position + vec3(0.0, delta, 0.0)) - reference_ocean_distance_detail(position - vec3(0.0, delta, 0.0));
    normal.z = reference_ocean_distance_detail(position + vec3(0.0, 0.0, delta)) - reference_ocean_distance_detail(position - vec3(0.0, 0.0, delta));
    return normalize(normal);
}

float reference_trace_ocean(vec3 position, vec3 ray) {
    float distanceField = 1.0;
    float distance = 0.0;
    for (int index = 0; index < 100; ++index) {
        if (distanceField < 0.01 || distance > 100.0) break;
        distanceField = reference_ocean_distance(position + distance * ray);
        distance += distanceField;
    }
    return distanceField > 0.1 ? 0.0 : distance;
}

struct BoatTransform { vec3 right; vec3 up; vec3 forward; vec3 position; };

BoatTransform reference_compute_boat_transform() {
    vec3 samples[5];
    samples[0] = vec3(0.0, 0.0, 0.0);
    samples[1] = vec3(0.0, 0.0, 0.5);
    samples[2] = vec3(0.0, 0.0, -0.5);
    samples[3] = vec3(0.5, 0.0, 0.0);
    samples[4] = vec3(-0.5, 0.0, 0.0);
    for (int index = 0; index < 5; ++index) samples[index].y = reference_waves_smooth(samples[index]);
    BoatTransform result;
    result.position = (samples[0] + samples[1] + samples[2] + samples[3] + samples[4]) / 5.0;
    result.right = samples[3] - samples[4];
    result.forward = samples[1] - samples[2];
    result.up = normalize(cross(result.forward, result.right));
    result.right = normalize(cross(result.up, result.forward));
    result.forward = normalize(result.forward);
    return result;
}

vec3 reference_world_to_boat(vec3 direction, BoatTransform boat) {
    return vec3(dot(direction, boat.right), dot(direction, boat.up), dot(direction, boat.forward));
}

float reference_trace_boat(vec3 position, vec3 ray, vec3 boatPosition) {
    vec3 center = boatPosition - position;
    float distance = dot(center, ray);
    float perpendicular = length(center - distance * ray);
    if (perpendicular > 1.0) return 0.0;
    return distance - sqrt(1.0 - perpendicular * perpendicular);
}

vec3 reference_sky(vec3 ray) {
    vec3 result = vec3(0.4, 0.45, 0.5);
    float sunCosine = clamp(dot(normalize(ray), normalize(frame.lightingParameters.xyz)), 0.0, 1.0);
    result += vec3(1.0, 0.82, 0.58) * pow(sunCosine, 256.0) * frame.lightingParameters.w * 0.35;
    return result;
}

vec3 reference_shade_boat(vec3 position, vec3 ray, BoatTransform boat) {
    position -= boat.position;
    vec3 normal = normalize(position);
    position = reference_world_to_boat(position, boat);
    vec3 lightDirection = normalize(frame.lightingParameters.xyz);
    float diffuse = dot(normal, lightDirection);
    vec3 light = smoothstep(-0.1, 1.0, diffuse) * vec3(1.0, 0.9, 0.8) + vec3(0.06, 0.1, 0.1);
    float antialias = 4.0 / max(frame.resolutionMouse.x, 1.0);
    vec3 albedo = vec3(1.0, 0.01, 0.0);
    albedo = mix(vec3(0.04), albedo, smoothstep(0.25 - antialias, 0.25, abs(position.y)));
    albedo = mix(mix(vec3(1.0), vec3(0.04), smoothstep(-antialias * 4.0, antialias * 4.0, cos(atan(position.x, position.z) * 6.0))),
        albedo, smoothstep(0.2 - antialias * 1.5, 0.2, abs(position.y)));
    albedo = mix(vec3(0.04), albedo, smoothstep(0.05 - antialias, 0.05, abs(abs(position.y) - 0.6)));
    albedo = mix(vec3(1.0, 0.8, 0.08), albedo, smoothstep(0.05 - antialias, 0.05, abs(abs(position.y) - 0.65)));
    vec3 result = albedo * light;
    vec3 halfVector = normalize(lightDirection - ray);
    float specular = pow(max(0.0, dot(normal, halfVector)), 100.0) * 100.0 / 32.0;
    vec3 specularColor = vec3(specular);
    vec3 reflectedRay = reflect(ray, normal);
    specularColor += mix(vec3(0.0, 0.04, 0.04), reference_sky(reflectedRay), smoothstep(-0.1, 0.1, reflectedRay.y));
    float fresnel = mix(0.001, 1.0, pow(1.0 - abs(dot(normal, ray)), 5.0));
    return mix(result, specularColor, fresnel);
}

vec3 reference_shade_ocean(vec3 position, vec3 ray, vec2 fragCoord, BoatTransform boat) {
    vec3 normal = reference_ocean_normal(position);
    float ndotr = dot(ray, normal);
    float fresnel = pow(1.0 - abs(ndotr), 5.0);
    vec3 reflectedRay = ray - 2.0 * normal * ndotr;
    vec3 refractedRay = normalize(ray + (-cos(1.33 * acos(-ndotr)) - ndotr) * normal);
    vec3 reflection = reference_sky(reflectedRay);
    float distance = reference_trace_boat(position, reflectedRay, boat.position);
    if (distance > 0.0) reflection = reference_shade_boat(position + distance * reflectedRay, reflectedRay, boat);
    distance = reference_trace_boat(position, refractedRay, boat.position);
    vec3 result = vec3(0.0, 0.04, 0.04);
    if (distance > 0.0) result = mix(result, reference_shade_boat(position + distance * refractedRay, refractedRay, boat), exp(-distance));
    result = mix(result, reflection, fresnel);
    result = mix(result, vec3(1.0), reference_wave_crests(position, fragCoord));
    return result;
}

void main() {
    vec2 fragCoord = uv * frame.resolutionMouse.xy;
    vec2 rayCoordinate = fragCoord - frame.resolutionMouse.xy * 0.5;
    vec3 ray = normalize(frame.cameraForwardTan.xyz +
        frame.cameraRightTime.xyz * rayCoordinate.x / max(frame.resolutionMouse.y, 1.0) +
        frame.cameraUpAspect.xyz * rayCoordinate.y / max(frame.resolutionMouse.y, 1.0));
    BoatTransform boat = reference_compute_boat_transform();
    float oceanDistance = reference_trace_ocean(frame.cameraPosition.xyz, ray);
    float boatDistance = reference_trace_boat(frame.cameraPosition.xyz, ray, boat.position);
    vec3 result;
    if (oceanDistance > 0.0 && (oceanDistance < boatDistance || boatDistance == 0.0))
        result = reference_shade_ocean(frame.cameraPosition.xyz + ray * oceanDistance, ray, fragCoord, boat);
    else if (boatDistance > 0.0)
        result = reference_shade_boat(frame.cameraPosition.xyz + ray * boatDistance, ray, boat);
    else
        result = reference_sky(ray);
    result *= 1.1 * smoothstep(0.35, 1.0, dot(ray, frame.cameraForwardTan.xyz));
    color = vec4(pow(max(result, vec3(0.0)), vec3(1.0 / 2.2)), 1.0);
}
