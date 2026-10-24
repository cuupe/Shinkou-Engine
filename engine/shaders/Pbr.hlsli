static const float SHINKOU_PI = 3.14159265359;

float3 shinkou_fresnel(float cosTheta, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}
float3 shinkou_pbr_direct(float3 albedo, float metallic, float roughness,
    float3 normal, float3 viewDirection, float3 lightDirection, float3 radiance) {
    float3 halfway = normalize(viewDirection + lightDirection + 1e-8);
    float nv = max(saturate(dot(normal,viewDirection)),1e-4);
    float nl = saturate(dot(normal,lightDirection));
    float nh = saturate(dot(normal,halfway));
    float vh = saturate(dot(viewDirection,halfway));
    float alpha = max(roughness*roughness,0.0025);
    float alpha2 = alpha*alpha;
    float denominator = nh*nh*(alpha2-1.0)+1.0;
    float distribution = alpha2 / max(SHINKOU_PI*denominator*denominator,1e-7);
    // Height-correlated Smith visibility for GGX.
    float gv = nl*sqrt(nv*nv*(1.0-alpha2)+alpha2);
    float gl = nv*sqrt(nl*nl*(1.0-alpha2)+alpha2);
    float visibility = 0.5 / max(gv+gl,1e-5);
    float3 fresnel = shinkou_fresnel(vh,lerp(0.04.xxx,albedo,metallic));
    float3 diffuse = (1.0-fresnel)*(1.0-metallic)*albedo/SHINKOU_PI;
    return (diffuse+distribution*visibility*fresnel)*radiance*nl;
}

float3 shinkou_tone_map(float3 linearColor) {
    linearColor = max(linearColor,0.0);
    return saturate((linearColor*(2.51*linearColor+0.03))/(linearColor*(2.43*linearColor+0.59)+0.14));
}

float3 shinkou_linear_to_srgb(float3 value) {
    value = max(value,0.0);
    return lerp(value*12.92,1.055*pow(value,1.0/2.4)-0.055,step(0.0031308.xxx,value));
}
