struct MeshVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
};

cbuffer SceneFrame : register(b2) {
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

cbuffer ObjectFrame : register(b3) {
    float4x4 model;
};

MeshVertexOutput MeshVS(float3 position : POSITION) {
    MeshVertexOutput output;
    const float3 worldPosition = mul(model, float4(position, 1.0)).xyz;
    output.position = mul(viewProjection, float4(worldPosition, 1.0));
    output.uv = position.xy * 0.5 + 0.5;
    output.worldPosition = worldPosition;
    return output;
}

Texture2D gTexture : register(t0);
SamplerState gSampler : register(s1);

float4 MeshPS(MeshVertexOutput input) : SV_Target {
    const float3 dx = ddx(input.worldPosition);
    const float3 dy = ddy(input.worldPosition);
    const float3 normal = normalize(cross(dx, dy));
    const float3 lightDirection = normalize(float3(0.35, 0.65, 0.75));
    const float diffuse = 0.22 + 0.78 * saturate(dot(normal, lightDirection));
    const float4 albedo = gTexture.Sample(gSampler, input.uv);
    return float4(albedo.rgb * diffuse, albedo.a);
}

struct PresentVertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

PresentVertexOutput PresentVS(uint vertexId : SV_VertexID) {
    const float2 positions[6] = {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0),
        float2(-1.0, -1.0), float2(1.0, 1.0), float2(1.0, -1.0)
    };
    PresentVertexOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = positions[vertexId] * 0.5 + 0.5;
    return output;
}

float4 PresentPS(PresentVertexOutput input) : SV_Target {
    return gTexture.Sample(gSampler, input.uv);
}
