
struct Particle
{
    float4 positionAge;
    float4 velocityLifetime;
    float4 sizeRotation;
    float4 color;
    uint4 identity;
};
struct Parameters
{
    uint4 range;
    float4 time;
    float4 velocity;
    float4 color;
    float4 emission;
};
struct Counts
{
    uint vertices, alive, firstVertex, firstInstance;
    uint spawned, dropped, generation, reserved;
};
[[vk::binding(0)]] Sampler2D<float4> scene;
[[vk::binding(1)]] Sampler2D<float4> depth;
[[vk::binding(2)]] RWStructuredBuffer<Particle> particles;
[[vk::binding(3)]] RWStructuredBuffer<Counts> counts;
[[vk::binding(4)]] StructuredBuffer<Parameters> inputs;
[[vk::binding(5)]] StructuredBuffer<Particle> particleRead;

// One invocation owns one disjoint range. Compaction preserves spawn order,
// free slots are the suffix, no atomics or unbounded allocation are needed.
[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    uint instance = id.x;
    if (instance >= 64) return;
    Parameters p = inputs[instance];
    Counts old = counts[instance];
    uint alive = 0;
    uint previous = old.generation == p.range.z && p.time.w == 0 ? min(old.alive, p.range.y) : 0;
    for (uint i = 0; i < previous; ++i)
    {
        Particle a = particles[p.range.x + i];
        a.positionAge.w += p.time.x;
        a.positionAge.xyz += a.velocityLifetime.xyz * p.time.x;
        if (a.positionAge.w < a.velocityLifetime.w && all(abs(a.positionAge.xyz) <= 1024.0))
            particles[p.range.x + alive++] = a;
    }
    uint spawned = min(p.range.w, p.range.y - alive);
    uint spawnBase = old.generation == p.range.z && p.time.w == 0 ? old.reserved : 0;
    for (uint i = 0; i < spawned; ++i)
    {
        Particle a;
        a.positionAge = float4(p.emission.xyz, 0);
        a.velocityLifetime = float4(p.velocity.xyz, p.time.y);
        a.sizeRotation = float4(p.time.z, p.time.z, 0, 0);
        a.color = p.color;
        a.identity = uint4(instance, p.range.z, spawnBase + i, 0);
        particles[p.range.x + alive + i] = a;
    }
    Counts c;
    c.vertices = 6;
    c.alive = alive + spawned;
    c.firstVertex = 0;
    c.firstInstance = 0;
    c.spawned = spawned;
    c.dropped = p.range.w - spawned;
    c.generation = p.range.z;
    c.reserved = spawnBase + spawned;
    counts[instance] = c;
}

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


