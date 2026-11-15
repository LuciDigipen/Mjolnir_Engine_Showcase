#ifndef MIR_NORMAL_MAPPING
#define MIR_NORMAL_MAPPING
#include "TangentFrame.slang"

float3 ApplyTangentNormal(float3 normal, float4 tangent, float3 encoded, float scale, bool flipY = false)
{
    const float3 n = SafeNormalize(normal, float3(0, 1, 0));
    float3 t = tangent.xyz - n * dot(n, tangent.xyz);
    if (!(dot(t, t) > 1e-12))
        t = cross(n, abs(n.y) < .99 ? float3(0, 1, 0) : float3(1, 0, 0));
    t = SafeNormalize(t, float3(1, 0, 0));
    float3 local = encoded * 2 - 1;
    local.xy *= scale;
    if (flipY)
        local.y = -local.y;
    local = SafeNormalize(local, float3(0, 0, 1));
    const float3 b = (tangent.w < 0 ? -1.0 : 1.0) * cross(n, t);
    return SafeNormalize(t * local.x + b * local.y + n * local.z, n);
}
#endif
