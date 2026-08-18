struct PSInput { float4 position : SV_Position; };
Texture2D colorImage : register(t0);
Texture2D bloomImage : register(t1);

float3 tonemap(float3 color) {
    color = max(color, 0.0.xxx);
    color = color / (color + 1.0.xxx);
    return pow(color, 1.0 / 2.2);
}

float3 sampleBilinear(Texture2D image, float2 uv, uint width, uint height) {
    float2 coordinate = uv * float2(width, height) - 0.5;
    int2 base = int2(floor(coordinate));
    float2 weight = frac(coordinate);
    int2 maximum = int2(width - 1, height - 1);
    int2 p00 = clamp(base, int2(0, 0), maximum);
    int2 p10 = clamp(base + int2(1, 0), int2(0, 0), maximum);
    int2 p01 = clamp(base + int2(0, 1), int2(0, 0), maximum);
    int2 p11 = clamp(base + int2(1, 1), int2(0, 0), maximum);
    float3 a = lerp(image.Load(int3(p00, 0)).rgb, image.Load(int3(p10, 0)).rgb, weight.x);
    float3 b = lerp(image.Load(int3(p01, 0)).rgb, image.Load(int3(p11, 0)).rgb, weight.x);
    return lerp(a, b, weight.y);
}

float4 PSMain(PSInput input) : SV_Target {
    float2 uv = input.position.xy / float2(1280.0, 720.0);
    uint colorWidth = 0;
    uint colorHeight = 0;
    colorImage.GetDimensions(colorWidth, colorHeight);
    uint bloomWidth = 0;
    uint bloomHeight = 0;
    bloomImage.GetDimensions(bloomWidth, bloomHeight);
    float3 color = sampleBilinear(colorImage, uv, colorWidth, colorHeight);
    float3 bloom = sampleBilinear(bloomImage, uv, bloomWidth, bloomHeight);
    return float4(tonemap(color + bloom * 0.22), 1.0);
}
