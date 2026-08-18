struct PSInput { float4 position : SV_Position; };
Texture2D sourceImage : register(t0);

float3 sampleImage(int2 p) { return sourceImage.Load(int3(clamp(p, int2(0, 0), int2(1279, 719)), 0)).rgb; }

float4 PSMain(PSInput input) : SV_Target {
    int2 p = int2(input.position.xy);
    float3 color = sampleImage(p) * 0.227027;
    color += sampleImage(p + int2(2, 0)) * 0.316216;
    color += sampleImage(p - int2(2, 0)) * 0.316216;
    color += sampleImage(p + int2(4, 0)) * 0.070270;
    color += sampleImage(p - int2(4, 0)) * 0.070270;
    return float4(color, 1.0);
}
