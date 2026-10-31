// Reflection-only witness for a material type. A ByteAddressBuffer Load<M>
// does not expose M's fields in reflection, so this computes the same layout
// through a typed StructuredBuffer for the build-time material packer.
#include "SurfaceCommon.slang"

[[vk::binding(1, 0)]] RWByteAddressBuffer layoutOutput;

[shader("compute")]
[numthreads(1, 1, 1)]
void layoutMain<M : IMaterial>(uniform StructuredBuffer<M> layout)
{
    SurfaceInput input;
    input.positionRS = float3(0, 0, 0);
    input.normalWS = float3(0, 0, 1);
    input.tangentWS = float3(1, 0, 0);
    input.bitangentSign = 1.0;
    input.uv0 = float2(0, 0);
    input.localPos = float3(0, 0, 0);
    input.viewDirWS = float3(0, 0, -1);
    input.viewDistance = 1.0;
    input.vertexColor = float4(1, 1, 1, 1);
    input.frontFace = true;
    input.uvDensity = 0.0;

    MaterialContext context;
    context.rayTraced = true;
    context.uvDx = float2(0, 0);
    context.uvDy = float2(0, 0);
    context.footprintMeters = 0.0;
    context.incidence = 1.0;
    context.uvDensity = 0.0;

    layoutOutput.Store(0, asuint(layout[0].Evaluate(input, context).roughness));
}
