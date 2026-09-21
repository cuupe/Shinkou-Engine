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
    float4 worldPosition = mul(instanceModels[instanceId], float4(position, 1.0));
    output.position = mul(viewProjection, worldPosition);
    output.uv = position.xy * 0.5 + 0.5;
    return output;
}
