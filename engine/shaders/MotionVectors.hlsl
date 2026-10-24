#ifdef SHINKOU_SPIRV
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif
SLOT(2) cbuffer MotionCamera : register(b2) {
    float4x4 currentViewProjection;
    float4x4 previousViewProjection;
    float4x4 inverseCurrentViewProjection;
    float4 cameraPosition;
    float4 motionFlags; // history valid, orthographic, reserved
};
#if defined(SHINKOU_MOTION_BACKGROUND_VERTEX) || defined(SHINKOU_MOTION_BACKGROUND_FRAGMENT)
struct BackgroundVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
#ifdef SHINKOU_MOTION_BACKGROUND_VERTEX
BackgroundVertex main(uint id:SV_VertexID) {
    const float2 positions[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    BackgroundVertex output;output.position=float4(positions[id],0,1);
    output.uv=positions[id]*float2(0.5,-0.5)+0.5;return output;
}
#else
SLOT(0) Texture2D SceneDepth:register(t0);
SLOT(1) SamplerState DepthSampler:register(s1);
float4 main(BackgroundVertex input):SV_Target {
    if (SceneDepth.SampleLevel(DepthSampler,input.uv,0).r<0.999999 || motionFlags.x<0.5) return float4(0,0,1,0);
    if (motionFlags.y>0.5) return float4(0,0,1,1);
    float4 world=mul(inverseCurrentViewProjection,float4(input.uv*float2(2,-2)+float2(-1,1),1,1));
    // A direction at infinity removes both current and previous translation.
    float3 direction=world.xyz-cameraPosition.xyz*world.w;
    float4 previous=mul(previousViewProjection,float4(direction,0));
    float2 previousUv=previous.xy/max(abs(previous.w),1e-6)*float2(0.5,-0.5)+0.5;
    return float4(previousUv-input.uv,1,previous.w>0?1.0:0.0);
}
#endif
#else
SLOT(3) cbuffer MotionObject : register(b3) {
    float4x4 currentModel;
    float4x4 previousModel;
    float4 objectHistory;
};
struct MotionVertex {
    float4 position:SV_Position;
    float4 currentClip:TEXCOORD0;
    float4 previousClip:TEXCOORD1;
};
#ifdef SHINKOU_MOTION_VERTEX
MotionVertex main(float3 position:POSITION) {
    MotionVertex output;
    output.currentClip=mul(currentViewProjection,mul(currentModel,float4(position,1)));
    output.previousClip=mul(previousViewProjection,mul(previousModel,float4(position,1)));
    output.position=output.currentClip;
    return output;
}
#else
float4 main(MotionVertex input):SV_Target {
    float3 current=input.currentClip.xyz/max(abs(input.currentClip.w),1e-6);
    float3 previous=input.previousClip.xyz/max(abs(input.previousClip.w),1e-6);
    float2 velocity=(previous.xy-current.xy)*float2(0.5,-0.5);
    float valid=objectHistory.x>0.5 && input.previousClip.w>0 && previous.z>=0 && previous.z<=1 ? 1.0:0.0;
    return float4(valid>0.5?velocity:float2(0,0),previous.z,valid);
}
#endif
#endif
