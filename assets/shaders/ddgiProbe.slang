#include "RaySceneCommon.slang"
#include "DdgiSampling.slang"
#include "ddgi/include/ProbeRayCommon.hlsl"

[[vk::binding(1, 2)]] RWTexture2DArray<float4> gRayData;
[[vk::binding(7, 2)]] StructuredBuffer<uint4> gBudget;

#include "ProbePayload.slang"

[shader("raygeneration")]
void probeRayGen()
{
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(gVolumes[0]);
    uint4 budget = gBudget[0];
    uint probeIndex = (budget.x + DispatchRaysIndex().y) % budget.z;
    uint rayIndex = DispatchRaysIndex().x;
    uint3 coords = DDGIGetRayDataTexelCoords(rayIndex, probeIndex, volume);
    uint3 probeCoords = DDGIGetProbeTexelCoords(probeIndex, volume);
    // Inactive probes still trace fixed rays so classification can reactivate them.
    if (rayIndex >= RTXGI_DDGI_NUM_FIXED_RAYS &&
        gProbeData.Load(int4(probeCoords, 0)).w == RTXGI_DDGI_PROBE_STATE_INACTIVE)
        return;
    RayDesc ray;
    ray.Origin = DDGIGetProbeWorldPosition(DDGIGetProbeCoords(probeIndex, volume), volume, gProbeData);
    ray.Direction = DDGIGetProbeRayDirection(rayIndex, volume);
    ray.TMin = 0.001f;
    ray.TMax = volume.probeMaxRayDistance;
    ProbePayload payload;
    payload.radiance = float3(0);
    payload.distance = ray.TMax;
    payload.kind = 0;
    payload.materialHit = 0;
    // Back faces are required by relocation/classification.
    TraceRay(gTlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    if (payload.materialHit != 0 && budget.w != 0)
    {
        const float3 position = ray.Origin + payload.distance * ray.Direction;
        const float3 irradiance = SampleProbeIrradiance(position, payload.geometricNormal, ray.Direction, volume);
        payload.radiance += payload.diffuseReflectance * (1.0 / 3.14159265359) * irradiance *
            DDGIGetVolumeBlendWeight(position, volume);
    }
    if (payload.kind == 0)
        DDGIStoreProbeRayMiss(gRayData, coords, volume, payload.radiance);
    else if (payload.kind == HIT_KIND_TRIANGLE_BACK_FACE)
        DDGIStoreProbeRayBackfaceHit(gRayData, coords, volume, payload.distance);
    else
        DDGIStoreProbeRayFrontfaceHit(gRayData, coords, volume, payload.radiance, payload.distance);
}

[shader("miss")]
void probeMiss(inout ProbePayload payload)
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
    float3 absolute = cameraWorldPos.xyz + WorldRayOrigin();
    payload.radiance = Atmosphere(WorldRayDirection(),
        float3(absolute.x, params.planetRadius + absolute.y, absolute.z), normalize(lightDirWS.xyz), params);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload) { payload.visibility = 1.0; }

[shader("anyhit")]
void probeAnyHit(inout ProbePayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    if (!OpaqueOrMaskedHit(attribs)) IgnoreHit();
}

[shader("anyhit")]
void shadowAnyHitMain(inout ShadowPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    if (!OpaqueOrMaskedHit(attribs)) IgnoreHit();
}

// Diffuse direct lighting excludes ambient to avoid feeding it into probe history.
float3 ProbeDirectDiffuse(RayVisibility visibility, float3 albedo, float3 normal, float3 position)
{
    float3 light = normalize(lightDirWS.xyz);
    float diffuse = max(dot(normal, light), 0.0);
    float3 result = diffuse > 0 ? albedo * lightColor.rgb * diffuse * visibility.Sun(light) : float3(0);
    for (int i = 0; i < int(lightingMeta.x); ++i)
    {
        GpuLight local = gLights[int(lightingMeta.y) + i];
        float3 delta = local.positionAndRange.xyz - position;
        float distance = length(delta);
        float3 direction = delta / max(distance, 1e-5);
        float attenuation = saturate(1.0 - distance / max(local.positionAndRange.w, 1e-5));
        attenuation *= dot(-direction, local.directionAndSpotCos.xyz) >= local.directionAndSpotCos.w ? 1.0 : 0.0;
        float nDotL = max(dot(normal, direction), 0.0);
        if (attenuation > 0 && nDotL > 0)
            result += albedo * local.color.rgb * attenuation * nDotL * visibility.Local(local, direction, distance);
    }
    return result;
}

[shader("closesthit")]
void probeClosestHit(inout ProbePayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    payload.distance = RayTCurrent();
    payload.kind = HitKind();
    if (payload.kind == HIT_KIND_TRIANGLE_BACK_FACE || DispatchRaysIndex().x < RTXGI_DDGI_NUM_FIXED_RAYS)
        return;
    RtInstanceInfo info = gInstances[InstanceID()];
    uint3 indices = TriangleIndices(info);
    float3 barycentrics = Barycentrics(attribs);
    if ((info.flags & kInstanceError) != 0) return;
    if ((info.flags & kInstanceUnlit) != 0)
    {
        payload.radiance = InterpolateFloat3(info, indices, barycentrics, kColorOffset) * info.tint.rgb;
        return;
    }
    float3 position = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    float3 normal = FaceNormal(info, LoadFloat3(info, indices.x, 0), LoadFloat3(info, indices.y, 0),
        LoadFloat3(info, indices.z, 0));
    float3 albedo = (info.flags & kInstanceTextured) != 0 ? ModelBaseColor(info, indices, barycentrics).rgb
        : kDefaultAlbedo * info.tint.rgb;
    RayVisibility visibility;
    visibility.origin = position + normal * max(kShadowNormalBias, length(position) * kShadowNormalBiasPerDistance);
    payload.radiance = ProbeDirectDiffuse(visibility, albedo, normal, position);
    if (gBudget[0].w != 0)
    {
        DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(gVolumes[0]);
        float3 irradiance = SampleProbeIrradiance(position, normal, WorldRayDirection(), volume);
        payload.radiance += min(albedo, float3(0.9)) * (1.0 / 3.14159265359) * irradiance *
            DDGIGetVolumeBlendWeight(position, volume);
    }
}
