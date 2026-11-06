// Lambert diffuse + GGX/Smith/Schlick specular shared by raster and RT.
// The existing lighting does not divide by pi; lightColor already carries that
// convention, so PBR keeps it to preserve the old brightness.

#ifndef MIR_PBR_COMMON
#define MIR_PBR_COMMON

#include "LightingCommon.slang"

static const float kPbrPi = 3.14159265;
static const float kMinRoughness = 0.045;
static const float kMinNdotV = 1e-4;

float DistributionGgx(float NdotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float d = (1.0 - NdotH * NdotH) + NdotH * NdotH * a2;
    return a2 / max(kPbrPi * d * d, 1e-12);
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
    if (NdotL <= 0.0 || dot(N, V) <= 0.0)
        return float3(0.0, 0.0, 0.0);
    const float3 halfVector = L + V;
    if (dot(halfVector, halfVector) < 1e-12)
        return float3(0.0, 0.0, 0.0);
    const float3 H = normalize(halfVector);
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
float3 EvaluateDiffusePbr(float3 diffuseAlbedo, float3 N, float3 L, float3 lightColor)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    return diffuseAlbedo * lightColor * NdotL;
}

float3 EvaluateSpecularPbr(float3 f0, float roughness, float3 N, float3 V, float3 L, float3 lightColor)
{
    const float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    return kPbrPi * SpecularBrdf(f0, roughness, N, V, L) * lightColor * NdotL;
}

// Directional-albedo fit from Ray Tracing Gems, referenced by NVIDIA DLSS RR.
float3 SpecularAlbedoGuide(float3 f0, float squaredRoughness, float NoV)
{
    const float n = saturate(abs(NoV));
    const float n2 = n * n;
    const float n3 = n2 * n;
    const float a = saturate(squaredRoughness);
    const float a3 = a * a * a;
    const float biasNumerator = 0.99044 - 1.28514 * n + (1.29678 - 0.755907 * n) * a;
    const float biasDenominator = 1.0 + 2.92338 * n + 59.4188 * n3 +
        (20.3225 - 27.0302 * n + 222.592 * n3) * a +
        (121.563 + 626.13 * n + 316.627 * n3) * a3;
    const float scaleNumerator = 0.0365463 + 3.32707 * n + (9.0632 - 9.04756 * n) * a;
    const float scaleDenominator = 1.0 + 3.59685 * n2 - 1.36772 * n3 +
        (9.04401 - 16.3174 * n2 + 9.22949 * n3) * a +
        (5.56589 + 19.7886 * n2 - 20.2123 * n3) * a3;
    const float3 reflectance = saturate(f0);
    const float bias = max(0.0, biasNumerator / max(biasDenominator, 1e-6)) *
        saturate(reflectance.g * 50.0);
    const float scale = max(0.0, scaleNumerator / max(scaleDenominator, 1e-6));
    return saturate(reflectance * scale + bias);
}

// The components the raster MRT and the ordinary scene/RT total share.
// ambientRadiance already carries AO; it is the baseline the SSGI composite
// restores when a receiver has no GI confidence.
struct PbrLighting
{
    float3 ambientRadiance;
    float3 diffuseDirect;
    float3 specularDirect;
    float3 emissive;
};

PbrLighting EvaluatePbrLighting<V : IShadowVisibility>(V visibility, SurfaceOutput surface, float3 worldPos)
{
    const float3 N = normalize(surface.normalWS);
    const float3 V = normalize(cameraPos.xyz - worldPos);
    const float3 f0 = surface.f0.xxx;
    const float3 diffuseAlbedo = surface.albedo * (1.0 - surface.f0);

    PbrLighting lighting;
    lighting.ambientRadiance = surface.albedo * surface.ao * ambientColor.rgb;
    lighting.diffuseDirect = float3(0.0, 0.0, 0.0);
    lighting.specularDirect = float3(0.0, 0.0, 0.0);
    lighting.emissive = surface.emissive;

    const float3 L = normalize(lightDirWS.xyz);
    const float sunVisibility = visibility.Sun(L);
    lighting.diffuseDirect += EvaluateDiffusePbr(diffuseAlbedo, N, L, lightColor.rgb) * sunVisibility;
    lighting.specularDirect += EvaluateSpecularPbr(f0, surface.roughness, N, V, L, lightColor.rgb) * sunVisibility;

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

        const float localVisibility = atten * visibility.Local(light, Lp, dist);
        lighting.diffuseDirect += EvaluateDiffusePbr(diffuseAlbedo, N, Lp, light.color.rgb) * localVisibility;
        lighting.specularDirect +=
            EvaluateSpecularPbr(f0, surface.roughness, N, V, Lp, light.color.rgb) * localVisibility;
    }

    return lighting;
}

// SSGI's bounce source: every non-specular outgoing radiance, including the
// AO-scaled ambient baseline and emissive.
float3 PbrDiffuseRadiance(PbrLighting lighting)
{
    return lighting.ambientRadiance + lighting.diffuseDirect + lighting.emissive;
}

// The raster direct target; the composite adds the receiver's albedo*AO*indirect.
float3 PbrDirectLighting(PbrLighting lighting)
{
    return lighting.diffuseDirect + lighting.specularDirect + lighting.emissive;
}

float3 PbrTotalLighting(PbrLighting lighting)
{
    return lighting.ambientRadiance + lighting.diffuseDirect + lighting.specularDirect + lighting.emissive;
}

float3 ShadePbr<V : IShadowVisibility>(V visibility, SurfaceOutput surface, float3 worldPos)
{
    return PbrTotalLighting(EvaluatePbrLighting(visibility, surface, worldPos));
}

// The legacy glTF material supplies base color and opacity. Its blended
// passes use the same fixed dielectric finish in raster and ray tracing.
SurfaceOutput BlendedModelSurface(float3 albedo, float3 normal)
{
    SurfaceOutput surface;
    surface.albedo = albedo;
    surface.normalWS = normal;
    surface.roughness = 0.24;
    surface.f0 = 0.04;
    surface.ao = 1.0;
    surface.height = 0.0;
    surface.emissive = float3(0.0, 0.0, 0.0);
    return surface;
}

#endif
