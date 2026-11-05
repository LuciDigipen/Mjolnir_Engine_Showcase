[[vk::push_constant]]
cbuffer DeformationPushConstants
{
    float *sourceVertices;
    float4 *deformation;
    float *outputVertices;
    uint vertexCount;
};

#include "ModelDeformation.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void deformMain(uint3 thread : SV_DispatchThreadID)
{
    uint vertex = thread.x;
    if (vertex >= vertexCount)
        return;
    uint offset = vertex * 16; // mir::Vertex
    for (uint component = 0; component < 16; ++component)
        outputVertices[offset + component] = sourceVertices[offset + component];

    float3 p = float3(sourceVertices[offset], sourceVertices[offset + 1], sourceVertices[offset + 2]);
    float3 n = float3(sourceVertices[offset + 6], sourceVertices[offset + 7], sourceVertices[offset + 8]);
    float3 t = float3(sourceVertices[offset + 12], sourceVertices[offset + 13], sourceVertices[offset + 14]);
    DeformModelVertex(vertex, p, n, t);
    for (uint component = 0; component < 3; ++component)
    {
        outputVertices[offset + component] = p[component];
        outputVertices[offset + 6 + component] = n[component];
        outputVertices[offset + 12 + component] = t[component];
    }
}
