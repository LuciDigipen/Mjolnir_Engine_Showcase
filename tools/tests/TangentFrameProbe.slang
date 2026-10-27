#include "../../assets/shaders/TangentFrame.slang"

[[vk::binding(1, 0)]] RWByteAddressBuffer probeOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    // Columns of a rotation, non-uniform scale and shear. A row-constructed
    // matrix produces a different tangent, so a GPU readback catches it.
    const float3 c0 = float3(2, 1, 0);
    const float3 c1 = float3(0, 3, 0);
    const float3 c2 = float3(0, 0, 4);
    const float4 tangent = TransformTangentFrame(c0, c1, c2, float4(1, 0, 0, 1), float3(0, 0, 1));
    probeOutput.Store(0, asuint(tangent.x));
    probeOutput.Store(4, asuint(tangent.y));
    probeOutput.Store(8, asuint(tangent.z));
    probeOutput.Store(12, asuint(tangent.w));

    const float4 mirrored = TransformTangentFrame(
        float3(-2, 1, 0), c1, c2, float4(1, 0, 0, 1), float3(0, 0, 1));
    probeOutput.Store(16, asuint(mirrored.x));
    probeOutput.Store(20, asuint(mirrored.y));
    probeOutput.Store(24, asuint(mirrored.z));
    probeOutput.Store(28, asuint(mirrored.w));

    const float4 degenerate = TransformTangentFrame(c0, c1, c2, float4(0, 0, 0, 1), float3(0, 1, 0));
    probeOutput.Store(32, asuint(degenerate.x));
    probeOutput.Store(36, asuint(degenerate.y));
    probeOutput.Store(40, asuint(degenerate.z));
    probeOutput.Store(44, asuint(degenerate.w));
}
