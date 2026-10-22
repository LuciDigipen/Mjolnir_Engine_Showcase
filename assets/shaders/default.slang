// Default material shader: a flat grey, non-metallic Blinn-Phong material
#include "ShadowMapCommon.slang"

struct VSInput
{
    float3 pos : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float3 tint : COLOR;
};

struct RasterPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
}

[[vk::push_constant]]
RasterPushConstants pushData;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.normal = pushData.normalCol0.xyz * input.normal.x +
                    pushData.normalCol1.xyz * input.normal.y +
                    pushData.normalCol2.xyz * input.normal.z;

    output.tint = pushData.tint.rgb;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float3 N = normalize(input.normal);

    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, N);

    float3 color = ShadeLit(visibility, kDefaultAlbedo * input.tint, N, input.worldPos);
    color = ApplyFog(color, length(cameraPos.xyz - input.worldPos));

    return float4(color, 1.0);
}
