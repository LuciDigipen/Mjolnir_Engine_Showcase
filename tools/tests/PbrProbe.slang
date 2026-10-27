#include "../../assets/shaders/SurfaceCommon.slang"
#include "../../assets/shaders/PbrCommon.slang"

[[vk::binding(0, 0)]] ByteAddressBuffer probeInput;
[[vk::binding(1, 0)]] RWByteAddressBuffer pbrOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void probeMain()
{
    const float3 N = float3(0, 0, 1);
    bool passed = true;
    for (uint i = 0; i < 3; ++i)
    {
        const float cosine = i == 0 ? 1.0 : (i == 1 ? 0.5 : 0.125);
        const float3 direction = float3(sqrt(1.0 - cosine * cosine), 0, cosine);
        const float diffuse = EvaluateDirectPbr(float3(0.5), float3(0), 0.5,
            N, direction, direction, float3(1)).x;
        passed = passed && abs(diffuse - 0.5 * cosine) < 1e-5;
    }
    passed = passed && all(EvaluateDirectPbr(float3(0.5), float3(0), 0.5,
        N, N, float3(0, 0, -1), float3(1)) == float3(0));
    passed = passed && abs(EvaluateDirectPbr(float3(0), float3(0.04), 0.5,
        N, N, N, float3(1)).x - 0.16) < 1e-5;
    passed = passed && abs(EvaluateDirectPbr(float3(0), float3(0.04), 0.1,
        N, N, N, float3(1)).x - 100.0) < 1e-3;
    passed = passed && all(SpecularBrdf(float3(0.04), 0.5, N, -N, N) == float3(0));
    const float3 grazingV = normalize(float3(1, 0, 1e-4));
    passed = passed && all(isfinite(EvaluateDirectPbr(float3(0.5), float3(0.04),
        0.5, N, grazingV, N, float3(1))));
    passed = passed && abs(SpecularAlbedoGuide(float3(0.04), 0.25, 0.5).x - 0.0557753554) < 1e-5;
    passed = passed && abs(SpecularAlbedoGuide(float3(0.04), 1.0, 1.0).x - 0.0132717084) < 1e-5;
    passed = passed && abs(SpecularAlbedoGuide(float3(0.04), 0.01, 0.1).x - 0.587247891) < 1e-5;
    passed = passed && all(SpecularAlbedoGuide(float3(0), 0.25, 0.5) == float3(0));
    pbrOutput.Store(0, asuint(asfloat(probeInput.Load(0)) + (passed ? 3.0 : 0.0)));
}
