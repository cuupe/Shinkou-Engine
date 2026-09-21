struct OceanVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Texture2D noiseTexture : register(t0);
SamplerState noiseSampler : register(s1);

cbuffer OceanFrame : register(b2) {
    float4 cameraPosition;
    float4 cameraForwardTan;
    float4 cameraRightTime;
    float4 cameraUpAspect;
    float4 windParameters;
    float4 lightingParameters;
    float4 renderParams;
    float4 waterParameters;
    float4 resolutionMouse;
    float4x4 reflectionViewProjection;
};

OceanVertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    OceanVertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = positions[vertexId] * 0.5 + 0.5;
    return output;
}

// This is the reference's noise-channel sampling scheme. The CPU supplies a
// repeat-wrapped 256x256 RGBA noise channel, matching iChannel0's contract.
float2 reference_noise(float3 position) {
    float3 p = floor(position);
    float3 f = frac(position);
    f = f * f * (3.0 - 2.0 * f);
    float2 uv = p.xy + float2(37.0, 17.0) * p.z;
    float4 rg = noiseTexture.SampleLevel(noiseSampler, (uv + f.xy + 0.5) / 256.0, 0.0);
    return lerp(rg.yw, rg.xz, f.z);
}

float2 reference_noise_precise(float3 position) {
    float3 p = floor(position);
    float3 f = frac(position);
    f = f * f * (3.0 - 2.0 * f);
    float2 uv = p.xy + float2(37.0, 17.0) * p.z;
    float4 rg = lerp(
        lerp(
            noiseTexture.SampleLevel(noiseSampler, (uv + 0.5) / 256.0, 0.0),
            noiseTexture.SampleLevel(noiseSampler, (uv + float2(1.0, 0.0) + 0.5) / 256.0, 0.0), f.x),
        lerp(
            noiseTexture.SampleLevel(noiseSampler, (uv + float2(0.0, 1.0) + 0.5) / 256.0, 0.0),
            noiseTexture.SampleLevel(noiseSampler, (uv + 1.5) / 256.0, 0.0), f.x),
        f.y);
    return lerp(rg.yw, rg.xz, f.z);
}

float4 reference_noise_2(float2 position) {
    float2 p = floor(position);
    float2 f = frac(position);
    f = f * f * (3.0 - 2.0 * f);
    return noiseTexture.SampleLevel(noiseSampler, (p + f + 0.5) / 256.0, 0.0);
}

float reference_noise_integer(float2 position) {
    return noiseTexture.SampleLevel(noiseSampler, (floor(position) + 0.5) / 256.0, 0.0).x;
}

float reference_waves(float3 position) {
    position *= 0.2;
    const int octaves = 5;
    float value = 0.0;
    position += cameraRightTime.w * float3(0.0, 0.1, 0.1);
    [loop]
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * float3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 2.0 + abs(reference_noise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    value /= exp2((float)octaves);
    return 0.5 - value;
}

float reference_waves_detail(float3 position) {
    position *= 0.2;
    const int octaves = 8;
    float value = 0.0;
    position += cameraRightTime.w * float3(0.0, 0.1, 0.1);
    [loop]
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * float3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 2.0 + abs(reference_noise_precise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    value /= exp2((float)octaves);
    return 0.5 - value;
}

float reference_waves_smooth(float3 position) {
    position *= 0.2;
    const int octaves = 2;
    float value = 0.0;
    position += cameraRightTime.w * float3(0.0, 0.1, 0.1);
    [loop]
    for (int index = 0; index < octaves; ++index) {
        position = (position.yzx + position.zyx * float3(1.0, -1.0, 1.0)) / sqrt(2.0);
        const float noise = reference_noise_precise(position).x - 0.5;
        value = value * 2.0 + sqrt(noise * noise + 0.01) * 2.0;
        position *= 2.0;
    }
    value /= exp2((float)octaves);
    return 0.5 - value;
}

float reference_wave_crests(float3 inputPosition, float2 fragCoord) {
    float3 position = inputPosition * 0.2;
    const int octaves1 = 6;
    const int octaves2 = 16;
    float value = 0.0;
    position += cameraRightTime.w * float3(0.0, 0.1, 0.1);
    const float3 largePosition = position;
    [loop]
    for (int index = 0; index < octaves1; ++index) {
        position = (position.yzx + position.zyx * float3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 1.5 + abs(reference_noise(position).x - 0.5) * 2.0;
        position *= 2.0;
    }
    position = largePosition * exp2((float)octaves1);
    position.y = -0.05 * cameraRightTime.w;
    [loop]
    for (int index = octaves1; index < octaves2; ++index) {
        position = (position.yzx + position.zyx * float3(1.0, -1.0, 1.0)) / sqrt(2.0);
        value = value * 1.5 + pow(abs(reference_noise(position).x - 0.5) * 2.0, 1.0);
        position *= 2.0;
    }
    value /= 1500.0;
    value -= reference_noise_integer(fragCoord) * 0.01;
    return pow(smoothstep(0.4, -0.1, value), 6.0);
}

float reference_ocean_distance(float3 position) {
    return position.y - reference_waves(position);
}

float reference_ocean_distance_detail(float3 position) {
    return position.y - reference_waves_detail(position);
}

float3 reference_ocean_normal(float3 position) {
    const float delta = 0.01 * length(position);
    float3 normal;
    normal.x = reference_ocean_distance_detail(position + float3(delta, 0.0, 0.0)) -
        reference_ocean_distance_detail(position - float3(delta, 0.0, 0.0));
    normal.y = reference_ocean_distance_detail(position + float3(0.0, delta, 0.0)) -
        reference_ocean_distance_detail(position - float3(0.0, delta, 0.0));
    normal.z = reference_ocean_distance_detail(position + float3(0.0, 0.0, delta)) -
        reference_ocean_distance_detail(position - float3(0.0, 0.0, delta));
    return normalize(normal);
}

float reference_trace_ocean(float3 position, float3 ray) {
    float distanceField = 1.0;
    float distance = 0.0;
    [loop]
    for (int index = 0; index < 100; ++index) {
        if (distanceField < 0.01 || distance > 100.0) break;
        distanceField = reference_ocean_distance(position + distance * ray);
        distance += distanceField;
    }
    return distanceField > 0.1 ? 0.0 : distance;
}

struct BoatTransform {
    float3 right;
    float3 up;
    float3 forward;
    float3 position;
};

BoatTransform reference_compute_boat_transform() {
    float3 samples[5];
    samples[0] = float3(0.0, 0.0, 0.0);
    samples[1] = float3(0.0, 0.0, 0.5);
    samples[2] = float3(0.0, 0.0, -0.5);
    samples[3] = float3(0.5, 0.0, 0.0);
    samples[4] = float3(-0.5, 0.0, 0.0);
    [unroll]
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

float3 reference_world_to_boat(float3 direction, BoatTransform boat) {
    return float3(dot(direction, boat.right), dot(direction, boat.up), dot(direction, boat.forward));
}

float reference_trace_boat(float3 position, float3 ray, float3 boatPosition) {
    float3 center = boatPosition - position;
    float distance = dot(center, ray);
    float perpendicular = length(center - distance * ray);
    if (perpendicular > 1.0) return 0.0;
    return distance - sqrt(1.0 - perpendicular * perpendicular);
}

float3 reference_sky(float3 ray) {
    float3 color = float3(0.4, 0.45, 0.5);
    const float sunCosine = saturate(dot(normalize(ray), normalize(lightingParameters.xyz)));
    color += float3(1.0, 0.82, 0.58) * pow(sunCosine, 256.0) * lightingParameters.w * 0.35;
    return color;
}

float3 reference_shade_boat(float3 position, float3 ray, BoatTransform boat) {
    position -= boat.position;
    const float3 normal = normalize(position);
    position = reference_world_to_boat(position, boat);
    const float3 lightDirection = normalize(lightingParameters.xyz);
    const float diffuse = dot(normal, lightDirection);
    const float3 light = smoothstep(-0.1, 1.0, diffuse) * float3(1.0, 0.9, 0.8) + float3(0.06, 0.1, 0.1);
    const float antialias = 4.0 / max(resolutionMouse.x, 1.0);
    float3 albedo = float3(1.0, 0.01, 0.0);
    albedo = lerp(float3(0.04, 0.04, 0.04), albedo, smoothstep(0.25 - antialias, 0.25, abs(position.y)));
    albedo = lerp(lerp(float3(1.0, 1.0, 1.0), float3(0.04, 0.04, 0.04),
        smoothstep(-antialias * 4.0, antialias * 4.0, cos(atan2(position.x, position.z) * 6.0))),
        albedo, smoothstep(0.2 - antialias * 1.5, 0.2, abs(position.y)));
    albedo = lerp(float3(0.04, 0.04, 0.04), albedo,
        smoothstep(0.05 - antialias, 0.05, abs(abs(position.y) - 0.6)));
    albedo = lerp(float3(1.0, 0.8, 0.08), albedo,
        smoothstep(0.05 - antialias, 0.05, abs(abs(position.y) - 0.65)));

    float3 color = albedo * light;
    const float3 halfVector = normalize(lightDirection - ray);
    const float specular = pow(max(0.0, dot(normal, halfVector)), 100.0) * 100.0 / 32.0;
    float3 specularColor = specular.xxx;
    const float3 reflectedRay = reflect(ray, normal);
    specularColor += lerp(float3(0.0, 0.04, 0.04), reference_sky(reflectedRay),
        smoothstep(-0.1, 0.1, reflectedRay.y));
    const float fresnel = lerp(0.001, 1.0, pow(1.0 - abs(dot(normal, ray)), 5.0));
    return lerp(color, specularColor, fresnel);
}

float3 reference_shade_ocean(float3 position, float3 ray, float2 fragCoord, BoatTransform boat) {
    const float3 normal = reference_ocean_normal(position);
    const float ndotr = dot(ray, normal);
    const float fresnel = pow(1.0 - abs(ndotr), 5.0);
    const float3 reflectedRay = ray - 2.0 * normal * ndotr;
    float3 refractedRay = ray + (-cos(1.33 * acos(-ndotr)) - ndotr) * normal;
    refractedRay = normalize(refractedRay);

    float3 reflection = reference_sky(reflectedRay);
    float distance = reference_trace_boat(position, reflectedRay, boat.position);
    if (distance > 0.0) reflection = reference_shade_boat(position + distance * reflectedRay, reflectedRay, boat);

    distance = reference_trace_boat(position, refractedRay, boat.position);
    float3 color = float3(0.0, 0.04, 0.04);
    if (distance > 0.0) color = lerp(color,
        reference_shade_boat(position + distance * refractedRay, refractedRay, boat), exp(-distance));
    color = lerp(color, reflection, fresnel);
    color = lerp(color, float3(1.0, 1.0, 1.0), reference_wave_crests(position, fragCoord));
    return color;
}

float4 PSMain(OceanVertexOutput input) : SV_Target {
    const float2 fragCoord = input.uv * resolutionMouse.xy;
    const float2 rayCoordinate = fragCoord - resolutionMouse.xy * 0.5;
    const float3 ray = normalize(cameraForwardTan.xyz +
        cameraRightTime.xyz * rayCoordinate.x / max(resolutionMouse.y, 1.0) +
        cameraUpAspect.xyz * rayCoordinate.y / max(resolutionMouse.y, 1.0));
    const BoatTransform boat = reference_compute_boat_transform();
    const float oceanDistance = reference_trace_ocean(cameraPosition.xyz, ray);
    const float boatDistance = reference_trace_boat(cameraPosition.xyz, ray, boat.position);

    float3 result;
    if (oceanDistance > 0.0 && (oceanDistance < boatDistance || boatDistance == 0.0))
        result = reference_shade_ocean(cameraPosition.xyz + ray * oceanDistance, ray, fragCoord, boat);
    else if (boatDistance > 0.0)
        result = reference_shade_boat(cameraPosition.xyz + ray * boatDistance, ray, boat);
    else
        result = reference_sky(ray);

    result *= 1.1 * smoothstep(0.35, 1.0, dot(ray, cameraForwardTan.xyz));
    return float4(pow(max(result, 0.0), 1.0 / 2.2), 1.0);
}
