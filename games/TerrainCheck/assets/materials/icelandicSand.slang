/**
 * @file icelandicSand.slang
 * @author Jackey Cen
 * @brief Packed Icelandic sand surface material.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
// The first terrain material. Terrain UVs are terrain-local meters; the
// material's physical tile size scales them and the sampled channels come from
// the three packed textures produced by the material cook. A tileable macro
// texture breaks up the repetition, and the packed displacement channel drives
// bounded parallax before the surface is sampled.

struct IcelandicSandMaterial : IMaterial
{
    float tileSizeMeters;
    float4 albedoTint;
    float roughnessScale;
    float normalScale;
    bool normalYUp;
    float macroPeriodMeters;
    float macroStrength;
    float macroHueShiftDegrees;
    float macroRoughnessContrast;
    bool parallaxEnabled;
    bool parallaxRtEnabled;
    float parallaxHeightMeters;
    float parallaxReference;
    float parallaxDistanceMeters;
    uint parallaxSteps;
    float parallaxFadeStartMeters;
    float parallaxFadeEndMeters;
    TextureRef albedo;
    TextureRef normal;
    TextureRef surface;
    TextureRef macro;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float invTile = 1.0 / max(tileSizeMeters, 0.001);
        float2 uv = input.uv0 * invTile;
        context.uvDx *= invTile;
        context.uvDy *= invTile;
        context.uvDensity *= invTile;

        if (parallaxEnabled)
            uv = SurfaceParallaxUv(input, context, surface, uv, tileSizeMeters, parallaxHeightMeters,
                parallaxReference, parallaxDistanceMeters, parallaxFadeStartMeters, parallaxFadeEndMeters,
                parallaxSteps, parallaxRtEnabled);

        float4 albedo = context.Sample(albedo, uv) * albedoTint;
        const float4 normal = context.Sample(normal, uv);
        const float4 surface = context.Sample(surface, uv);
        float roughness = saturate(surface.r * roughnessScale);

        const float strength = saturate(macroStrength);
        if (strength > 0.0 && macroPeriodMeters > 0.0)
        {
            const MaterialContext macroContext = SurfaceMacroContext(context, tileSizeMeters, macroPeriodMeters);
            const float3 macro = macroContext.Sample(macro, input.uv0 / macroPeriodMeters).rgb;
            const float variation = macro.r * 2.0 - 1.0;
            albedo.rgb *= 1.0 + strength * variation;
            albedo.rgb = SurfaceHueShift(albedo.rgb, (macro.g * 2.0 - 1.0) * macroHueShiftDegrees);
            roughness = saturate(roughness + strength * macroRoughnessContrast * (macro.b * 2.0 - 1.0));
        }

        float3 tangentNormal = normal.xyz * 2.0 - 1.0;
        tangentNormal.xy *= normalScale;
        tangentNormal.y *= normalYUp ? -1.0 : 1.0;
        tangentNormal = normalize(tangentNormal);
        const float3 tangent = normalize(input.tangentWS);
        const float3 bitangent = input.bitangentSign * cross(input.normalWS, tangent);

        SurfaceOutput output;
        output.albedo = albedo.rgb;
        output.normalWS = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                                    input.normalWS * tangentNormal.z);
        output.roughness = roughness;
        output.f0 = saturate(surface.b);
        output.ao = albedo.a;
        output.height = surface.g;
        output.emissive = float3(0.0, 0.0, 0.0);
        return output;
    }
};
