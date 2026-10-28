/**
 * @file particleProbe.slang
 * @author sumin.park
 * @brief Static world-space quad and trail composition, no simulation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
struct VSOutput { float4 position : SV_Position; };

[shader("vertex")]
VSOutput vertexMain(uint id : SV_VertexID)
{
    float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    VSOutput result;
    result.position = float4(positions[id], 0, 1);
    return result;
}

[[vk::binding(0, 0)]] Sampler2D<float4> gColor;
[[vk::binding(1, 0)]] Sampler2D<float> gDepth;
struct ProbeData
{
    float4x4 invViewProj;
    float4 anchor;
    float4 rect;
    float4 sampling;
    float4 depthJitter;
};
[[vk::push_constant]] ProbeData probe;

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * probe.sampling.xy;
    float4 color = gColor.Sample(uv);
    if (probe.anchor.w == 0) return color;
    const float2 ndc = (input.position.xy - probe.rect.xy) / probe.rect.zw * 2 - 1;
    const float4 nearPoint = mul(probe.invViewProj, float4(ndc, 1, 1));
    const float3 ray = normalize(nearPoint.xyz / nearPoint.w);
    // Resolve color and probe at pixel centers. Nearest jitter-compensated depth
    // remains an approximation at silhouettes, no probe color enters RR guides.
    const float depth = gDepth.Sample((input.position.xy - probe.depthJitter.xy) * probe.sampling.xy);
    float opaqueDistance = 1e30;
    if (probe.sampling.z != 0)
        opaqueDistance = depth;
    else if (depth > 0)
    {
        const float4 point = mul(probe.invViewProj, float4(ndc, depth, 1));
        opaqueDistance = length(point.xyz / point.w);
    }
    const float4 farPoint = mul(probe.invViewProj, float4(ndc, 0, 1));
    if (abs(farPoint.w) > 1e-8)
        opaqueDistance = min(opaqueDistance, length(farPoint.xyz / farPoint.w));
    // Back-to-front world XY planes, from either side of the fixture.
    for (int layer = 0; layer < 2; ++layer)
    {
        const int shape = ray.z < 0 ? layer : 1 - layer;
        const float plane = probe.anchor.z + (shape == 0 ? -0.3 : 0.0);
        if (abs(ray.z) < 1e-6) continue;
        const float distance = plane / ray.z;
        if (distance <= length(nearPoint.xyz / nearPoint.w) || distance >= opaqueDistance) continue;
        const float2 p = ray.xy * distance - probe.anchor.xy;
        const float2 halfSize = shape == 0 ? float2(3.0, 0.18) : float2(1.4, 1.4);
        const float2 center = shape == 0 ? float2(0, 0.8) : float2(-0.8, 0);
        const float2 edge = halfSize - abs(p - center);
        const float aa = max(fwidth(p.x), fwidth(p.y));
        const float coverage = saturate(min(edge.x, edge.y) / max(aa, 1e-5));
        const float alpha = coverage * (shape == 0 ? 0.65 : 0.5);
        const float3 tint = shape == 0 ? float3(0.2, 1.3, 0.35) : float3(1.6, 0.2, 0.12);
        color.rgb = tint * alpha + color.rgb * (1 - alpha);
    }
    return color;
}
