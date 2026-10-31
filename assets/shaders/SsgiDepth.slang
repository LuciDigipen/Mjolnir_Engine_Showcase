#include "SsgiTraceCommon.slang"

[shader("compute")]
[numthreads(8, 8, 1)]
void initMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.outputSize)) return;
    depthOutput[id.xy] = sceneDepth.Load(int3(pc.viewOffset + id.xy, 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void reduceMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.outputSize)) return;
    uint width, height;
    pyramid.GetDimensions(width, height);
    uint2 sourceSize = uint2(width, height);
    // Overlapping footprints preserve the last row/column of odd-sized views.
    uint2 begin = id.xy * sourceSize / pc.outputSize;
    uint2 end = ((id.xy + 1) * sourceSize + pc.outputSize - 1) / pc.outputSize;
    float nearest = 0;
    for (uint y = begin.y; y < end.y; ++y)
        for (uint x = begin.x; x < end.x; ++x)
            nearest = max(nearest, pyramid.Load(int3(x, y, 0)));
    depthOutput[id.xy] = nearest;
}
