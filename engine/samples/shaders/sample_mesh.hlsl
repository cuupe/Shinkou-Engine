cbuffer SceneFrame : register(b2) {
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

StructuredBuffer<float4x4> instanceModels : register(t4);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(float3 position : POSITION, uint instanceId : SV_InstanceID) {
    VertexOutput output;
    float4 worldPosition = mul(float4(position, 1.0), instanceModels[instanceId]);
    output.position = mul(worldPosition, viewProjection);
    output.uv = position.xy * 0.5 + 0.5;
    return output;
}
