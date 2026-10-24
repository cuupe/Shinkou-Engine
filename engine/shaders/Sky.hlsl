#ifdef SHINKOU_SPIRV
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif

struct SkyVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
#ifdef SHINKOU_SKY_VERTEX
SkyVertex main(uint id:SV_VertexID) {
    const float2 positions[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    SkyVertex output;output.position=float4(positions[id],1,1);
    output.uv=positions[id]*float2(0.5,-0.5)+0.5;return output;
}
#else
SLOT(0) Texture2D Environment:register(t0);
SLOT(1) SamplerState EnvironmentSampler:register(s1);
SLOT(4) cbuffer SkyParameters:register(b4) {
    float4x4 inverseProjection;
    float4x4 cameraOrientation;
    float4 zenith;
    float4 horizon;
    float4 ground;
    float4 settings; // intensity, rotation, orthographic, environment present
    float4 flags; // environment uses sRGB, reserved
};
float4 main(SkyVertex input):SV_Target {
    float4 viewPoint=mul(inverseProjection,float4(input.uv*float2(2,-2)+float2(-1,1),1,1));
    float3 ray=settings.z>0.5?float3(0,0,1):viewPoint.xyz;
    float3 direction=normalize(mul(cameraOrientation,float4(ray,0)).xyz);
    float3 color;
    if (settings.w>0.5) {
        float cs=cos(settings.y),sn=sin(settings.y);
        direction.xz=float2(cs*direction.x-sn*direction.z,sn*direction.x+cs*direction.z);
        float2 uv=float2(atan2(direction.z,direction.x)/6.28318530718+0.5,acos(clamp(direction.y,-1,1))/3.14159265359);
        color=Environment.SampleLevel(EnvironmentSampler,uv,0).rgb;
        if (flags.x>0.5) color=lerp(color/12.92,pow((max(color,0)+0.055)/1.055,2.4),step(0.04045.xxx,color));
    } else {
        float height=saturate(abs(direction.y));
        color=lerp(horizon.rgb,direction.y>=0?zenith.rgb:ground.rgb,height);
    }
    return float4(max(color,0)*settings.x,1);
}
#endif
