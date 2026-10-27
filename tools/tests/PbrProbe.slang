#include "../../assets/shaders/SurfaceCommon.slang"
#include "../../assets/shaders/PbrCommon.slang"
#include "../../assets/materials/default.slang"

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
    DefaultMaterial material;
    material.albedoRoughness = float4(0.5, 0.5, 0.5, 0.4924790605);
    material.surfaceParams = float4(0.04, 1, 0, 0);
    SurfaceInput input;
    input.normalWS = N;
    input.vertexColor = float4(1, 0, 0, 1);
    MaterialContext context;
    context.rayTraced = false;
    context.uvDx = float2(0);
    context.uvDy = float2(0);
    const SurfaceOutput base = material.Evaluate(input, context);
    const SurfaceOutput tinted = ApplySurfaceTint(base, float4(0.2, 0.6, 1, 1));
    passed = passed && all(abs(base.albedo - float3(0.5)) < 1e-5);
    passed = passed && all(abs(tinted.albedo - float3(0.1, 0.3, 0.5)) < 1e-5);
    pbrOutput.Store(0, asuint(asfloat(probeInput.Load(0)) + (passed ? 3.0 : 0.0)));
}
