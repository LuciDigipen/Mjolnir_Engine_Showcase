// Raster surface pass: one vertex shader for MeshFrontend and a material-
// specialized fragment entry. Compile:
//   slangc SurfaceForward.slang <material>.slang -entry fragmentMain -stage fragment
//          -specialize <Type> -fvk-use-gl-layout ...
#include "ShadowMapCommon.slang"
#include "PbrCommon.slang"
#include "SurfaceCommon.slang"

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

[shader("vertex")]
SurfaceVSOutput vertexMain(SurfaceVSInput input)
{
    SurfaceVSOutput output;
    const float4 worldPos = mul(pushData.model, float4(input.pos, 1.0));
    output.position = mul(viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    output.normal = normalize(pushData.normalCol0.xyz * input.normal.x +
                              pushData.normalCol1.xyz * input.normal.y +
                              pushData.normalCol2.xyz * input.normal.z);
    const float3x3 linear = (float3x3)pushData.model;
    float3 tangent = mul(linear, input.tangent.xyz);
    tangent -= output.normal * dot(output.normal, tangent);
    if (dot(tangent, tangent) < 1e-12)
        tangent = cross(output.normal, abs(output.normal.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0));
    tangent = normalize(tangent);
    float handedness = input.tangent.w < 0.0 ? -1.0 : 1.0;
    if (determinant(linear) < 0.0)
        handedness = -handedness;
    output.tangent = float4(tangent, handedness);
    output.uv = input.uv;
    output.color = float4(input.color, input.alpha) * pushData.tint;
    output.localPos = input.pos;
    return output;
}

[shader("fragment")]
float4 fragmentMain<M : IMaterial>(SurfaceVSOutput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    const M material = gSurfaceRecords.Load<M>(gMaterialRecordRef.offset);

    MaterialContext context;
    context.rayTraced = false;
    context.uvDx = ddx(input.uv);
    context.uvDy = ddy(input.uv);

    const SurfaceInput surfaceInput = MeshFrontend::Build(input.worldPos, normalize(input.normal),
        input.tangent, input.uv, input.localPos, input.color, frontFace);
    const SurfaceOutput surface = material.Evaluate(surfaceInput, context);

    ShadowMapVisibility visibility;
    visibility.worldPos = input.worldPos;
    visibility.geoN = GeometricNormal(input.worldPos, surface.normalWS);

    const float3 color = ShadePbr(visibility, surface, input.worldPos);
    return float4(ApplyFog(color, length(cameraPos.xyz - input.worldPos)), 1.0);
}
