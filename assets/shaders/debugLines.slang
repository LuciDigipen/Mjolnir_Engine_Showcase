// Editor debug views: colored line segments, camera-relative.
struct VSInput
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
};

struct DebugLinesPushConstants
{
    float4x4 viewProj;
}

[[vk::push_constant]]
DebugLinesPushConstants pushData;

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(pushData.viewProj, float4(input.pos, 1.0));
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    return input.color;
}
