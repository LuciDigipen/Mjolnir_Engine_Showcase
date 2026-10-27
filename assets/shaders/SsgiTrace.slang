#include "SsgiTraceCommon.slang"
#include "LightingCommon.slang"

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}

float Random(inout uint state)
{
    state = Hash(state);
    return float(state >> 8) * (1.0 / 16777216.0);
}

float3 Reconstruct(float2 uv, float depth)
{
    float4 p = mul(invViewProj, float4(uv * 2 - 1, depth, 1));
    return p.xyz / p.w;
}

float3 Hemisphere(float3 normal, inout uint state)
{
    float r = sqrt(Random(state));
    float phi = 6.28318530718 * Random(state);
    float3 tangent = normalize(cross(abs(normal.z) < 0.999 ? float3(0, 0, 1) : float3(0, 1, 0), normal));
    return tangent * (r * cos(phi)) + cross(normal, tangent) * (r * sin(phi)) +
        normal * sqrt(max(0.0, 1 - r * r));
}

float4 Trace(float3 origin, float3 direction, uint2 receiver)
{
    float4 start = mul(viewProj, float4(origin, 1));
    float4 delta = mul(viewProj, float4(direction, 0));
    // Perspective reconstruction below uses clip.w as positive view depth.
    if (start.w <= 0 || start.z <= 0 || start.z >= start.w) return 0;
    float distance = pc.maxDistance;
    if (delta.z < 0) distance = min(distance, -start.z / delta.z * 0.9999);
    if (delta.w - delta.z < 0)
        distance = min(distance, (start.w - start.z) / (delta.z - delta.w) * 0.9999);
    float4 finish = start + delta * distance;
    if (finish.w <= 0 || distance <= 0) return 0;
    float2 uv0 = start.xy / start.w * 0.5 + 0.5;
    float2 uv1 = finish.xy / finish.w * 0.5 + 0.5;
    float2 line = uv1 - uv0;
    float lengthPixels = length(line * float2(pc.viewSize));
    if (!all(isfinite(uv0)) || !all(isfinite(uv1)) || lengthPixels < 0.5 ||
        any(uv0 < 0) || any(uv0 >= 1)) return 0;
    float end = 1;
    for (uint axis = 0; axis < 2; ++axis)
    {
        if (line[axis] > 0) end = min(end, (1 - uv0[axis]) / line[axis]);
        if (line[axis] < 0) end = min(end, -uv0[axis] / line[axis]);
    }
    float epsilon = 0.0001 / max(lengthPixels, 1.0);
    float f = 0;
    uint mip = min(3u, pc.mipCount - 1);
    float2 inverseW = 1 / float2(start.w, finish.w);
    for (uint step = 0; step < pc.steps && f < end; ++step)
    {
        float2 uv = uv0 + line * f;
        if (any(uv < 0) || any(uv >= 1)) break;
        uint2 size = max(uint2(1), pc.viewSize >> mip);
        uint2 cell = min(uint2(uv * float2(size)), size - 1);
        float next = end;
        for (uint axis = 0; axis < 2; ++axis)
        {
            if (abs(line[axis]) < 1e-15) continue;
            float boundary = float(cell[axis] + (line[axis] > 0 ? 1u : 0u)) / float(size[axis]);
            next = min(next, max(f, (boundary - uv0[axis]) / line[axis]));
        }
        float nearest = pyramid.Load(int3(cell, mip));
        float rayZ0 = lerp(start.z / start.w, finish.z / finish.w, f);
        float rayZ1 = lerp(start.z / start.w, finish.z / finish.w, next);
        if (nearest > 0 && min(rayZ0, rayZ1) <= nearest)
        {
            if (mip > 0) { --mip; continue; }
            if (any(cell != receiver))
            {
                float2 hitUv = (float2(cell) + 0.5) / float2(pc.viewSize);
                float3 surface = Reconstruct(hitUv, nearest);
                float surfaceW = mul(viewProj, float4(surface, 1)).w;
                float w0 = 1 / lerp(inverseW.x, inverseW.y, f);
                float w1 = 1 / lerp(inverseW.x, inverseW.y, next);
                if (min(w0, w1) <= surfaceW + pc.thickness && max(w0, w1) >= surfaceW)
                {
                    float low = f, high = next;
                    // Perspective-correct refinement within the candidate pixel's slab.
                    for (uint refine = 0; refine < 5; ++refine)
                    {
                        float mid = (low + high) * 0.5;
                        float w = 1 / lerp(inverseW.x, inverseW.y, mid);
                        if ((w < surfaceW) == (w1 >= w0)) low = mid;
                        else high = mid;
                    }
                    float hitF = (low + high) * 0.5;
                    float hitW = 1 / lerp(inverseW.x, inverseW.y, hitF);
                    if (hitW >= surfaceW - pc.thickness * 0.05 && hitW <= surfaceW + pc.thickness)
                    {
                        float4 hit = normalKind.Load(int3(pc.viewOffset + cell, 0));
                        float facing = dot(hit.xyz, -direction);
                        if (hit.w < 0.5 || facing <= 0) return 0;
                        float2 edge = min(hitUv, 1 - hitUv) * float2(pc.viewSize);
                        float confidence = saturate(min(edge.x, edge.y) / 4) * saturate(facing * 4);
                        float3 source = hit.w > 2.5 ? float3(0) :
                            max(diffuseRadiance.Load(int3(pc.viewOffset + cell, 0)).rgb, float3(0));
                        return float4(source * confidence, confidence);
                    }
                }
            }
        }
        f = next + epsilon;
        mip = min(mip + 1, pc.mipCount - 1);
    }
    return 0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void traceMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.outputSize)) return;
    uint2 pixel = min(id.xy * pc.scale + pc.scale / 2, pc.viewSize - 1);
    float depth = sceneDepth.Load(int3(pc.viewOffset + pixel, 0));
    float4 receiver = normalKind.Load(int3(pc.viewOffset + pixel, 0));
    float4 result = 0;
    // Orthographic/invalid projections and unsupported materials have no raw estimate.
    float perspective = length(float3(viewProj[3][0], viewProj[3][1], viewProj[3][2]));
    if (depth > 0 && abs(receiver.w - 1) < 0.25 && perspective > 0 && pc.rays > 0)
    {
        float3 position = Reconstruct((float2(pixel) + 0.5) / float2(pc.viewSize), depth);
        float3 normal = normalize(receiver.xyz);
        float bias = max(0.001, min(pc.thickness * 0.25, pc.maxDistance * 0.01));
        uint state = Hash(pixel.x ^ Hash(pixel.y) ^ Hash(pc.seed + 1));
        if (all(isfinite(position)) && all(isfinite(normal)))
            for (uint ray = 0; ray < pc.rays; ++ray)
                result += Trace(position + normal * bias, Hemisphere(normal, state), pixel);
        result /= float(pc.rays);
        result.rgb *= 3.14159265359;
    }
    rawOutput[id.xy] = result;
}
