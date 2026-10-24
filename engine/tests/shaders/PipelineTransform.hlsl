#ifdef SHINKOU_SPIRV
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif
SLOT(0) Texture2D Source : register(t0);
SLOT(1) SamplerState LinearClamp : register(s1);
SLOT(4) cbuffer Transform : register(b4) { float4 scale; float4 bias; };
float4 main(float4 position:SV_Position,float2 uv:TEXCOORD0):SV_Target {
    return Source.SampleLevel(LinearClamp,uv,0)*scale+bias;
}
