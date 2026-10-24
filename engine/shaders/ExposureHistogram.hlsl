#ifdef SHINKOU_SPIRV
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif
SLOT(0) RWStructuredBuffer<uint> HistogramOutput:register(u0);
#ifdef SHINKOU_HISTOGRAM_BUILD
SLOT(1) Texture2D SourceImage:register(t1);
// Matches the shared ImageParameters ABI; metering starts at byte 256.
SLOT(4) cbuffer Parameters:register(b4) {
    float4 settings;
    float4 viewport;
    float4x4 inverseProjection;
    float4x4 projection;
    float4x4 reprojection;
    float4 tuning;
    float4 temporal;
    float4 metering;
};
groupshared uint localHistogram[256];
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID,uint index:SV_GroupIndex) {
    localHistogram[index]=0;
    GroupMemoryBarrierWithGroupSync();
    uint width,height;SourceImage.GetDimensions(width,height);
    if (id.x<width && id.y<height) {
        float luminance=dot(max(SourceImage.Load(int3(id.xy,0)).rgb,0),float3(0.2126,0.7152,0.0722));
        uint bin=0;
        if (luminance>1e-6) bin=1+(uint)(saturate((log2(luminance)-metering.x)/(metering.y-metering.x))*254);
        InterlockedAdd(localHistogram[bin],1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (localHistogram[index]) InterlockedAdd(HistogramOutput[index],localHistogram[index]);
}
#else
[numthreads(256,1,1)]
void main(uint index:SV_GroupIndex) {HistogramOutput[index]=0;}
#endif
