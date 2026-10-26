#include "AtmosphereCommon.slang"
#include "LightingCommon.slang"

[[vk::binding(0, 0)]]
RaytracingAccelerationStructure gTlas;

[[vk::binding(1, 0)]]
RWTexture2D<float4> gOutput;

[[vk::binding(3, 0)]]
RWTexture2D<float> gDistance;

[[vk::binding(4, 0)]]
RWTexture2D<float2> gVelocity;

// Ray Reconstruction's guide buffers
[[vk::binding(5, 0)]]
RWTexture2D<float4> gAlbedo; // rgb = diffuse albedo

[[vk::binding(6, 0)]]
RWTexture2D<float4> gNormal; // xyz = world normal, w = roughness

[[vk::binding(7, 0)]]
RWTexture2D<float4> gSpecularAlbedo;

[[vk::binding(8, 0)]]
RWTexture2D<float> gDlssDepth; // Projected reversed-Z; gDistance remains ray distance for DoF.

struct RtInstanceInfo
{
    float *vertexAddress;
    uint *indexAddress;
    float4 tint;
    float4 normalCol0;
    float4 normalCol1;
    float4 normalCol2;
    uint flags;            // kInstance* bits
    uint baseColorTexture; // index into gImages; read only with kInstanceTextured
    uint baseColorSampler; // index into gSamplers
    uint materialRecord;   // byte offset into gMaterialRecords
    float alphaCutoff;     // read only with kInstanceAlphaTest
    float3 pad0;
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
static const uint kMaxMaterialImages = 1024;
static const uint kMaxMaterialSamplers = 256;

[[vk::binding(9, 0)]]
Texture2D<float4> gImages[kMaxMaterialImages];

[[vk::binding(10, 0)]]
SamplerState gSamplers[kMaxMaterialSamplers];

[[vk::binding(11, 0)]]
ByteAddressBuffer gMaterialRecords;

static const uint kAaModeOff = 0;
static const uint kAaModeMsaa = 2;
static const uint kAaModeDlss = 3;

// D3D's standard 1x/2x/4x/8x sample patterns, 16ths of a pixel from center - the ray-traced
// multi-sample anti-aliasing fallback's spatial supersample offsets. Packed back to back, so
// the pattern for n samples starts at index n - 1.
static const uint kMaxMsaaSamples = 8;
static const float2 kMsaaOffsets[2 * kMaxMsaaSamples - 1] = {
    float2(0.0, 0.0),
    float2(4.0, 4.0), float2(-4.0, -4.0),
    float2(-2.0, -6.0), float2(6.0, -2.0), float2(-6.0, 2.0), float2(2.0, 6.0),
    float2(1.0, -3.0), float2(-1.0, 3.0), float2(5.0, 1.0), float2(-3.0, -5.0),
    float2(-5.0, 5.0), float2(-7.0, -1.0), float2(3.0, 7.0), float2(7.0, -7.0)
};

// The CPU already rounds to a pattern size (ClampRtSampleCount); this keeps a bad value in range.
uint MsaaPatternSize(uint requested)
{
    return requested <= 1u ? 1u : requested <= 2u ? 2u
                              : requested <= 4u   ? 4u
                                                  : kMaxMsaaSamples;
}

struct RtPushConstants
{
    uint2 outputOffset;
    uint aaMode;
    uint sampleCount;
};

[[vk::push_constant]]
RtPushConstants pushData;

struct RayPayload
{
    float3 color;
    float distance;
    float hardwareDepth;
    float2 velocity; // NDC-space, current minus previous - see closestHitMain
};

struct ShadowPayload
{
    float visibility;
};

// No per-material roughness exists yet (MaterialDesc has none) - same fixed-constant
// simplification kShininess already is, standing in for Ray Reconstruction's roughness guide.
static const float kMaterialRoughness = pow(2.0 / (kShininess + 2.0), 0.25);
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
    const uint image = NonUniformResourceIndex(info.baseColorTexture);
    const uint sampler = NonUniformResourceIndex(info.baseColorSampler);
    const float4 texel = gImages[image].SampleLevel(gSamplers[sampler], uvAlpha.xy, 0.0);
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

[shader("raygeneration")]
void rayGenMain()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const uint2 dims = DispatchRaysDimensions().xy;

    const uint sampleCount = pushData.aaMode == kAaModeMsaa ? MsaaPatternSize(pushData.sampleCount) : 1u;

    float3 colorSum = float3(0.0, 0.0, 0.0);
    float centerDistance = 0.0;
    float centerDepth = 0.0;
    float2 centerVelocity = float2(0.0, 0.0);

    for (uint i = 0; i < sampleCount; i++)
    {
        const float2 msaaOffset =
            pushData.aaMode == kAaModeMsaa ? kMsaaOffsets[sampleCount - 1u + i] / 16.0 : float2(0.0, 0.0);
        const float2 pixelOffset = msaaOffset + jitterAndPad.xy;

        const float2 ndc = (float2(pixel) + 0.5 + pixelOffset) / float2(dims) * 2.0 - 1.0;

        const float4 onNearPlane = mul(invViewProj, float4(ndc, 1.0, 1.0));
        const float3 toNearPlane = onNearPlane.xyz / onNearPlane.w - cameraPos.xyz;
        const float3 rayDir = normalize(toNearPlane);

        const float toFarPlane = clipPlanes.y * length(toNearPlane) / clipPlanes.x;

        RayDesc ray;
        ray.Origin = cameraPos.xyz;
        ray.Direction = rayDir;
        ray.TMin = 0.001;
        ray.TMax = clipPlanes.y > 0.0 ? toFarPlane : 1.0e30;

        RayPayload payload;
        payload.color = float3(0.0, 0.0, 0.0);
        payload.distance = clipPlanes.y > 0.0 ? toFarPlane : 1.0e8;
        payload.hardwareDepth = 0.0; // Sky is at infinity with reversed Z.
        const float4 previousSkyClip = mul(prevViewProj, float4(rayDir, 0.0));
        payload.velocity = previousSkyClip.w > 0.0
                               ? ndc - previousSkyClip.xy / previousSkyClip.w
                               : float2(0.0, 0.0);

        TraceRay(gTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

        colorSum += payload.color;
        if (i == 0)
        {
            centerDistance = payload.distance;
            centerDepth = payload.hardwareDepth;
            centerVelocity = payload.velocity;
        }
    }

    gOutput[pixel + pushData.outputOffset] = float4(colorSum / float(sampleCount), 1.0);
    gDistance[pixel + pushData.outputOffset] = centerDistance;
    gVelocity[pixel + pushData.outputOffset] = centerVelocity;
    if (pushData.aaMode == kAaModeDlss)
        gDlssDepth[pixel + pushData.outputOffset] = centerDepth;
}

[shader("miss")]
void missMain(inout RayPayload payload)
{
    AtmosphereParams params;
    params.rayleighCoeff = atmosphereScattering.xyz;
    params.mieCoeff = atmosphereScattering.w;
    params.planetRadius = atmosphereRadii.x;
    params.atmosphereRadius = atmosphereRadii.y;
    params.rayleighScaleHeight = atmosphereRadii.z;
    params.mieScaleHeight = atmosphereRadii.w;
    params.mieG = atmosphereMisc.x;
    params.sunIntensity = atmosphereMisc.y;

    const float3 rayOrigin = float3(cameraWorldPos.x, params.planetRadius + cameraWorldPos.y, cameraWorldPos.z);
    payload.color = Atmosphere(WorldRayDirection(), rayOrigin, normalize(lightDirWS.xyz), params);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload)
{
    payload.visibility = 1.0;
}

[shader("anyhit")]
void anyHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    if (!AlphaTestPasses(attribs))
        IgnoreHit();
}

[shader("anyhit")]
void shadowAnyHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    if (!AlphaTestPasses(attribs))
        IgnoreHit();
}

[shader("closesthit")]
void closestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    const RtInstanceInfo info = gInstances[InstanceID()];
    const float3 barycentrics = Barycentrics(attribs);
    const uint3 indices = TriangleIndices(info);

    // Object-space hit position - needed by both branches below, to reproject
    // through info.prevModel for velocity.
    const float3 op0 = LoadFloat3(info, indices.x, 0);
    const float3 op1 = LoadFloat3(info, indices.y, 0);
    const float3 op2 = LoadFloat3(info, indices.z, 0);
    const float3 objectPos = op0 * barycentrics.x + op1 * barycentrics.y + op2 * barycentrics.z;
    const float3 hitWorldPos = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    const bool isError = (info.flags & kInstanceError) != 0;

    float3 color;

    if ((info.flags & (kInstanceUnlit | kInstanceError)) != 0)
    {
        // error.slang's solid magenta, or unlit.slang's vertex color * tint.
        color = isError ? float3(1.0, 0.0, 1.0)
                        : InterpolateFloat3(info, indices, barycentrics, kColorOffset) * info.tint.rgb;
        if (pushData.aaMode == kAaModeDlss)
        {
            const uint2 guidePixel = DispatchRaysIndex().xy + pushData.outputOffset;
            gAlbedo[guidePixel] = float4(color, 1.0);
            gSpecularAlbedo[guidePixel] = float4(0.0, 0.0, 0.0, 0.0);
            gNormal[guidePixel] = float4(FaceNormal(info, op0, op1, op2), 1.0);
        }
    }
    else
    {
        const float3 objNormal = InterpolateFloat3(info, indices, barycentrics, kNormalOffset);

        float3 N = normalize(
            info.normalCol0.xyz * objNormal.x +
            info.normalCol1.xyz * objNormal.y +
            info.normalCol2.xyz * objNormal.z);
        // model.slang lights a double-sided back face with the flipped normal.
        if ((info.flags & kInstanceDoubleSided) != 0 && dot(N, WorldRayDirection()) > 0.0)
            N = -N;

        const float3 albedo = (info.flags & kInstanceTextured) != 0
                                  ? ModelBaseColor(info, indices, barycentrics).rgb
                                  : kDefaultAlbedo * info.tint.rgb;

        RayVisibility visibility;
        visibility.origin = hitWorldPos + FaceNormal(info, op0, op1, op2) *
                                              max(kShadowNormalBias, length(hitWorldPos) * kShadowNormalBiasPerDistance);
        color = ShadeLit(visibility, albedo, N, hitWorldPos);

        if (pushData.aaMode == kAaModeDlss)
        {
            const uint2 guidePixel = DispatchRaysIndex().xy + pushData.outputOffset;
            gAlbedo[guidePixel] = float4(albedo, 1.0);
            gSpecularAlbedo[guidePixel] = float4(kSpecularStrength.xxx, 1.0);
            gNormal[guidePixel] = float4(N, kMaterialRoughness);
        }
    }

    // error.slang draws no fog.
    payload.color = isError ? color : ApplyFog(color, RayTCurrent());
    payload.distance = RayTCurrent();

    // hitWorldPos is already this frame's camera-relative space (cameraPos is
    // its origin); info.prevModel puts objectPos in that SAME space (rebased
    // to this frame's camera, not last frame's - see DrawItem::previousModel),
    // so viewProj/prevViewProj alone carry the camera's own motion.
    const float4 currClip = mul(viewProj, float4(hitWorldPos, 1.0));
    payload.hardwareDepth = saturate(currClip.z / currClip.w);
    const float4 prevClip = mul(prevViewProj, mul(info.prevModel, float4(objectPos, 1.0)));
    payload.velocity = (currClip.xy / currClip.w) - (prevClip.xy / prevClip.w);
}
