// Editor debug views: a mesh's triangles drawn as flat-colored lines.
struct VSInput
{
    float3 pos : POSITION;
};

struct DebugWireframePushConstants
{
    float4x4 viewProjModel;
    float4 color;
}

[[vk::push_constant]]
DebugWireframePushConstants pushData;

[shader("vertex")]
float4 vertexMain(VSInput input) : SV_Position
{
    return mul(pushData.viewProjModel, float4(input.pos, 1.0));
}

[shader("fragment")]
float4 fragmentMain() : SV_Target
{
    return pushData.color;
}
