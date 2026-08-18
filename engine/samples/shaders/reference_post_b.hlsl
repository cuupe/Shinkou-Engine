struct PSInput { float4 position : SV_Position; };
Texture2D sourceImage : register(t0);

float4 PSMain(PSInput input) : SV_Target {
    int2 p = int2(input.position.xy);
    float3 color = sourceImage.Load(int3(p, 0)).rgb;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float mask = smoothstep(0.45, 1.25, luminance);
    float3 bloom = max(color - 0.35.xxx, 0.0.xxx) * (0.45 + mask * 0.9);
    return float4(bloom, 1.0);
}
