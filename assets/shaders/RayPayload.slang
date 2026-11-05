#ifndef MIR_RAY_PAYLOAD
#define MIR_RAY_PAYLOAD

// Camera-ray ABI shared by the ray generation shader and every material hit group.
struct RayPayload
{
    float3 color;
    float distance;
    float hardwareDepth;
    float2 velocity; // NDC-space, current minus previous.
    float opacity;
    uint writeGuides;
    uint skipBlend;
};

#endif
