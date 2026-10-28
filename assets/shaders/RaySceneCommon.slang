// RT geometry, materials and shadow visibility shared by camera and probe pipelines.
#include "AtmosphereCommon.slang"
#include "LightingCommon.slang"

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure gTlas;

struct RtInstanceInfo
{
    float *vertexAddress;
    uint *indexAddress;
    float4 tint;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    uint flags;            // kInstance* bits
    uint baseColorTexture; // index into gTextures; read only with kInstanceTextured
    float alphaCutoff;     // read only with kInstanceAlphaTest
    uint pad0;
    float4x4 prevModel; // camera-relative to THIS frame's camera - see DrawItem::previousModel
};

[[vk::binding(2, 0)]]
StructuredBuffer<RtInstanceInfo> gInstances;

// RtInstanceInfo::flags; keep in sync with RayTracer.cpp.
static const uint kInstanceUnlit = 1u;
static const uint kInstanceTextured = 2u; // the model shader: base color = texture * vertex color * tint
static const uint kInstanceError = 4u;
static const uint kInstanceAlphaTest = 8u;
static const uint kInstanceDoubleSided = 16u;
static const uint kMaxRtTextures = 1024;

[[vk::binding(9, 0)]]
Sampler2D<float4> gTextures[kMaxRtTextures];

// pos(3) + color(3) + normal(3) + uv(2) + alpha(1) + tangent(4); keep in sync with mir::Vertex.
static const uint kVertexStrideFloats = 16;
static const uint kColorOffset = 3;
static const uint kNormalOffset = 6;
static const uint kUvOffset = 9;
static const uint kAlphaOffset = 11;

uint3 TriangleIndices(RtInstanceInfo info)
{
    const uint primBase = PrimitiveIndex() * 3;
    return uint3(info.indexAddress[primBase + 0], info.indexAddress[primBase + 1], info.indexAddress[primBase + 2]);
}

float3 Barycentrics(BuiltInTriangleIntersectionAttributes attribs)
{
    return float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y, attribs.barycentrics.x, attribs.barycentrics.y);
}

float3 LoadFloat3(RtInstanceInfo info, uint vertex, uint offset)
{
    const uint b = vertex * kVertexStrideFloats + offset;
    return float3(info.vertexAddress[b], info.vertexAddress[b + 1], info.vertexAddress[b + 2]);
}

float3 InterpolateFloat3(RtInstanceInfo info, uint3 indices, float3 barycentrics, uint offset)
{
    return LoadFloat3(info, indices.x, offset) * barycentrics.x + LoadFloat3(info, indices.y, offset) * barycentrics.y +
           LoadFloat3(info, indices.z, offset) * barycentrics.z;
}

// xy = uv, z = vertex alpha.
float3 InterpolateUvAlpha(RtInstanceInfo info, uint3 indices, float3 barycentrics)
{
    float3 result = float3(0.0, 0.0, 0.0);
    for (uint k = 0; k < 3; k++)
    {
        const uint b = indices[k] * kVertexStrideFloats;
        result += float3(info.vertexAddress[b + kUvOffset], info.vertexAddress[b + kUvOffset + 1],
                         info.vertexAddress[b + kAlphaOffset]) *
                  barycentrics[k];
    }
    return result;
}

// model.slang's base color: texture * vertex color and alpha * tint.
float4 ModelBaseColor(RtInstanceInfo info, uint3 indices, float3 barycentrics)
{
    const float3 uvAlpha = InterpolateUvAlpha(info, indices, barycentrics);
    const float3 vertexColor = InterpolateFloat3(info, indices, barycentrics, kColorOffset);
    // Level 0: there are no ray differentials yet to pick a mip from.
    const float4 texel = gTextures[NonUniformResourceIndex(info.baseColorTexture)].SampleLevel(uvAlpha.xy, 0.0);
    return texel * float4(vertexColor, uvAlpha.z) * info.tint;
}

// model.slang discards below the cutoff. Blend materials are cut the same way, since a trace has no blending.
bool AlphaTestPasses(BuiltInTriangleIntersectionAttributes attribs)
{
    const RtInstanceInfo info = gInstances[InstanceID()];
    if ((info.flags & kInstanceAlphaTest) == 0)
        return true;
    return ModelBaseColor(info, TriangleIndices(info), Barycentrics(attribs)).a >= info.alphaCutoff;
}

struct ShadowPayload { float visibility; };

// Shadow ray origin offset; grows with distance, where floats get coarser.
static const float kShadowNormalBias = 0.01;
static const float kShadowNormalBiasPerDistance = 1e-4;
static const float kSunShadowDistance = 10000.0;

float TraceShadowRay(float3 origin, float3 direction, float tMax)
{
    ShadowPayload shadowPayload;
    shadowPayload.visibility = 0.0;

    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.001;
    shadowRay.TMax = max(tMax, shadowRay.TMin);

    TraceRay(gTlas, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0xFF, 1, 0, 1, shadowRay, shadowPayload);

    return shadowPayload.visibility;
}

// Traces every Point/Spot, not only those with a raster shadow slot.
struct RayVisibility : IShadowVisibility
{
    float3 origin; // biased off the surface

    float Sun(float3 L)
    {
        return TraceShadowRay(origin, L, kSunShadowDistance);
    }

    float Local(GpuLight light, float3 Lp, float dist)
    {
        if (light.color.w == kAreaLightSlot)
            return 1.0;
        return TraceShadowRay(origin, Lp, dist);
    }
}

float3 FaceNormal(RtInstanceInfo info, float3 op0, float3 op1, float3 op2)
{
    const float3 n = cross(op1 - op0, op2 - op0);
    const float3 worldNormal =
        normalize(info.normalCol0.xyz * n.x + info.normalCol1.xyz * n.y + info.normalCol2.xyz * n.z);
    return dot(worldNormal, WorldRayDirection()) > 0.0 ? -worldNormal : worldNormal;
}

