/**
 * @file SurfaceTerrainForward.slang
 * @author Jackey Cen
 * @brief Raster frontend for terrain surface materials.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
// Terrain raster frontend. A terrain mesh has no authored tangent, so the
// macro normal from the rasterized surface builds the tangent frame.
#include "ShadowMapCommon.slang"
#include "PbrCommon.slang"
#include "SurfaceCommon.slang"

struct TerrainVSInput
{
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 color : COLOR;
    [[vk::location(2)]] float3 normal : NORMAL;
    [[vk::location(3)]] float2 uv : TEXCOORD0;
    [[vk::location(4)]] float alpha : TEXCOORD1;
    [[vk::location(5)]] float4 tangent : TANGENT;
};

struct TerrainVSOutput
{
    float4 position : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD1;
    float4 color : COLOR;
    float3 localPos : TEXCOORD2;
};

struct TerrainPushConstants
{
    float4x4 model;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    float4 tint;
};

[[vk::push_constant]] TerrainPushConstants pushData;

[[vk::binding(2, 0)]] ConstantBuffer<MaterialRecordRef> gMaterialRecordRef;

[shader("vertex")]
TerrainVSOutput vertexMain(TerrainVSInput input)
{
    TerrainVSOutput output;
    const float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.normal = normalize(pushData.normalCol0.xyz * input.normal.x +
                              pushData.normalCol1.xyz * input.normal.y +
                              pushData.normalCol2.xyz * input.normal.z);
    output.uv = input.uv;
    output.color = float4(input.color, input.alpha);
    output.localPos = input.pos;
    return output;
}

[shader("fragment")]
float4 fragmentMain<M : IMaterial>(TerrainVSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    const M material = gSurfaceRecords.Load<M>(gMaterialRecordRef.offset);
    const float3 macroNormal = SafeNormalize(input.normal, float3(0.0, 1.0, 0.0));

    MaterialContext context;
    context.rayTraced = false;
    context.uvDx = ddx(input.uv);
    context.uvDy = ddy(input.uv);
    context.footprintMeters = 0.0;
    context.incidence = 1.0;
    context.uvDensity = 0.0;

    const SurfaceInput surfaceInput = TerrainFrontend::Build(input.worldPos, macroNormal, input.uv,
        input.localPos, input.color, frontFace, 0.0);
    const SurfaceOutput surface = ApplySurfaceTint(material.Evaluate(surfaceInput, context), pushData.tint);

    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, surface.normalWS);

    const float3 color = ShadePbr(visibility, surface, input.worldPos);
    return float4(ApplyFog(color, length(cameraPos.xyz - input.worldPos)), 1.0);
}
