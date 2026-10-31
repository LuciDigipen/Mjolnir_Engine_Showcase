struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD0; };
[[vk::binding(0)]] Sampler2D<float4> scene;
[shader("vertex")]
Varying vertexMain(uint vertex : SV_VertexID)
{
    Varying o;
    o.uv = float2((vertex << 1) & 2, vertex & 2);
    o.position = float4(o.uv * 2 - 1, 0, 1);
    return o;
}
[shader("fragment")]
float4 fragmentMain(Varying i) : SV_Target { return float4(scene.Sample(i.uv).rgb, 1); }
