#include "LightingCommon.slang"

struct FilterPush
{
    float4x4 previousMatrix;
    float4 cameraDelta;
    uint2 viewOffset;
    uint2 viewSize;
    uint2 traceSize;
    uint scale;
    uint historyValid;
    float thickness;
    uint pad;
};
[[vk::push_constant]] ConstantBuffer<FilterPush> pc;
[[vk::binding(0, 0)]] Texture2D<float> depthInput;
[[vk::binding(1, 0)]] Texture2D<float4> normalInput;
[[vk::binding(2, 0)]] Texture2D<float4> currentInput;
[[vk::binding(3, 0)]] Texture2D<float4> historyInput;
[[vk::binding(4, 0)]] Texture2D<float4> geometryInput;
[[vk::binding(5, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> output;
[[vk::binding(6, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> geometryOutput;

uint2 SourcePixel(uint2 pixel)
{ return min(pixel * pc.scale + pc.scale / 2, pc.viewSize - 1); }

float2 TraceCoordinate(float2 sourcePixel)
{
    float2 coordinate = (sourcePixel - float(pc.scale / 2)) / float(pc.scale);
    for (uint axis = 0; axis < 2; ++axis)
    {
        if (pc.traceSize[axis] == 1) coordinate[axis] = 0;
        else if (pc.scale == 2 && pc.viewSize[axis] % 2 != 0 && sourcePixel[axis] > float(pc.viewSize[axis] - 2))
            coordinate[axis] = float(pc.traceSize[axis] - 2) + sourcePixel[axis] - float(pc.viewSize[axis] - 2);
    }
    return coordinate;
}

bool Surface(uint2 pixel, out float3 position, out float3 normal, out float viewDepth)
{
    float depth = depthInput.Load(int3(pc.viewOffset + pixel, 0));
    float4 material = normalInput.Load(int3(pc.viewOffset + pixel, 0));
    float2 uv = (float2(pixel) + 0.5) / float2(pc.viewSize);
    float4 p = mul(invViewProj, float4(uv * 2 - 1, depth, 1));
    position = p.xyz / p.w;
    normal = material.xyz;
    viewDepth = mul(viewProj, float4(position, 1)).w;
    return depth > 0 && abs(material.w - 1) < 0.25 && viewDepth > 0 && isfinite(viewDepth) &&
        all(isfinite(position)) && all(isfinite(normal));
}

float SurfaceWeight(float3 center, float3 normal, float3 samplePosition, float3 sampleNormal)
{
    float facing = saturate(dot(normal, sampleNormal));
    float tolerance = max(0.01, min(pc.thickness * 0.5, 0.1)) + length(center) * 0.001;
    float planeDistance = max(abs(dot(samplePosition - center, normal)), abs(dot(samplePosition - center, sampleNormal)));
    return facing < 0.8 || planeDistance > tolerance * 3 ? 0 : pow(facing, 32) * exp(-planeDistance / tolerance);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.traceSize)) return;
    float3 position, normal;
    float viewDepth;
    if (!Surface(SourcePixel(id.xy), position, normal, viewDepth))
    {
        output[id.xy] = 0;
        geometryOutput[id.xy] = 0;
        return;
    }
    float4 current = currentInput.Load(int3(id.xy, 0));
    float4 result = current;
    if (pc.historyValid != 0)
    {
        float4 clip = mul(pc.previousMatrix, float4(position + pc.cameraDelta.xyz, 1));
        float2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (clip.w > 0 && clip.z > 0 && clip.z < clip.w && all(uv >= 0) && all(uv < 1))
        {
            // History texels correspond to traced source pixels, including odd last rows/columns.
            float2 previousPixel = TraceCoordinate(uv * float2(pc.viewSize) - 0.5);
            int2 base = int2(floor(previousPixel));
            float2 fraction = frac(previousPixel);
            float4 history = 0;
            float weight = 0;
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x)
                {
                    int2 sample = base + int2(x, y);
                    if (any(sample < 0) || any(sample >= int2(pc.traceSize))) continue;
                    float4 geometry = geometryInput.Load(int3(sample, 0));
                    if (geometry.w <= 0 || dot(geometry.xyz, normal) < 0.9 ||
                        abs(geometry.w - clip.w) > max(0.02, clip.w * 0.01)) continue;
                    float w = (x == 0 ? 1 - fraction.x : fraction.x) * (y == 0 ? 1 - fraction.y : fraction.y);
                    history += historyInput.Load(int3(sample, 0)) * w;
                    weight += w;
                }
            if (weight > 0.25)
            {
                history /= weight;
                float4 minimum = current, maximum = current;
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                    {
                        int2 sample = int2(id.xy) + int2(x, y);
                        if (any(sample < 0) || any(sample >= int2(pc.traceSize))) continue;
                        float3 p, n; float z;
                        if (!Surface(SourcePixel(uint2(sample)), p, n, z) || SurfaceWeight(position, normal, p, n) < 0.1) continue;
                        float4 value = currentInput.Load(int3(sample, 0));
                        minimum = min(minimum, value); maximum = max(maximum, value);
                    }
                history = clamp(history, minimum, maximum);
                result = lerp(current, history, 0.85);
            }
        }
    }
    output[id.xy] = result;
    geometryOutput[id.xy] = float4(normal, viewDepth);
}

float4 SpatialEstimate(uint2 centerPixel, int2 base, uint stride, float2 centerTrace)
{
    float3 position, normal; float depth;
    if (!Surface(centerPixel, position, normal, depth)) return 0;
    float4 sum = 0;
    float weight = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            int2 sample = base + int2(x, y) * int(stride);
            if (any(sample < 0) || any(sample >= int2(pc.traceSize))) continue;
            float3 p, n; float z;
            if (!Surface(SourcePixel(uint2(sample)), p, n, z)) continue;
            float2 distance = (float2(sample) - centerTrace) / float(stride);
            float w = exp(-dot(distance, distance) * 0.75) * SurfaceWeight(position, normal, p, n);
            sum += currentInput.Load(int3(sample, 0)) * w;
            weight += w;
        }
    return weight > 1e-5 ? sum / weight : float4(0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spatialMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.traceSize)) return;
    output[id.xy] = SpatialEstimate(SourcePixel(id.xy), int2(id.xy), 1, float2(id.xy));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resolveMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= pc.viewSize)) return;
    float2 tracePixel = TraceCoordinate(float2(id.xy));
    output[id.xy] = SpatialEstimate(id.xy, int2(floor(tracePixel + 0.5)), pc.scale == 1 ? 2u : 1u, tracePixel);
}
