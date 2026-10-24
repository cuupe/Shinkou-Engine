#ifdef SHINKOU_SPIRV
#include "Pbr.hlsli"
#define SLOT(n) [[vk::binding(n,0)]]
#else
#define SLOT(n)
#endif
SLOT(2) cbuffer SceneFrame : register(b2) {
    float4x4 viewProjection;
    float4 cameraPositionAndFlags;
    float4 lightingParameters;
};
SLOT(3) cbuffer ObjectFrame : register(b3) { float4x4 model; };
struct PbrVertex { float4 position:SV_Position; float2 uv:TEXCOORD0; float3 worldPosition:TEXCOORD1; float3 normal:TEXCOORD2; };
#ifdef SHINKOU_PBR_VERTEX
PbrVertex main(float3 position:POSITION,float2 uv:TEXCOORD0,float3 normal:NORMAL) {
    PbrVertex output;
    output.worldPosition=mul(model,float4(position,1)).xyz;
    output.position=mul(viewProjection,float4(output.worldPosition,1)); output.uv=uv;
    // Cofactor transform is proportional to inverse-transpose, including
    // non-uniform scaling; determinant sign preserves mirrored transforms.
    float3 a=float3(model[0][0],model[1][0],model[2][0]);
    float3 b=float3(model[0][1],model[1][1],model[2][1]);
    float3 c=float3(model[0][2],model[1][2],model[2][2]);
    float determinant=dot(a,cross(b,c));
    output.normal=(cross(b,c)*normal.x+cross(c,a)*normal.y+cross(a,b)*normal.z)*sign(determinant);
    return output;
}
#elif defined(SHINKOU_SHADOW_FRAGMENT)
void main() {}
#else
SLOT(0) Texture2D BaseColor : register(t0);
SLOT(1) SamplerState MaterialSampler : register(s1);
SLOT(4) cbuffer MaterialParameters : register(b4) {
    float4 baseColorFactor;
    float4 emissiveMetallic;
    float4 surface;
    float4 ibl;
    float4 flags;
};
struct LightData { float4 position; float4 direction; float4 colorIntensity; float4 parameters; };
SLOT(5) StructuredBuffer<LightData> lights : register(t5);
SLOT(6) Texture2D NormalMap : register(t6);
SLOT(7) Texture2D MetallicRoughness : register(t7);
SLOT(8) Texture2D Occlusion : register(t8);
SLOT(9) Texture2D Emissive : register(t9);
SLOT(10) Texture2D Irradiance : register(t10);
SLOT(11) Texture2D PrefilteredEnvironment : register(t11);
SLOT(12) Texture2D BrdfLut : register(t12);
SLOT(14) Texture2D ShadowAtlas : register(t14);
SLOT(13) cbuffer ShadowFrame : register(b13) {
    float4x4 shadowViewProjection[8];
    float4 shadowAtlasRect[8];
    float4 shadowDistances[8];
    float4 shadowCameraPosition;
    float4 shadowCameraForward;
    float4 shadowSettings; // cascade count, depth bias, atlas texel size, light index
};
float shadow_visibility(uint lightIndex,float3 worldPosition,float3 normal,float3 lightDirection) {
    uint count=min(8u,(uint)shadowSettings.x);
    if (count==0 || lightIndex!=(uint)shadowSettings.w) return 1;
    float cameraDepth=dot(worldPosition-shadowCameraPosition.xyz,shadowCameraForward.xyz);
    uint cascade=0;
    while (cascade<count && cameraDepth>shadowDistances[cascade].y) ++cascade;
    if (cascade==count || cameraDepth<shadowDistances[cascade].x) return 1;
    float4 position=mul(shadowViewProjection[cascade],float4(worldPosition,1));
    float3 ndc=position.xyz/max(abs(position.w),1e-6);
    if (position.w<=0 || any(abs(ndc.xy)>1) || ndc.z<0 || ndc.z>1) return 1;
    float4 rect=shadowAtlasRect[cascade];
    float2 uv=(ndc.xy*float2(0.5,-0.5)+0.5)*rect.zw+rect.xy;
    float halfTexel=shadowSettings.z*0.5;
    float bias=shadowSettings.y*(1+2*(1-saturate(dot(normal,lightDirection))));
    float visibility=0;
    [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
        float2 sampleUv=clamp(uv+float2(x,y)*shadowSettings.z,rect.xy+halfTexel,rect.xy+rect.zw-halfTexel);
        float stored=ShadowAtlas.SampleLevel(MaterialSampler,sampleUv,0).r;
        visibility+=ndc.z-bias<=stored?1.0:0.0;
    }
    return visibility/9;
}
float3 srgb_to_linear(float3 color) {
    return lerp(color/12.92,pow((max(color,0)+0.055)/1.055,2.4),step(0.04045.xxx,color));
}
float2 environment_uv(float3 direction) {
    float cs=cos(ibl.y),sn=sin(ibl.y);
    direction.xz=float2(cs*direction.x-sn*direction.z,sn*direction.x+cs*direction.z);
    return float2(atan2(direction.z,direction.x)/(2*SHINKOU_PI)+0.5,acos(clamp(direction.y,-1,1))/SHINKOU_PI);
}
float4 main(PbrVertex input,bool frontFace:SV_IsFrontFace):SV_Target {
    float4 sampled=BaseColor.Sample(MaterialSampler,input.uv);
    float3 albedo=baseColorFactor.rgb*(flags.z>0.5?srgb_to_linear(sampled.rgb):sampled.rgb);
    float alpha=baseColorFactor.a*sampled.a;
    if (flags.y>0.5) clip(alpha-surface.w);
    float4 mr=MetallicRoughness.Sample(MaterialSampler,input.uv);
    float metallic=saturate(emissiveMetallic.w*mr.b);
    float roughness=clamp(surface.x*mr.g,0.04,1);
    float3 normal=dot(input.normal,input.normal)>1e-10?normalize(input.normal):normalize(cross(ddx(input.worldPosition),ddy(input.worldPosition)));
    if (!frontFace) normal=-normal;
    if (flags.x>0.5) {
        float3 dpdx=ddx(input.worldPosition),dpdy=ddy(input.worldPosition);
        float2 duvdx=ddx(input.uv),duvdy=ddy(input.uv);
        float3 tangent=dpdx*duvdy.y-dpdy*duvdx.y;
        tangent=normalize(tangent-normal*dot(tangent,normal)+1e-8);
        float handedness=sign(duvdx.x*duvdy.y-duvdx.y*duvdy.x);
        float3 bitangent=normalize(cross(normal,tangent))*handedness;
        float3 mapped=NormalMap.Sample(MaterialSampler,input.uv).xyz*2-1;
        mapped.xy*=surface.y;
        normal=normalize(tangent*mapped.x+bitangent*mapped.y+normal*mapped.z);
    }
    float3 viewDirection=normalize(cameraPositionAndFlags.xyz-input.worldPosition+1e-8);
    float3 color=0;
    uint capacity,stride; lights.GetDimensions(capacity,stride);
    uint count=min(capacity,(uint)lightingParameters.x);
    for (uint index=0;index<count;++index) {
        LightData light=lights[index];
        float3 direction=-light.direction.xyz;
        float attenuation=1;
        if (light.position.w>0.5) {
            float3 delta=light.position.xyz-input.worldPosition;
            float distanceSquared=max(dot(delta,delta),1e-4);
            direction=delta*rsqrt(distanceSquared);
            float range=max(light.parameters.x,0.001);
            float rangeWeight=saturate(1-pow(sqrt(distanceSquared)/range,4));
            attenuation=rangeWeight*rangeWeight/distanceSquared;
            if (light.position.w>1.5) {
                float outer=cos(light.direction.w*SHINKOU_PI*0.5);
                float inner=cos(light.parameters.y*SHINKOU_PI*0.5);
                attenuation*=smoothstep(outer,max(inner,outer+1e-4),dot(-direction,normalize(light.direction.xyz)));
            }
        }
        attenuation*=shadow_visibility(index,input.worldPosition,normal,normalize(direction));
        color+=shinkou_pbr_direct(albedo,metallic,roughness,normal,viewDirection,normalize(direction),
            light.colorIntensity.rgb*light.colorIntensity.w*attenuation);
    }
    float ao=lerp(1,Occlusion.Sample(MaterialSampler,input.uv).r,surface.z);
    if (ibl.w>0.5) {
        float nv=saturate(dot(normal,viewDirection));
        float3 f0=lerp(0.04.xxx,albedo,metallic);
        float3 fresnel=f0+(max((1-roughness).xxx,f0)-f0)*pow(1-nv,5);
        float3 diffuse=Irradiance.SampleLevel(MaterialSampler,environment_uv(normal),0).rgb*albedo;
        float3 specular=PrefilteredEnvironment.SampleLevel(MaterialSampler,environment_uv(reflect(-viewDirection,normal)),roughness*ibl.z).rgb;
        float2 brdf=BrdfLut.SampleLevel(MaterialSampler,float2(nv,roughness),0).rg;
        color+=((1-fresnel)*(1-metallic)*diffuse+specular*(fresnel*brdf.x+brdf.y))*ibl.x*ao;
    } else color+=albedo*0.03*(1-metallic)*ao;
    color+=srgb_to_linear(Emissive.Sample(MaterialSampler,input.uv).rgb)*emissiveMetallic.rgb;
    return float4(max(color,0),alpha);
}
#endif
