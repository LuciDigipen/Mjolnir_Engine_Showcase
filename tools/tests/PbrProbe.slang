#include "../../assets/shaders/SurfaceCommon.slang"
#include "../../assets/shaders/PbrCommon.slang"

[[vk::binding(1, 0)]] RWByteAddressBuffer pbrOutput;

void Store(uint offset, float value)
{
    pbrOutput.Store(offset, asuint(value));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    const float3 N = float3(0, 0, 1);
    const float3 V = float3(0, 0, 1);

    // Analytic oracle: F0 = 0 makes Schlick zero, so albedo 0.5 with V = L
    // must be exactly 0.5 * NdotL.
    Store(0, EvaluateDirectPbr(float3(0.5), float3(0), 0.5, N, V, N, float3(1)).x);
    const float3 tilted = normalize(float3(0.5, 0.5, 0.70710678));
    Store(4, EvaluateDirectPbr(float3(0.5), float3(0), 0.5, N, V, tilted, float3(1)).x);

    // A back-facing light contributes zero.
    Store(8, EvaluateDirectPbr(float3(0.5), float3(0), 0.5, N, V, float3(0, 0, -1), float3(1)).x);

    // Grazing view stays finite and includes the pi-scaled specular convention.
    const float3 grazingV = normalize(float3(1, 0, 1e-4));
    Store(12, EvaluateDirectPbr(float3(0.5), float3(0.04), 0.5, N, grazingV, N, float3(1)).x);

    // Guide: F0/roughness/NoV dependence; gNormal.w remains linear roughness.
    Store(16, SpecularAlbedoGuide(float3(0.04), 0.25, 1.0).x);
    Store(20, SpecularAlbedoGuide(float3(0.04), 0.25, 0.0).x);
    Store(24, SpecularAlbedoGuide(float3(0.5), 0.25, 0.0).x);
    Store(28, SpecularAlbedoGuide(float3(0.04), 0.81, 0.0).x);
}
