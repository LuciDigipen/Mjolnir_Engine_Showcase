// Raster surface pass: one vertex shader for MeshFrontend and a material-
// specialized fragment entry. Compile:
//   slangc SurfaceForward.slang <material>.slang -entry fragmentMain -stage fragment
//          -specialize <Type> -fvk-use-gl-layout ...
#include "ShadowMapCommon.slang"
#include "PbrCommon.slang"
#include "SurfaceCommon.slang"
#ifdef MIR_SSGI_SURFACE
#include "RasterSurfaceCommon.slang"
#endif

struct SurfaceVSInput
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float alpha : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
};

struct SurfaceVSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD1;
    float4 color : COLOR;
    float3 localPos : TEXCOORD2;
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

[[vk::binding(2, 0)]] ConstantBuffer<MaterialRecordRef> gMaterialRecordRef;

[shader("vertex")] SurfaceVSOutput vertexMain(SurfaceVSInput input) {
    SurfaceVSOutput output;
    const float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.normal = normalize(pushData.normalCol0.xyz * input.normal.x + pushData.normalCol1.xyz * input.normal.y +
                              pushData.normalCol2.xyz * input.normal.z);
    const float3 c0 = mul(pushData.model, float4(1, 0, 0, 0)).xyz;
    const float3 c1 = mul(pushData.model, float4(0, 1, 0, 0)).xyz;
    const float3 c2 = mul(pushData.model, float4(0, 0, 1, 0)).xyz;
    output.tangent = TransformTangentFrame(c0, c1, c2, input.tangent, output.normal);
    output.uv = input.uv;
    output.color = float4(input.color, input.alpha);
    output.localPos = input.pos;
    return output;
}

    [shader("fragment")]
#ifdef MIR_SSGI_SURFACE
    RasterSurfaceOutput fragmentMain<M : IMaterial>(SurfaceVSOutput input, bool frontFace : SV_IsFrontFace)
#else
    float4 fragmentMain<M : IMaterial>(SurfaceVSOutput input, bool frontFace : SV_IsFrontFace)
    : SV_Target
#endif
{
    const M material = gSurfaceRecords.Load<M>(gMaterialRecordRef.offset);

    MaterialContext context;
    context.rayTraced = false;
    context.uvDx = ddx(input.uv);
    context.uvDy = ddy(input.uv);
    context.footprintMeters = 0.0;
    context.incidence = 1.0;
    context.uvDensity = 0.0;

    const SurfaceInput surfaceInput = MeshFrontend::Build(input.worldPos, normalize(input.normal), input.tangent,
                                                          input.uv, input.localPos, input.color, frontFace);
    const SurfaceOutput surface = ApplySurfaceTint(material.Evaluate(surfaceInput, context), pushData.tint);

    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, surface.normalWS);

#ifdef MIR_SSGI_SURFACE
    const PbrLighting lighting = EvaluatePbrLighting(visibility, surface, input.worldPos);
    const float3 color = ApplyFog(PbrTotalLighting(lighting), length(cameraPos.xyz - input.worldPos));
    return MakeRasterSurface(float4(color, 1.0), normalize(surface.normalWS), 1,
                             surface.albedo * surface.ao * (1.0 - surface.metallic), PbrDiffuseRadiance(lighting),
                             PbrDirectLighting(lighting), 1.0 - max(surface.f0.x, max(surface.f0.y, surface.f0.z)));
#else
    const float3 color = ShadePbr(visibility, surface, input.worldPos);
    return float4(ApplyFog(color, length(cameraPos.xyz - input.worldPos)), 1.0);
#endif
}
