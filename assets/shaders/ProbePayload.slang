#ifndef MIR_PROBE_PAYLOAD
#define MIR_PROBE_PAYLOAD
struct ProbePayload
{
    float3 radiance;
    float distance;
    uint kind;
    float3 diffuseReflectance;
    float3 geometricNormal;
    uint materialHit;
};
static const uint kFixedProbeRays = 32;
#endif
