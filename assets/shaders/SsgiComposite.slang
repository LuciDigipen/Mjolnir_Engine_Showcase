#include "LightingCommon.slang"

struct CompositePush
{
    uint2 viewOffset;
    uint2 viewSize;
    float intensity;
};
[[vk::push_constant]] ConstantBuffer<CompositePush> pc;
[[vk::binding(0, 0)]] Texture2D<float> depthInput;
[[vk::binding(1, 0)]] Texture2D<float4> normalInput;
[[vk::binding(2, 0)]] Texture2D<float4> albedoInput;
[[vk::binding(3, 0)]] Texture2D<float4> directInput;
[[vk::binding(4, 0)]] Texture2D<float4> irradianceInput;

[shader("vertex")]
float4 vertexMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * 2 - 1, 0, 1);
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target
{
    uint2 pixel = uint2(position.xy);
    uint2 local = pixel - pc.viewOffset;
    float4 irradiance = irradianceInput.Load(int3(local, 0));
    if (abs(normalInput.Load(int3(pixel, 0)).w - 1) > 0.25 || irradiance.w <= 0 || pc.intensity <= 0)
        discard;
    float depth = depthInput.Load(int3(pixel, 0));
    float2 uv = (float2(local) + 0.5) / float2(pc.viewSize);
    float4 world = mul(invViewProj, float4(uv * 2 - 1, depth, 1));
    float3 point = world.xyz / world.w;
    if (depth <= 0 || !all(isfinite(point)) || !all(isfinite(irradiance))) discard;
    float confidence = saturate(irradiance.w) * min(pc.intensity, 1.0);
    // RGB already includes resolved confidence; do not multiply it by alpha again.
    const float3 ambientFallback = ambientColor.rgb * (1 - confidence);
    const float3 giRadiance = max(irradiance.rgb, float3(0)) * (pc.intensity / 3.14159265359);
    const float4 receiver = albedoInput.Load(int3(pixel, 0));
    // rgb is albedo*AO; a is (1-f0), so only the diffuse GI term loses the
    // specular share and the ambient fallback matches ordinary Pbr ambient.
    float3 color = directInput.Load(int3(pixel, 0)).rgb + receiver.rgb * (ambientFallback + giRadiance * receiver.a);
    return float4(clamp(ApplyFog(color, length(point - cameraPos.xyz)), -65504.0, 65504.0), 1);
}
