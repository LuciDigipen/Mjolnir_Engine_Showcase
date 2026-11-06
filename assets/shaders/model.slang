#include "ShadowMapCommon.slang"
#include "SurfaceCommon.slang"
#include "PbrCommon.slang"
#ifdef MIR_SSGI_SURFACE
#include "RasterSurfaceCommon.slang"
#endif

struct VSInput
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float alpha : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
};
struct VSOutput
{
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
    float3 normal : NORMAL;
    float pointSize : PSIZE;
};
struct RasterPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
};
[[vk::push_constant]] RasterPushConstants pushData;

// set 0, binding 0: base color, combined with this material's sampler.
[[vk::binding(0, 0)]] Sampler2D<float4> baseColor;

// set 2, binding 0: this draw's morph/skin data. Slot 2 is the material
// channel the renderer writes in; see vk::ModelDescriptors::Bind.
[[vk::binding(0, 2)]] StructuredBuffer<float4> deformation;

#include "ModelDeformation.slang"

[shader("vertex")] VSOutput vertexMain(VSInput input, uint vertex : SV_VertexID) {
    float3 p = input.pos, n = input.normal, t = input.tangent.xyz;
    DeformModelVertex(vertex, p, n, t);
    VSOutput output;
    const float4 worldPos = mul(pushData.model, float4(p, 1));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.color = float4(input.color, input.alpha) * pushData.tint;
    output.uv = input.uv;
    output.normal = pushData.normalCol0.xyz * n.x + pushData.normalCol1.xyz * n.y + pushData.normalCol2.xyz * n.z;
    output.pointSize = 1;
    return output;
}
[shader("fragment")]
#ifdef MIR_SSGI_SURFACE
RasterSurfaceOutput fragmentMain(VSOutput input, bool front : SV_IsFrontFace)
#else
float4 fragmentMain(VSOutput input, bool front : SV_IsFrontFace) : SV_Target
#endif
{
    float4 color = baseColor.Sample(input.uv) * input.color;
    float4 material = deformation[2];
    if (material.x == 1 && color.a < material.y)
        discard;
    if (material.x != 2)
        color.a = 1;
    float3 normal = normalize(input.normal);
    if (material.z > 0 && !front)
        normal = -normal;
    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, normal);
#ifdef MIR_SSGI_SURFACE
    float3 diffuse;
    float3 direct = ShadeLitAndDiffuse(visibility, color.rgb, normal, input.worldPos, float3(0), diffuse);
    float3 lit = direct + color.rgb * ambientColor.rgb;
    return MakeRasterSurface(float4(ApplyFog(lit, length(cameraPos.xyz - input.worldPos)), color.a),
        normal, 1, color.rgb, diffuse, direct);
#else
    if (material.x == 2)
    {
        const float3 lit = ShadePbr(visibility, BlendedModelSurface(color.rgb, normal), input.worldPos);
        return float4(ApplyFog(lit, length(cameraPos.xyz - input.worldPos)), color.a);
    }
    const float3 lit = ShadeLit(visibility, color.rgb, normal, input.worldPos);
    return float4(ApplyFog(lit, length(cameraPos.xyz - input.worldPos)), color.a);
#endif
}
