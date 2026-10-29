// Batch guards around the pinned SDK kernels. Every lane in a blending group
// agrees on the guard, preserving the SDK's group barriers and border copies.
#if MIR_DDGI_PASS == 0
#define DDGIProbeBlendingCS DDGIProbeBlendingImpl
#include "ddgi/ProbeBlendingCS.hlsl"
#undef DDGIProbeBlendingCS
#elif MIR_DDGI_PASS == 1
#define DDGIProbeRelocationCS DDGIProbeRelocationImpl
#include "ddgi/ProbeRelocationCS.hlsl"
#undef DDGIProbeRelocationCS
#else
#define DDGIProbeClassificationCS DDGIProbeClassificationImpl
#include "ddgi/ProbeClassificationCS.hlsl"
#undef DDGIProbeClassificationCS
#endif

[[vk::binding(7, 0)]] StructuredBuffer<uint4> UpdateBudget;

bool ProbeSelected(uint index)
{
    uint4 budget = UpdateBudget[0]; // first, count, total, history ready
    return index < budget.z && ((index + budget.z - budget.x) % budget.z) < budget.y;
}

#if MIR_DDGI_PASS == 0
[numthreads(RTXGI_DDGI_PROBE_NUM_TEXELS, RTXGI_DDGI_PROBE_NUM_TEXELS, 1)]
void DDGIProbeBlendingCS(uint3 dispatch : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID,
    uint3 group : SV_GroupID, uint index : SV_GroupIndex)
{
    DDGIVolumeDescGPU volume = UnpackDDGIVolumeDescGPU(DDGIVolumes[0]);
    if (ProbeSelected(DDGIGetProbeIndex(group * uint3(RTXGI_DDGI_PROBE_NUM_TEXELS,
        RTXGI_DDGI_PROBE_NUM_TEXELS, 1), RTXGI_DDGI_PROBE_NUM_TEXELS, volume)))
        DDGIProbeBlendingImpl(dispatch, thread, group, index);
}
#elif MIR_DDGI_PASS == 1
[numthreads(32, 1, 1)]
void DDGIProbeRelocationCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (ProbeSelected(dispatch.x)) DDGIProbeRelocationImpl(dispatch);
}
#else
[numthreads(32, 1, 1)]
void DDGIProbeClassificationCS(uint3 dispatch : SV_DispatchThreadID)
{
    if (ProbeSelected(dispatch.x)) DDGIProbeClassificationImpl(dispatch);
}
#endif
