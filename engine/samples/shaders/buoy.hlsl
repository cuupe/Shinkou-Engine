struct BuoyVertexOutput {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : NORMAL;
};

cbuffer BuoyFrame : register(b2) {
    float4x4 viewProjection;
    float4 sunDirectionIntensity;
    float4 cameraPosition;
};

cbuffer BuoyObject : register(b3) {
    float4x4 model;
};

BuoyVertexOutput VSMain(float3 position : POSITION, float3 normal : NORMAL) {
    BuoyVertexOutput output;
    const float4 world = mul(model, float4(position, 1.0));
    output.position = mul(viewProjection, world);
    output.worldPosition = world.xyz;
    output.normal = normalize(mul(model, float4(normal, 0.0)).xyz);
    return output;
}

float4 PSMain(BuoyVertexOutput input) : SV_Target {
    const float y = input.worldPosition.y;
    float3 albedo;
    if (y > 2.05) albedo = float3(0.86, 0.015, 0.008);
    else if (y > 1.62) albedo = float3(1.0, 0.75, 0.03);
    else if (y > 1.38) albedo = float3(0.015, 0.02, 0.018);
    else if (y > 0.35) albedo = float3(0.82, 0.84, 0.80);
    else albedo = float3(0.02, 0.025, 0.022);

    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(sunDirectionIntensity.xyz);
    const float3 viewDirection = normalize(cameraPosition.xyz - input.worldPosition);
    const float3 halfVector = normalize(lightDirection + viewDirection);
    const float diffuse = 0.25 + 0.75 * saturate(dot(normal, lightDirection));
    const float specular = pow(saturate(dot(normal, halfVector)), 64.0) *
        sunDirectionIntensity.w;
    const float rim = pow(1.0 - saturate(dot(normal, viewDirection)), 3.0) * 0.16;
    float3 color = albedo * diffuse + float3(1.0, 0.86, 0.65) * specular + rim;
    color = color / (1.0 + color);
    return float4(pow(max(color, 0.0), 1.0 / 2.2), 1.0);
}
