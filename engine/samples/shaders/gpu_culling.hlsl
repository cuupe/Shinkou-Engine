struct InstanceData {
    float4x4 model;
};

StructuredBuffer<InstanceData> instances : register(t0);
struct DrawArguments {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};
RWStructuredBuffer<DrawArguments> arguments : register(u1);
cbuffer SceneFrame : register(b2) {
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
};

[numthreads(64, 1, 1)]
void CSMain(uint3 dispatchId : SV_DispatchThreadID) {
    const uint index = dispatchId.x;
    const float4 center = mul(instances[index].model, float4(0, 0, 0, 1));
    const float4 clip = mul(viewProjection, center);
    const bool visible = clip.w > 0.0 && abs(clip.x) <= clip.w && abs(clip.y) <= clip.w && clip.z >= 0.0 && clip.z <= clip.w;
    DrawArguments draw = arguments[index];
    draw.instanceCount = visible ? 1 : 0;
    arguments[index] = draw;
}
