#include "ddgi/Irradiance.hlsl"

[[vk::binding(0, 2)]] StructuredBuffer<DDGIVolumeDescGPUPacked> gVolumes;
[[vk::binding(8, 2)]] Texture2DArray<float4> gIrradiance;
[[vk::binding(9, 2)]] Texture2DArray<float4> gProbeDistance;
[[vk::binding(10, 2)]] Texture2DArray<float4> gProbeData;
[[vk::binding(11, 2)]] SamplerState gProbeSampler;

float3 SampleProbeIrradiance(float3 position, float3 geometricNormal, float3 rayDirection,
    DDGIVolumeDescGPU volume)
{
    DDGIVolumeResources resources;
    resources.probeIrradiance = gIrradiance;
    resources.probeDistance = gProbeDistance;
    resources.probeData = gProbeData;
    resources.bilinearSampler = gProbeSampler;
    return DDGIGetVolumeIrradiance(position, DDGIGetSurfaceBias(geometricNormal, rayDirection, volume),
        geometricNormal, volume, resources);
}
