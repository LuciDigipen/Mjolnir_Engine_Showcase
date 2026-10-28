/**
 * @file particlePool.slang
 * @author sumin.park
 * @brief Sprite drawing for cooked GPU emitter datasets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

struct Particle
{
    float4 positionAge;
    float4 velocityLifetime;
    float4 sizeRotation;
    float4 color;
    uint4 identity;
};
[[vk::binding(0)]] Sampler2D<float4> scene;
[[vk::binding(1)]] Sampler2D<float4> depth;
[[vk::binding(5)]] StructuredBuffer<Particle> particleRead;

struct View
{
    float4x4 viewProj;
    float4 anchorStart;
    float4 right;
    float4 up;
    float4 sampling;
};
[[vk::push_constant]] View view;
struct Varying
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float distance : TEXCOORD1;
};
[shader("vertex")]
Varying vertexMain(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    Varying o;
    if (view.sampling.w != 0)
    {
        float2 uv = float2((vertex << 1) & 2, vertex & 2);
        o.position = float4(uv * 2 - 1, 0, 1);
        o.uv = uv;
        o.color = 1;
        o.distance = 0;
        return o;
    }
    const float2 corners[6] = {float2(-1,-1),float2(1,-1),float2(1,1),
        float2(-1,-1),float2(1,1),float2(-1,1)};
    Particle p = particleRead[uint(view.anchorStart.w) + instance];
    float2 corner = corners[vertex];
    float3 center = view.anchorStart.xyz + p.positionAge.xyz;
    float3 position = center + (view.right.xyz * corner.x + view.up.xyz * corner.y) * p.sizeRotation.x;
    o.position = mul(view.viewProj, float4(position, 1));
    o.uv = corner;
    o.color = p.color;
    o.color.a *= saturate(1 - p.positionAge.w / p.velocityLifetime.w);
    o.distance = length(position);
    return o;
}
[shader("fragment")]
float4 fragmentMain(Varying i) : SV_Target
{
    if (view.sampling.w != 0) return float4(scene.Sample(i.uv).rgb, 1);
    float2 uv = (i.position.xy - float2(view.right.w, view.up.w)) * view.sampling.xy;
    float d = depth.Sample(uv).r;
    if (view.sampling.z != 0 ? (d > 0 && i.distance > d) : i.position.z < d) discard;
    float alpha = i.color.a * (1 - smoothstep(0.65, 1.0, length(i.uv)));
    return float4(i.color.rgb * alpha, alpha);
}


