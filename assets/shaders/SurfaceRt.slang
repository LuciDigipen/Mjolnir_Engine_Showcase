// Ray-traced surface pass. The hit group is one material type, so the instance
// SBT offset selects <name>.closestHitSurface.spv; legacy instances stay on
// raytrace.slang's closestHitMain.
#define MIR_SURFACE_SET 0
#define MIR_SURFACE_IMAGES_BINDING 9
#define MIR_SURFACE_SAMPLERS_BINDING 10
#define MIR_SURFACE_RECORDS_BINDING 11

#include "PbrCommon.slang"
#include "SurfaceCommon.slang"

[[vk::binding(0, 0)]] RaytracingAccelerationStructure gSurfaceTlas;

[[vk::binding(2, 0)]] StructuredBuffer<RtInstanceInfo> gInstances;

[[vk::binding(5, 0)]] RWTexture2D<float4> gAlbedo;
[[vk::binding(6, 0)]] RWTexture2D<float4> gNormal;
[[vk::binding(7, 0)]] RWTexture2D<float4> gSpecularAlbedo;

struct RtInstanceInfo
{
    float *vertexAddress;
    uint *indexAddress;
    float4 tint;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    uint flags;
    uint baseColorTexture;
    uint baseColorSampler;
    uint materialRecord;
    float alphaCutoff;
    float pad0;
    float pad1;
    float pad2;
    float4x4 prevModel;
    float4 modelCol0;
    float4 modelCol1;
    float4 modelCol2;
};

struct RayPayload
{
    float3 color;
    float distance;
    float hardwareDepth;
    float2 velocity;
};

struct ShadowPayload
{
    float visibility;
};

struct RtPushConstants
{
    uint2 outputOffset;
    uint aaMode;
    uint sampleCount;
};

[[vk::push_constant]] RtPushConstants pushData;

static const uint kInstanceRasterFacingCorrection = 32u;
static const uint kAaModeDlss = 3u;
static const uint kVertexStrideFloats = 16;
static const uint kColorOffset = 3;
static const uint kNormalOffset = 6;
static const uint kUvOffset = 9;
static const uint kAlphaOffset = 11;
static const uint kTangentOffset = 12;
static const float kShadowNormalBias = 0.01;
static const float kShadowNormalBiasPerDistance = 1e-4;
static const float kSunShadowDistance = 10000.0;

struct SurfaceRayVisibility : IShadowVisibility
{
    float3 origin;

    float Sun(float3 L)
    {
        ShadowPayload shadowPayload;
        shadowPayload.visibility = 0.0;
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = L;
        ray.TMin = 0.001;
        ray.TMax = kSunShadowDistance;
        TraceRay(gSurfaceTlas, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
            0xFF, 1, 0, 1, ray, shadowPayload);
        return shadowPayload.visibility;
    }

    float Local(GpuLight light, float3 Lp, float dist)
    {
        if (light.color.w == kAreaLightSlot)
            return 1.0;
        ShadowPayload shadowPayload;
        shadowPayload.visibility = 0.0;
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = Lp;
        ray.TMin = 0.001;
        ray.TMax = max(dist, ray.TMin);
        TraceRay(gSurfaceTlas, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
            0xFF, 1, 0, 1, ray, shadowPayload);
        return shadowPayload.visibility;
    }
};

uint3 TriangleIndices(RtInstanceInfo info)
{
    const uint primBase = PrimitiveIndex() * 3;
    return uint3(info.indexAddress[primBase + 0], info.indexAddress[primBase + 1], info.indexAddress[primBase + 2]);
}

float3 Barycentrics(BuiltInTriangleIntersectionAttributes attribs)
{
    return float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
}

float4 LoadFloat4(RtInstanceInfo info, uint vertex, uint offset)
{
    const uint b = vertex * kVertexStrideFloats + offset;
    return float4(info.vertexAddress[b], info.vertexAddress[b + 1], info.vertexAddress[b + 2],
        info.vertexAddress[b + 3]);
}

float3 InterpolateFloat3(RtInstanceInfo info, uint3 indices, float3 barycentrics, uint offset)
{
    return LoadFloat4(info, indices.x, offset).xyz * barycentrics.x +
           LoadFloat4(info, indices.y, offset).xyz * barycentrics.y +
           LoadFloat4(info, indices.z, offset).xyz * barycentrics.z;
}

float InterpolateFloat(RtInstanceInfo info, uint3 indices, float3 barycentrics, uint offset)
{
    return info.vertexAddress[indices.x * kVertexStrideFloats + offset] * barycentrics.x +
           info.vertexAddress[indices.y * kVertexStrideFloats + offset] * barycentrics.y +
           info.vertexAddress[indices.z * kVertexStrideFloats + offset] * barycentrics.z;
}

float4 InterpolateTangent(RtInstanceInfo info, uint3 indices, float3 barycentrics)
{
    float4 tangent = float4(0.0, 0.0, 0.0, 0.0);
    for (uint k = 0; k < 3; k++)
    {
        const uint b = indices[k] * kVertexStrideFloats + kTangentOffset;
        tangent += float4(info.vertexAddress[b], info.vertexAddress[b + 1], info.vertexAddress[b + 2],
                        info.vertexAddress[b + 3]) *
                   barycentrics[k];
    }
    return tangent;
}

float3 FaceNormal(RtInstanceInfo info, float3 op0, float3 op1, float3 op2)
{
    const float3 n = cross(op1 - op0, op2 - op0);
    const float3 worldNormal =
        normalize(info.normalCol0.xyz * n.x + info.normalCol1.xyz * n.y + info.normalCol2.xyz * n.z);
    return dot(worldNormal, WorldRayDirection()) > 0.0 ? -worldNormal : worldNormal;
}

[shader("closesthit")]
void closestHitSurface<M : IMaterial>(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const RtInstanceInfo info = gInstances[InstanceID()];
    const float3 barycentrics = Barycentrics(attribs);
    const uint3 indices = TriangleIndices(info);

    const float3 op0 = LoadFloat4(info, indices.x, 0).xyz;
    const float3 op1 = LoadFloat4(info, indices.y, 0).xyz;
    const float3 op2 = LoadFloat4(info, indices.z, 0).xyz;
    const float3 objectPos = op0 * barycentrics.x + op1 * barycentrics.y + op2 * barycentrics.z;
    const float3 hitWorldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    const float3 objNormal = InterpolateFloat3(info, indices, barycentrics, kNormalOffset);
    float3 N = normalize(info.normalCol0.xyz * objNormal.x + info.normalCol1.xyz * objNormal.y +
                         info.normalCol2.xyz * objNormal.z);

    // HitKind carries the instance's facing flip; the per-instance correction
    // makes it match the raster frontFace/mirror convention. Materials orient
    // their normal from frontFace if they need a back-face response.
    bool frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE;
    if ((info.flags & kInstanceRasterFacingCorrection) != 0)
        frontFace = !frontFace;

    const float4 sourceTangent = InterpolateTangent(info, indices, barycentrics);
    const float4 tangentFrame = TransformTangentFrame(info.modelCol0.xyz, info.modelCol1.xyz,
        info.modelCol2.xyz, sourceTangent, N);
    const float4 vertexColor = float4(InterpolateFloat3(info, indices, barycentrics, kColorOffset),
                                    InterpolateFloat(info, indices, barycentrics, kAlphaOffset)) *
                               info.tint;
    const float2 uv = float2(InterpolateFloat(info, indices, barycentrics, kUvOffset),
        InterpolateFloat(info, indices, barycentrics, kUvOffset + 1));

    MaterialContext context;
    context.rayTraced = true;
    context.uvDx = float2(0.0, 0.0);
    context.uvDy = float2(0.0, 0.0);

    const M material = gSurfaceRecords.Load<M>(info.materialRecord);
    const SurfaceInput surfaceInput =
        MeshFrontend::Build(hitWorldPos, N, tangentFrame, uv, objectPos, vertexColor, frontFace);
    const SurfaceOutput surface = material.Evaluate(surfaceInput, context);

    SurfaceRayVisibility visibility;
    visibility.origin = hitWorldPos + FaceNormal(info, op0, op1, op2) *
                                          max(kShadowNormalBias, length(hitWorldPos) * kShadowNormalBiasPerDistance);

    const float3 lit = ShadePbr(visibility, surface, hitWorldPos);
    payload.color = ApplyFog(lit, RayTCurrent());
    payload.distance = RayTCurrent();

    const float4 currClip = mul(viewProj, float4(hitWorldPos, 1.0));
    payload.hardwareDepth = saturate(currClip.z / currClip.w);
    const float4 prevClip = mul(prevViewProj, mul(info.prevModel, float4(objectPos, 1.0)));
    payload.velocity = (currClip.xy / currClip.w) - (prevClip.xy / prevClip.w);

    if (pushData.aaMode == kAaModeDlss)
    {
        const uint2 guidePixel = DispatchRaysIndex().xy + pushData.outputOffset;
        const float3 viewDir = normalize(cameraPos.xyz - hitWorldPos);
        const float NoV = saturate(dot(normalize(surface.normalWS), viewDir));
        gAlbedo[guidePixel] = float4(surface.albedo, 1.0);
        gSpecularAlbedo[guidePixel] =
            float4(SpecularAlbedoGuide(surface.f0.xxx, surface.roughness * surface.roughness, NoV), 1.0);
        gNormal[guidePixel] = float4(surface.normalWS, surface.roughness);
    }
}
