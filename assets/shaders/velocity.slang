struct VSInput
{
    float3 pos : POSITION;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 currClip : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
};

struct VelocityPushConstants
{
    float4x4 model;
    float4x4 prevModel;
}

[[vk::push_constant]]
VelocityPushConstants pushData;

[[vk::binding(0, 0)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos;
    float4 fogColor;
    float4 fogRange;
    float4 cameraWorldPos;
    float4 clipPlanes;
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta;
    float4x4 prevViewProj;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    const float4 currWorld = mul(pushData.model, float4(input.pos, 1.0));
    const float4 prevWorld = mul(pushData.prevModel, float4(input.pos, 1.0));
    output.currClip = mul(viewProj, currWorld);
    output.prevClip = mul(prevViewProj, prevWorld);
    output.position = output.currClip;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    const float2 currNdc = input.currClip.xy / input.currClip.w;
    const float2 prevNdc = input.prevClip.xy / input.prevClip.w;
    return float4(currNdc - prevNdc, 0.0, 1.0);
}
