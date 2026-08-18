struct SceneVertex {
    float4 position : SV_Position;
};

cbuffer FrameState : register(b2) {
    float4 timeFrame;
    float4 cameraState;
};

float DecodeTime() {
    return timeFrame.x;
}

float3 DecodeCameraPosition() {
    float4 data = cameraState;
    float yaw = (data.r - 0.5) * 6.28318530718;
    float pitch = (data.g - 0.5) * 2.4;
    float distance = lerp(18.0, 26.0, data.b);
    return float3(sin(yaw) * cos(pitch), sin(pitch), cos(yaw) * cos(pitch)) * distance;
}

SceneVertex VSMain(uint vertexId : SV_VertexID) {
    static const float2 positions[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    SceneVertex output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

static const float CONST_M = 0.5;
static const float SPIN = 0.997114514;
static const float CHARGE = 0.08;
static const float INNER_DISK = 2.0;
static const float OUTER_DISK = 20.0;

float hash21(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float3 blackbody(float temperature) {
    float t = saturate((temperature - 900.0) / 9000.0);
    float3 cold = float3(1.0, 0.12, 0.015);
    float3 hot = float3(1.0, 0.78, 0.32);
    return lerp(cold, hot, t) * (0.65 + 0.35 * t);
}

float3 background(float3 direction) {
    float3 color = float3(0.002, 0.003, 0.008);
    float2 sky = normalize(direction.xz + float2(0.0001, 0.0001));
    float stars = smoothstep(0.995, 1.0, hash21(floor(direction.xy * 900.0)));
    color += stars * float3(0.45, 0.65, 1.0) * (0.4 + 0.6 * abs(direction.y));
    color += exp(-18.0 * abs(direction.y + 0.08)) * float3(0.06, 0.025, 0.012);
    color += 0.015 * float3(abs(sky.x), abs(sky.y), 1.0);
    return color;
}

float4 TraceKerrRay(float2 uv, float3 cameraPosition, float time) {
    float aspect = 1280.0 / 720.0;
    float3 forward = normalize(-cameraPosition);
    float3 right = normalize(cross(forward, float3(-0.5, 1.0, 0.0)));
    float3 up = normalize(cross(right, forward));
    float3 direction = normalize(forward + right * ((uv.x * 2.0 - 1.0) * aspect * 0.72) + up * ((uv.y * 2.0 - 1.0) * 0.72));
    float3 position = cameraPosition;
    float3 ray = direction;
    float4 accumulated = float4(0.0, 0.0, 0.0, 0.0);
    float previousY = position.y;
    float horizonDiscriminant = CONST_M * CONST_M - (SPIN * CONST_M) * (SPIN * CONST_M) - CHARGE * CHARGE;
    float eventHorizon = CONST_M + sqrt(max(0.001, horizonDiscriminant));

    [loop]
    for (int stepIndex = 0; stepIndex < 120; ++stepIndex) {
        float radius = length(position);
        if (radius < eventHorizon) {
            accumulated.a = 1.0;
            break;
        }
        if (radius > 180.0) break;

        float stepSize = clamp(radius * 0.035, 0.018, 0.42);
        float3 radial = -position / max(radius * radius * radius, 0.0001);
        float3 frameDrag = cross(float3(0.0, 1.0, 0.0), position) * (SPIN * CONST_M / max(radius * radius * radius, 0.0001));
        ray = normalize(ray + (1.65 * radial + 0.42 * frameDrag) * stepSize);
        float3 nextPosition = position + ray * stepSize;

        if (previousY * nextPosition.y <= 0.0) {
            float crossing = previousY / max(previousY - nextPosition.y, 0.00001);
            float3 hit = lerp(position, nextPosition, crossing);
            float diskRadius = length(hit.xz);
            if (diskRadius > INNER_DISK && diskRadius < OUTER_DISK) {
                float density = smoothstep(OUTER_DISK, INNER_DISK, diskRadius);
                float temperature = 950.0 + 13000.0 * pow(max(density, 0.001), 0.42);
                float doppler = 0.65 + 0.35 * dot(normalize(hit.xz), normalize(ray.xz));
                float3 diskColor = blackbody(temperature) * density * (1.0 + 2.5 * doppler);
                diskColor *= 0.92 + 0.08 * sin(time * 0.8 + atan2(hit.z, hit.x) * 3.0);
                accumulated.rgb += diskColor * (1.0 - accumulated.a);
                accumulated.a = saturate(accumulated.a + density * 0.22);
            }
        }

        float cylindricalRadius = length(position.xz);
        float jet = (cylindricalRadius < 1.1 + 0.08 * abs(position.y)) ? exp(-0.18 * cylindricalRadius * cylindricalRadius) : 0.0;
        float jetFade = exp(-0.045 * abs(position.y));
        accumulated.rgb += jet * jetFade * float3(0.18, 0.42, 1.0) * stepSize * 0.18;
        previousY = nextPosition.y;
        position = nextPosition;
        if (accumulated.a > 0.985) break;
    }

    float3 escaped = background(ray);
    accumulated.rgb += escaped * (1.0 - accumulated.a);
    accumulated.a = 1.0;
    return accumulated;
}

float3 toneMap(float3 color) {
    color = color / (1.0 + color);
    color = pow(max(color, 0.0), float3(0.75, 0.8, 0.9));
    return saturate(color);
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    float2 uv = position.xy / float2(1280.0, 720.0);
    float time = DecodeTime();
    float3 color = TraceKerrRay(uv, DecodeCameraPosition(), time).rgb;
    float2 center = uv - float2(0.5, 0.5);
    float diskGlow = exp(-90.0 * abs(center.y + 0.02)) * exp(-2.0 * length(center) * length(center));
    color += diskGlow * float3(0.65, 0.18, 0.025);
    return float4(toneMap(color), 1.0);
}
