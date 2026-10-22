// Raster IShadowVisibility, from ShadowMaps' set 3.
#include "LightingCommon.slang"

[[vk::binding(0, 3)]]
Sampler2DShadow sunShadowMap;

[[vk::binding(1, 3)]]
Sampler2DArrayShadow spotShadowMap;

[[vk::binding(2, 3)]]
StructuredBuffer<float4x4> gSpotShadowViewProj;

[[vk::binding(3, 3)]]
SamplerCubeArray<float> pointShadowMap;

// Keep in sync with VulkanRenderer::kSunShadowHalfExtent.
static const float kSunShadowHalfExtent = 25.0;
// Receiver normal offset in texels, growing toward grazing light.
static const float kShadowNormalOffsetTexels = 1.5;
static const float kShadowMinNormalOffsetTexels = 0.5;
// Point maps store distance as color, so depth bias doesn't reach them.
static const float kPointShadowBiasTexels = 1.0;
// 3x3 PCF; lower spacing gives harder edges.
static const int kPcfRadius = 1;
static const float kPcfTapSpacing = 0.5;
static const float kPcfTaps = float((2 * kPcfRadius + 1) * (2 * kPcfRadius + 1));

float SunPcf(float2 uv, float depth, float texel)
{
    float lit = 0.0;
    for (int y = -kPcfRadius; y <= kPcfRadius; y++)
        for (int x = -kPcfRadius; x <= kPcfRadius; x++)
            lit += sunShadowMap.SampleCmpLevelZero(uv + float2(x, y) * kPcfTapSpacing * texel, depth);
    return lit / kPcfTaps;
}

float SpotPcf(float2 uv, float layer, float depth, float texel)
{
    float lit = 0.0;
    for (int y = -kPcfRadius; y <= kPcfRadius; y++)
        for (int x = -kPcfRadius; x <= kPcfRadius; x++)
            lit += spotShadowMap.SampleCmpLevelZero(float3(uv + float2(x, y) * kPcfTapSpacing * texel, layer), depth);
    return lit / kPcfTaps;
}

// Face normal, toward the camera.
float3 GeometricNormal(float3 worldPos, float3 N)
{
    const float3 c = cross(ddx(worldPos), ddy(worldPos));
    const float lengthSq = dot(c, c);
    const float3 g = lengthSq > 1e-20 ? c * rsqrt(lengthSq) : N;
    return dot(g, cameraPos.xyz - worldPos) < 0.0 ? -g : g;
}

struct ShadowMapVisibility : IShadowVisibility
{
    float3 worldPos;
    float3 geoN;

    float3 Offset(float3 L, float texelWorldSize)
    {
        const float grazing = 1.0 - saturate(dot(geoN, L));
        return worldPos + geoN * texelWorldSize *
                              (kShadowMinNormalOffsetTexels + kShadowNormalOffsetTexels * grazing);
    }

    float Sun(float3 L)
    {
        if (lightingMeta.z < 0.5)
            return 1.0;

        uint width, height;
        sunShadowMap.GetDimensions(width, height);
        const float4 clip = mul(sunShadowViewProj, float4(Offset(L, 2.0 * kSunShadowHalfExtent / float(width)), 1.0));
        const float3 ndc = clip.xyz / clip.w;
        if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
            return 1.0;

        return SunPcf(ndc.xy * 0.5 + 0.5, ndc.z, 1.0 / float(width));
    }

    float Local(GpuLight light, float3 Lp, float dist)
    {
        const int slot = int(light.color.w);
        if (slot < 0)
            return 1.0;

        if (light.directionAndSpotCos.w >= 0.0) // has a cone: Spot
        {
            uint width, height, layers;
            spotShadowMap.GetDimensions(width, height, layers);
            const float spotCos = max(light.directionAndSpotCos.w, 1e-4);
            const float tanHalfAngle = sqrt(max(1.0 - spotCos * spotCos, 0.0)) / spotCos;
            const float texelWorldSize = 2.0 * dist * tanHalfAngle / float(width);

            const float4x4 vp = gSpotShadowViewProj[int(lightingMeta.w) + slot];
            const float4 clip = mul(vp, float4(Offset(Lp, texelWorldSize), 1.0));
            const float3 ndc = clip.xyz / clip.w;
            if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
                return 1.0;

            return SpotPcf(ndc.xy * 0.5 + 0.5, float(slot), ndc.z, 1.0 / float(width));
        }

        // no cone: Point
        uint width, height, layers;
        pointShadowMap.GetDimensions(width, height, layers);
        const float texelWorldSize = 2.0 * dist / float(width);
        const float3 fromLight = Offset(Lp, texelWorldSize) - light.positionAndRange.xyz;
        const float currentDist = length(fromLight);
        const float3 dir = fromLight / max(currentDist, 1e-5);
        const float3 tangent = normalize(cross(abs(dir.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), dir));
        const float3 bitangent = cross(dir, tangent);
        const float step = kPcfTapSpacing * texelWorldSize / max(currentDist, 1e-5);
        const float bias = texelWorldSize * kPointShadowBiasTexels;

        float lit = 0.0;
        for (int y = -kPcfRadius; y <= kPcfRadius; y++)
            for (int x = -kPcfRadius; x <= kPcfRadius; x++)
            {
                const float3 tapDir = dir + (tangent * float(x) + bitangent * float(y)) * step;
                const float sampledDist = pointShadowMap.SampleLevel(float4(tapDir, float(slot)), 0);
                lit += currentDist <= sampledDist + bias ? 1.0 : 0.0;
            }
        return lit / kPcfTaps;
    }
}
