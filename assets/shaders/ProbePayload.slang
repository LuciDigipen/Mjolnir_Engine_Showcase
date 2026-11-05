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
// Material hit shaders build without the SDK; ddgiProbe.slang checks this matches it.
#define MIR_DDGI_NUM_FIXED_RAYS 32
static const uint kFixedProbeRays = MIR_DDGI_NUM_FIXED_RAYS;
#endif
