#ifdef SHINKOU_SPIRV
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif

#ifdef SHINKOU_EXPOSURE_HISTOGRAM_TEMPORAL
#define SHINKOU_EXPOSURE_TEMPORAL 1
#define SHINKOU_EXPOSURE_HISTOGRAM 1
#endif
#if defined(SHINKOU_EXPOSURE_TEMPORAL) || defined(SHINKOU_EXPOSURE_HISTOGRAM)
#define SHINKOU_EXPOSURE 1
#endif

struct FullscreenVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
#ifdef SHINKOU_VERTEX
FullscreenVertex main(uint id : SV_VertexID) {
    const float2 positions[6] = {float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    FullscreenVertex output;
    output.position = float4(positions[id],0,1);
    output.uv = positions[id]*float2(0.5,-0.5)+0.5;
    return output;
}
#else
SLOT(0) Texture2D Source : register(t0);
SLOT(1) SamplerState LinearClamp : register(s1);
#ifdef SHINKOU_EXPOSURE_HISTOGRAM
SLOT(9) StructuredBuffer<uint> LuminanceHistogram:register(t9);
#endif
#if defined(SHINKOU_SSR) || defined(SHINKOU_TAA) || defined(SHINKOU_MOTION_BLUR)
SLOT(2) Texture2D Depth : register(t2);
#endif
#if defined(SHINKOU_TAA) || defined(SHINKOU_EXPOSURE_TEMPORAL)
SLOT(3) Texture2D History : register(t3);
#endif
#if defined(SHINKOU_BLOOM_UP)
SLOT(3) Texture2D Secondary : register(t3);
#endif
#if defined(SHINKOU_TAA) || defined(SHINKOU_MOTION_BLUR)
SLOT(8) Texture2D MotionVectors : register(t8);
#endif
#if defined(SHINKOU_TONE_MAP)
SLOT(5) Texture2D Bloom : register(t5);
SLOT(6) Texture2D Exposure : register(t6);
SLOT(7) Texture2D AmbientOcclusion : register(t7);
#endif
#ifndef SHINKOU_PRESENT
SLOT(4) cbuffer Parameters : register(b4) {
    float4 settings;
    float4 viewport; // inverse dimensions, history valid, reserved
    float4x4 inverseProjection;
    float4x4 projection;
    float4x4 reprojection;
    float4 tuning;
    float4 temporal; // seconds, reserved, object motion available, reserved
    float4 metering; // log luminance range, low/high percentiles
};
float3 view_position(float2 uv, float depth) {
    float4 viewPoint = mul(inverseProjection,float4(uv*float2(2,-2)+float2(-1,1),depth,1));
    return viewPoint.xyz / max(abs(viewPoint.w),1e-6) * sign(viewPoint.w);
}
#endif

#if defined(SHINKOU_EXPOSURE)
float exposure_value() {
#ifdef SHINKOU_EXPOSURE_HISTOGRAM
    uint count=0;
    [loop] for (uint bin=1;bin<256;++bin) count+=LuminanceHistogram[bin];
    if (!count) return tuning.y;
    float lower=count*metering.z,upper=count*metering.w;
    float prefix=0,weightedLog=0,weight=0;
    [loop] for (uint index=1;index<256;++index) {
        float samples=(float)LuminanceHistogram[index];
        float retained=max(0,min(prefix+samples,upper)-max(prefix,lower));
        float binLog=lerp(metering.x,metering.y,min(1.0,(index-1+0.5)/254));
        weightedLog+=binLog*retained;weight+=retained;prefix+=samples;
    }
    float luminance=exp2(weightedLog/max(weight,1e-5));
    return clamp(tuning.z/max(luminance,1e-6)*settings.x*exp2(settings.w),tuning.x,tuning.y);
#else
    float sum=0;
    [loop] for (uint y=0;y<16;++y) [loop] for (uint x=0;x<16;++x) {
        float3 color=Source.SampleLevel(LinearClamp,(float2(x,y)+0.5)/16,0).rgb;
        sum+=log(max(dot(color,float3(0.2126,0.7152,0.0722)),1e-4));
    }
    return clamp(tuning.z/exp(sum/256)*settings.x*exp2(settings.w),tuning.x,tuning.y);
#endif
}
#endif
#if defined(SHINKOU_BLOOM) || defined(SHINKOU_BLOOM_PREFILTER) || defined(SHINKOU_BLOOM_DOWN) || defined(SHINKOU_BLOOM_UP)
float3 bloom_filter(float2 uv) {
    float3 sum=0;
    [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
        float weight=(x==0?2.0:1.0)*(y==0?2.0:1.0);
        sum+=Source.SampleLevel(LinearClamp,uv+float2(x,y)*viewport.xy,0).rgb*weight;
    }
    return sum/16;
}
#endif
#if defined(SHINKOU_TAA) || defined(SHINKOU_EXPOSURE_TEMPORAL)
struct TemporalOutput { float4 color:SV_Target0; float4 history:SV_Target1; };
TemporalOutput main(FullscreenVertex input) {
#ifdef SHINKOU_EXPOSURE_TEMPORAL
    float target=exposure_value();
    float exposure=target;
    if (viewport.z>0.5) {
        float old=History.SampleLevel(LinearClamp,float2(0.5,0.5),0).r;
        float rate=target>old && settings.z>0 ? settings.z:settings.y;
        exposure=lerp(clamp(old,tuning.x,tuning.y),target,1-exp(-rate*temporal.x));
    }
    TemporalOutput output;output.color=float4(exposure,0,0,1);output.history=output.color;return output;
#else
    float3 current=Source.SampleLevel(LinearClamp,input.uv,0).rgb;
    float depth=Depth.SampleLevel(LinearClamp,input.uv,0).r;
    TemporalOutput output;
    if (viewport.z<0.5) {output.color=float4(current,1);output.history=float4(current,depth);return output;}
    float4 previous=mul(reprojection,float4(input.uv*float2(2,-2)+float2(-1,1),depth,1));
    float2 historyUv=previous.xy/max(abs(previous.w),1e-6)*float2(0.5,-0.5)+0.5;
    float previousDepth=previous.z/max(abs(previous.w),1e-6);
    float valid=previous.w>0?1.0:0.0;
    if (temporal.z>0.5) {
        float4 motion=MotionVectors.SampleLevel(LinearClamp,input.uv,0);
        historyUv=input.uv+motion.xy;previousDepth=motion.z;valid=motion.w;
    }
    float3 lo=current,hi=current;
    [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
        float3 sampleColor=Source.SampleLevel(LinearClamp,input.uv+float2(x,y)*viewport.xy,0).rgb;
        lo=min(lo,sampleColor);hi=max(hi,sampleColor);
    }
    float4 history=History.SampleLevel(LinearClamp,historyUv,0);
    float3 old=clamp(history.rgb,lo,hi);
    float difference=length(current-old)/max(length(current),0.05);
    float feedback=settings.x*(1-saturate(difference*settings.z));
    if (valid<0.5 || any(historyUv<0) || any(historyUv>1) || abs(history.a-previousDepth)>settings.w) feedback=0;
    float3 color=lerp(current,old,feedback);
    color=max(0,color+(current-(lo+hi)*0.5)*settings.y);
    output.color=float4(color,1);output.history=float4(color,depth);return output;
#endif
}
#else
float4 main(FullscreenVertex input) : SV_Target {
#if defined(SHINKOU_SSAO)
    float centerDepth = Source.SampleLevel(LinearClamp,input.uv,0).r;
    if (centerDepth >= 0.999999) return 1;
    float3 center = view_position(input.uv,centerDepth);
    float3 normal = normalize(cross(ddx(center),ddy(center)));
    float occlusion = 0;
    uint count = min(64u,max(1u,(uint)settings.w));
    float pixelRadius = min(0.1,settings.x/max(abs(center.z),0.05));
    for (uint i=0;i<count;++i) {
        float angle = (i+0.5)*2.39996323;
        float2 offset = float2(cos(angle),sin(angle))*sqrt((i+0.5)/count)*pixelRadius;
        float sampleDepth = Source.SampleLevel(LinearClamp,saturate(input.uv+offset),0).r;
        float3 delta = view_position(input.uv+offset,sampleDepth)-center;
        float distance = length(delta);
        float horizon = max(0,dot(normal,delta)/max(distance,1e-5)-settings.z);
        occlusion += horizon*saturate(1.0-distance/max(settings.x,1e-4));
    }
    return pow(saturate(1.0-occlusion/count*2.0),settings.y).xxxx;
#elif defined(SHINKOU_SSR)
    float3 color = Source.SampleLevel(LinearClamp,input.uv,0).rgb;
    float depth = Depth.SampleLevel(LinearClamp,input.uv,0).r;
    if (depth >= 0.999999) return float4(color,1);
    float3 viewPoint = view_position(input.uv,depth);
    float3 normal = normalize(cross(ddx(viewPoint),ddy(viewPoint)));
    float3 ray = reflect(normalize(viewPoint),normal);
    float stepLength = settings.x / max(settings.w,1.0);
    for (uint i=1;i<=min(256u,(uint)settings.w);++i) {
        float3 candidate = viewPoint+normal*settings.y+ray*(i*stepLength*settings.z);
        float4 clip = mul(projection,float4(candidate,1));
        if (clip.w <= 0) break;
        float2 uv = clip.xy/clip.w*float2(0.5,-0.5)+0.5;
        if (any(uv<0) || any(uv>1)) break;
        float3 surface = view_position(uv,Depth.SampleLevel(LinearClamp,uv,0).r);
        float difference = candidate.z-surface.z;
        if (difference>0 && difference<settings.y) {
            float edge = saturate(min(min(uv.x,1-uv.x),min(uv.y,1-uv.y))*8.0);
            color = lerp(color,Source.SampleLevel(LinearClamp,uv,0).rgb,edge*0.25);
            break;
        }
    }
    return float4(color,1);
#elif defined(SHINKOU_BLOOM_PREFILTER)
    float3 color=bloom_filter(input.uv);
    float brightness=max(color.r,max(color.g,color.b));
    float knee=max(settings.x*settings.y,1e-5);
    float soft=clamp(brightness-settings.x+knee,0,2*knee);
    soft=soft*soft/(4*knee+1e-5);
    return float4(color*max(brightness-settings.x,soft)/max(brightness,1e-5),1);
#elif defined(SHINKOU_BLOOM_DOWN)
    return float4(bloom_filter(input.uv),1);
#elif defined(SHINKOU_BLOOM_UP)
    float3 low=0;
    [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
        float weight=(x==0?2.0:1.0)*(y==0?2.0:1.0);
        low+=Secondary.SampleLevel(LinearClamp,input.uv+float2(x,y)*viewport.xy,0).rgb*weight;
    }
    return float4(lerp(Source.SampleLevel(LinearClamp,input.uv,0).rgb,low/16,tuning.x),1);
#elif defined(SHINKOU_BLOOM)
    return float4(bloom_filter(input.uv)*settings.z,1);
#elif defined(SHINKOU_EXPOSURE)
    return float4(exposure_value(),0,0,1);
#elif defined(SHINKOU_MOTION_BLUR)
    float4 current=Source.SampleLevel(LinearClamp,input.uv,0);
    float4 motion=MotionVectors.SampleLevel(LinearClamp,input.uv,0);
    if (motion.w<0.5) return current;
    float2 pixels=motion.xy/viewport.xy*settings.x;
    float radius=length(pixels);
    if (radius<0.5) return current;
    float2 velocity=pixels*(min(radius,settings.y)/max(radius,1e-5))*viewport.xy;
    float depth=Depth.SampleLevel(LinearClamp,input.uv,0).r;
    float3 sum=current.rgb;float weights=1;
    uint count=min(32u,max(2u,(uint)settings.w));
    for (uint i=0;i<count;++i) {
        float2 uv=input.uv+velocity*((float)i/(count-1)-0.5);
        if (any(uv<0) || any(uv>1)) continue;
        float sampleDepth=Depth.SampleLevel(LinearClamp,uv,0).r;
        float weight=1-saturate(abs(sampleDepth-depth)/max(settings.z,1e-5));
        sum+=Source.SampleLevel(LinearClamp,uv,0).rgb*weight;weights+=weight;
    }
    return float4(sum/weights,current.a);
#elif defined(SHINKOU_TONE_MAP)
    float3 color = Source.SampleLevel(LinearClamp,input.uv,0).rgb;
    color *= AmbientOcclusion.SampleLevel(LinearClamp,input.uv,0).r;
    color += Bloom.SampleLevel(LinearClamp,input.uv,0).rgb;
    color *= Exposure.SampleLevel(LinearClamp,float2(0.5,0.5),0).r*settings.x;
    color = saturate((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14));
    color = lerp(color*12.92,1.055*pow(max(color,0),1.0/2.4)-0.055,step(0.0031308.xxx,color));
    return float4(color,1);
#else
    return Source.SampleLevel(LinearClamp,input.uv,0);
#endif
}
#endif
#endif
