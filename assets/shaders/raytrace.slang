#include "RaySceneCommon.slang"
#include "RayPayload.slang"
#include "SurfaceCommon.slang"
#include "PbrCommon.slang"
#ifdef MIR_DDGI
#include "DdgiSampling.slang"
#endif


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

static const uint kAaModeOff = 0;
static const uint kAaModeMsaa = 2;
static const uint kAaModeDlss = 3;
static const uint kMaxBlendLayers = 8;

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
    float giIntensity;
    float giWeight;
};

[[vk::push_constant]]
RtPushConstants pushData;

// No per-material roughness exists yet (MaterialDesc has none) - same fixed-constant
// simplification kShininess already is, standing in for Ray Reconstruction's roughness guide.
static const float kMaterialRoughness = pow(2.0 / (kShininess + 2.0), 0.25);
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

        float transmittance = 1.0;
        for (uint layer = 0; layer <= kMaxBlendLayers; ++layer)
        {
            payload.opacity = 1.0; // Opaque surface hits and sky leave this alone.
            payload.writeGuides = layer == 0 && pushData.aaMode == kAaModeDlss ? 1u : 0u;
            // After the layer budget, trace once for the opaque background.
            payload.skipBlend = layer == kMaxBlendLayers ? 1u : 0u;
            TraceRay(gTlas, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

            if (i == 0 && layer == 0)
            {
                centerDistance = payload.distance;
                centerDepth = payload.hardwareDepth;
                centerVelocity = payload.velocity;
            }

            colorSum += transmittance * payload.opacity * payload.color;
            if (payload.opacity == 1.0)
                break;
            transmittance *= 1.0 - payload.opacity;
            // Keep the ray origin so hit distances, fog and projection stay camera-relative.
            const float bias = max(0.0001, abs(payload.distance) * 0.000001);
            ray.TMin = min(ray.TMax, payload.distance + bias);
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
    const RtInstanceInfo info = gInstances[InstanceID()];
    if ((info.flags & kInstanceAlphaBlend) != 0)
    {
        if (payload.skipBlend != 0 ||
            ModelBaseColor(info, TriangleIndices(info), Barycentrics(attribs)).a <= 0.0)
            IgnoreHit();
    }
    else if (!AlphaTestPasses(attribs))
        IgnoreHit();
}

[shader("anyhit")]
void shadowAnyHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    if (!OpaqueOrMaskedHit(attribs))
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
        if (payload.writeGuides != 0)
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

        const float4 baseColor = (info.flags & kInstanceTextured) != 0
                                     ? ModelBaseColor(info, indices, barycentrics)
                                     : float4(kDefaultAlbedo * info.tint.rgb, 1.0);
        const float3 albedo = baseColor.rgb;

        RayVisibility visibility;
        visibility.origin = hitWorldPos + FaceNormal(info, op0, op1, op2) *
                                              max(kShadowNormalBias, length(hitWorldPos) * kShadowNormalBiasPerDistance);
        float roughness = kMaterialRoughness;
        if ((info.flags & kInstanceAlphaBlend) != 0)
        {
            const SurfaceOutput surface = BlendedModelSurface(albedo, N);
            color = ShadePbr(visibility, surface, hitWorldPos);
            roughness = surface.roughness;
            payload.opacity = saturate(baseColor.a);
        }
        else
        {
#ifdef MIR_DDGI
            const DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(gVolumes[0]);
            const float weight = DDGIGetVolumeBlendWeight(hitWorldPos, volume) * pushData.giWeight;
            float3 indirect = ambientColor.rgb;
            if (weight > 0.0)
            {
                const float3 geometricNormal = FaceNormal(info, op0, op1, op2);
                const float3 irradiance = SampleProbeIrradiance(hitWorldPos, geometricNormal, WorldRayDirection(), volume);
                // Replace the ambient baseline with GI where the probe volume contributes.
                indirect = lerp(ambientColor.rgb, irradiance * (pushData.giIntensity / 3.14159265359), weight);
            }
            color = ShadeLitWithIndirect(visibility, albedo, N, hitWorldPos, indirect);
#else
            color = ShadeLit(visibility, albedo, N, hitWorldPos);
#endif
        }

        if (payload.writeGuides != 0)
        {
            const uint2 guidePixel = DispatchRaysIndex().xy + pushData.outputOffset;
            gAlbedo[guidePixel] = float4(albedo, 1.0);
            gSpecularAlbedo[guidePixel] = float4(kSpecularStrength.xxx, 1.0);
            gNormal[guidePixel] = float4(N, roughness);
        }
    }

    // error.slang draws no fog.
    payload.color = isError ? color : ApplyFog(color, RayTCurrent());
    payload.distance = RayTCurrent();

    // hitWorldPos is already this frame's camera-relative space (cameraPos is
    // its origin); info.prevModel puts the previous pose in that SAME space (rebased
    // to this frame's camera, not last frame's - see DrawItem::previousModel),
    // so viewProj/prevViewProj alone carry the camera's own motion.
    const float4 currClip = mul(viewProj, float4(hitWorldPos, 1.0));
    payload.hardwareDepth = saturate(currClip.z / currClip.w);
    RtInstanceInfo previousInfo = info;
    previousInfo.vertexAddress = info.previousVertexAddress;
    const float3 previousObjectPos = InterpolateFloat3(previousInfo, indices, barycentrics, 0);
    const float4 prevClip = mul(prevViewProj, mul(info.prevModel, float4(previousObjectPos, 1.0)));
    payload.velocity = (currClip.xy / currClip.w) - (prevClip.xy / prevClip.w);
}
