// Lighting shared by both pipelines; only shadow visibility differs.

#ifndef MIR_LIGHTING_COMMON
#define MIR_LIGHTING_COMMON

// CameraData's layout must match the CameraData struct in CameraBuffer.cpp exactly.
[[vk::binding(0, 1)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS; // xyz = normalized direction *toward* the Sun
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin of camera-relative render space
    float4 fogColor;  // rgb = fog color, a = density (0 = disabled)
    float4 fogRange;  // x = start distance, y = end distance
    float4 cameraWorldPos; // absolute camera position, for the atmosphere only
    float4 clipPlanes;     // x = near, y = far, or 0 for no far plane
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
    float4 lightingMeta; // x = this view's count in gLights, y = its first index, z = 1 if the Sun casts a
                         // shadow map, w = this view's first spot shadow matrix
    float4x4 prevViewProj;
    float4x4 sunShadowViewProj;
    float4 jitterAndPad; // xy = this frame's sub-pixel jitter in pixels, ray tracing + Dlss only
};

// GpuLight's layout must match the GpuLight struct in CameraBuffer.cpp exactly.
struct GpuLight
{
    float4 positionAndRange;    // xyz = position relative to this view's camera, w = range
    float4 directionAndSpotCos; // xyz = normalized aim direction, w = cos(half-angle); -1 = no cone
    float4 color;               // rgb = color * intensity, w = raster shadow slot; -1 = none, -2 = Area
};

[[vk::binding(1, 1)]]
StructuredBuffer<GpuLight> gLights;

static const float kAreaLightSlot = -2.0;

static const float3 kDefaultAlbedo = float3(0.5, 0.5, 0.5);
static const float kSpecularStrength = 0.04;
static const float kShininess = 32.0;

interface IShadowVisibility
{
    float Sun(float3 L);
    float Local(GpuLight light, float3 Lp, float dist);
}

// Visibility is only queried where a light contributes, so RT skips rays it doesn't need.
float3 ShadeLit<V : IShadowVisibility>(V visibility, float3 albedo, float3 N, float3 worldPos)
{
    const float3 L = normalize(lightDirWS.xyz);
    const float3 viewDir = normalize(cameraPos.xyz - worldPos);
    const float3 H = normalize(L + viewDir);

    float3 color = albedo * ambientColor.rgb;

    const float diffuse = max(dot(N, L), 0.0);
    if (diffuse > 0.0)
    {
        const float specular = pow(max(dot(N, H), 0.0), kShininess);
        color += (albedo * lightColor.rgb * diffuse + lightColor.rgb * kSpecularStrength * specular) *
                 visibility.Sun(L);
    }

    const int firstLight = int(lightingMeta.y);
    const int lightCount = int(lightingMeta.x);
    for (int i = 0; i < lightCount; i++)
    {
        const GpuLight light = gLights[firstLight + i];
        const float3 toLight = light.positionAndRange.xyz - worldPos;
        const float dist = length(toLight);
        const float range = light.positionAndRange.w;
        const float3 Lp = toLight / max(dist, 1e-5);

        float atten = clamp(1.0 - dist / max(range, 1e-5), 0.0, 1.0);
        const float cosAngle = dot(-Lp, light.directionAndSpotCos.xyz);
        atten *= (cosAngle >= light.directionAndSpotCos.w) ? 1.0 : 0.0;

        const float diffuseP = max(dot(N, Lp), 0.0);
        if (atten <= 0.0 || diffuseP <= 0.0)
            continue;

        color += albedo * light.color.rgb * atten * diffuseP * visibility.Local(light, Lp, dist);
    }

    return color;
}

float3 ApplyFog(float3 color, float dist)
{
    const float clampedDist = clamp(dist, fogRange.x, fogRange.y);
    const float fogFactor = clamp(1.0 - exp(-fogColor.a * (clampedDist - fogRange.x)), 0.0, 1.0);
    return lerp(color, fogColor.rgb, fogFactor);
}

#endif