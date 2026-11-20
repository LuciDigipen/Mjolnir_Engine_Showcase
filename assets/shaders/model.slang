#include "ShadowMapCommon.slang"
#include "SurfaceCommon.slang"
#include "PbrCommon.slang"
#include "ModelMaterial.slang"
[[vk::binding(MIR_DRAW_RECORDS_BINDING, MIR_DRAW_RECORDS_SET)]]
StructuredBuffer<DrawRecord> gDrawRecords;
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
    float4 tangent : TANGENT;
    float pointSize : PSIZE;
    nointerpolation uint baseInstance : TEXCOORD3;
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

[shader("vertex")] VSOutput vertexMain(VSInput input, uint vertex : SV_VertexID,
                                       uint baseInstance : SV_StartInstanceLocation) {
    float3 p = input.pos, n = input.normal, t = input.tangent.xyz;
    float sign = input.tangent.w;
    DeformModelVertex(vertex, p, n, t, sign);
    VSOutput output;
    const float4 worldPos = mul(pushData.model, float4(p, 1));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.color = float4(input.color, input.alpha) * pushData.tint;
    output.uv = input.uv;
    output.normal = pushData.normalCol0.xyz * n.x + pushData.normalCol1.xyz * n.y + pushData.normalCol2.xyz * n.z;
    output.tangent =
        TransformTangentFrame(mul(pushData.model, float4(1, 0, 0, 0)).xyz, mul(pushData.model, float4(0, 1, 0, 0)).xyz,
                              mul(pushData.model, float4(0, 0, 1, 0)).xyz, float4(t, sign), output.normal);
    output.pointSize = 1;
    output.baseInstance = baseInstance;
    return output;
}[shader("fragment")]
#ifdef MIR_SSGI_SURFACE
    RasterSurfaceOutput fragmentMain(VSOutput input, bool front : SV_IsFrontFace)
#else
    float4 fragmentMain(VSOutput input, bool front : SV_IsFrontFace)
    : SV_Target
#endif
{
    float4 color = baseColor.Sample(input.uv) * input.color;
    float4 material = deformation[2];
    if (material.x == 1 && color.a < material.y)
        discard;
    if (material.x != 2)
        color.a = 1;
    MaterialContext context;
    context.rayTraced = false;
    context.uvDx = ddx(input.uv);
    context.uvDy = ddy(input.uv);
    context.footprintMeters = 0;
    context.incidence = 1;
    context.uvDensity = 0;
    const SurfaceOutput surface = EvaluateModelMaterial(gDrawRecords[input.baseInstance].materialOffset, color, input.normal, input.tangent,
                                                        input.uv, material.z > 0 && !front, context);
    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, normalize(input.normal));
    const PbrLighting lighting = EvaluatePbrLighting(visibility, surface, input.worldPos);
    const float3 lit = ApplyFog(PbrTotalLighting(lighting), length(cameraPos.xyz - input.worldPos));
#ifdef MIR_SSGI_SURFACE
    return MakeRasterSurface(float4(lit, color.a), surface.normalWS, surface.unlit ? 2 : 1,
                             surface.albedo * surface.ao * (1 - surface.metallic), PbrDiffuseRadiance(lighting),
                             PbrDirectLighting(lighting), 1 - max(surface.f0.x, max(surface.f0.y, surface.f0.z)));
#else
    return float4(lit, color.a);
#endif
}
