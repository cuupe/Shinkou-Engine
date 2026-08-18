struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

cbuffer SceneFrame : register(b2) {
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

cbuffer ObjectFrame : register(b3) {
    float4x4 model;
};

VertexOutput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    VertexOutput output;
    const float4 localPosition = float4(positions[vertexId], 0.0, 1.0);
    const float4 worldPosition = mul(localPosition, model);
    output.position = cameraPositionAndFlags.w > 1.5
        ? localPosition
        : mul(worldPosition, viewProjection);
    output.uv = positions[vertexId] * 0.5 + 0.5;
    return output;
}

// The portable sample material uses a fixed one-element descriptor. Bindless
// arrays are exercised by the dedicated GPU-driven shaders instead.
Texture2D gTextures[1] : register(t0);
SamplerState gSampler : register(s1);

float4 PSMain(VertexOutput input) : SV_Target {
    float sceneTint = 0.9 + 0.1 * (0.5 + 0.5 * sin(cameraPositionAndFlags.z));
    return gTextures[0].Sample(gSampler, input.uv) * sceneTint;
}
