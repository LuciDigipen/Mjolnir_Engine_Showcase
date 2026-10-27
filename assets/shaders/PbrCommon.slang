// Lambert diffuse + GGX/Smith/Schlick specular shared by raster and RT.
// The existing lighting does not divide by pi; lightColor already carries that
// convention, so PBR keeps it to preserve the old brightness.

#ifndef MIR_PBR_COMMON
#define MIR_PBR_COMMON

#include "LightingCommon.slang"

static const float kPi = 3.14159265;
static const float kMinRoughness = 0.045;
static const float kMinNdotV = 1e-4;

float DistributionGgx(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    const float r = roughness + 1.0;
    const float k = r * r / 8.0;
    const float gv = NdotV / (NdotV * (1.0 - k) + k);
    const float gl = NdotL / (NdotL * (1.0 - k) + k);
    return gv * gl;
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 SpecularBrdf(float3 f0, float roughness, float3 N, float3 V, float3 L)
{
    const float NdotL = saturate(dot(N, L));
    const float NdotV = max(saturate(dot(N, V)), kMinNdotV);
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    const float3 H = normalize(L + V);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));
    const float3 F = FresnelSchlick(VdotH, f0);
    const float D = DistributionGgx(NdotH, max(roughness, kMinRoughness));
    const float G = GeometrySmith(NdotV, NdotL, max(roughness, kMinRoughness));
    return D * G * F / (4.0 * NdotV * NdotL);
}

// Direct lighting keeps the existing pi-scaled lightColor convention. D
// already contains 1/pi, so the specular term is scaled back by pi; both
// diffuse and specular receive the saturated cosine exactly once.
float3 EvaluateDirectPbr(float3 diffuseAlbedo, float3 f0, float roughness, float3 N, float3 V, float3 L,
    float3 lightColor)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    const float3 specular = SpecularBrdf(f0, roughness, N, V, L);
    return (diffuseAlbedo + kPi * specular) * lightColor * NdotL;
}

// NVIDIA DLSS Ray Reconstruction specular-albedo guide approximation:
// F0 + (max(1 - roughness^2, F0) - F0) * (1 - NoV)^5.
// The caller passes squared roughness; gNormal.w keeps linear roughness.
float3 SpecularAlbedoGuide(float3 f0, float squaredRoughness, float NoV)
{
    const float3 dielectric = float3(1.0 - squaredRoughness, 1.0 - squaredRoughness,
        1.0 - squaredRoughness);
    const float3 reflectance = max(dielectric, f0);
    return saturate(f0 + (reflectance - f0) * pow(saturate(1.0 - NoV), 5.0));
}

float3 ShadePbr<V : IShadowVisibility>(V visibility, SurfaceOutput surface, float3 worldPos)
{
    const float3 N = normalize(surface.normalWS);
    const float3 V = normalize(cameraPos.xyz - worldPos);
    const float3 f0 = surface.f0.xxx;
    const float3 diffuseAlbedo = surface.albedo * (1.0 - surface.f0);

    float3 color = surface.albedo * surface.ao * ambientColor.rgb;

    const float3 L = normalize(lightDirWS.xyz);
    const float3 sun = EvaluateDirectPbr(diffuseAlbedo, f0, surface.roughness, N, V, L, lightColor.rgb);
    color += sun * visibility.Sun(L);

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
        if (atten <= 0.0)
            continue;

        const float3 direct = EvaluateDirectPbr(diffuseAlbedo, f0, surface.roughness, N, V, Lp, light.color.rgb);
        color += direct * atten * visibility.Local(light, Lp, dist);
    }

    return color + surface.emissive;
}

#endif