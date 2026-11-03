// Error material shader: solid magenta, ignoring the draw's tint and textures.
// color and normal are unused, but BuildPipeline declares them for every non-model shader.
#ifdef MIR_SSGI_SURFACE
#include "RasterSurfaceCommon.slang"
#endif
struct VSInput
{
    float3 pos : POSITION;
    float3 color : COLOR;
    float3 normal : NORMAL;
};

struct VSOutput
{
    float4 position : SV_Position;
#ifdef MIR_SSGI_SURFACE
    float3 worldPos : TEXCOORD0;
#endif
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

// set 1, binding 0: only viewProj is read, and it leads the camera UBO.
[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = mul(viewProj, mul(pushData.model, float4(input.pos, 1.0)));
#ifdef MIR_SSGI_SURFACE
    output.worldPos = mul(pushData.model, float4(input.pos, 1.0)).xyz;
#endif
    return output;
}

[shader("fragment")]
#ifdef MIR_SSGI_SURFACE
RasterSurfaceOutput fragmentMain(VSOutput input)
{
    return MakeRasterSurface(float4(1.0, 0.0, 1.0, 1.0), RasterSurfaceFaceNormal(input.worldPos), 3,
        float3(0), float3(0), float3(1, 0, 1));
}
#else
float4 fragmentMain(VSOutput input) : SV_Target
{
    return float4(1.0, 0.0, 1.0, 1.0);
}
#endif
