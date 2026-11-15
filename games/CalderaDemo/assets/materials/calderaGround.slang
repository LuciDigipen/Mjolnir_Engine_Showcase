/**
 * @file calderaGround.slang
 * @author Jackey Cen
 * @brief Packed Icelandic sand surface material.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
// The first terrain material. Terrain UVs are terrain-local meters; the
// material's physical tile size scales them and the sampled channels come from
// the three packed textures produced by the material cook.

struct CalderaGroundMaterial : IMaterial
{
    float tileSizeMeters;
    float4 albedoTint;
    float roughnessScale;
    float normalScale;
    bool normalYUp;
    TextureRef albedo;
    TextureRef normal;
    TextureRef surface;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        const float invTile = 1.0 / max(tileSizeMeters, 0.001);
        const float2 uv = input.uv0 * invTile;
        context.uvDx *= invTile;
        context.uvDy *= invTile;
        context.uvDensity *= invTile;

        const float4 albedo = context.Sample(albedo, uv) * albedoTint;
        const float4 normal = context.Sample(normal, uv);
        const float4 surface = context.Sample(surface, uv);

        SurfaceOutput output = (SurfaceOutput)0;
        output.f90 = float3(1);
        output.dielectricF90 = 1;
        output.albedo = albedo.rgb;
        output.normalWS = ApplyTangentNormal(input.normalWS, float4(input.tangentWS, input.bitangentSign), normal.xyz,
                                             normalScale, normalYUp);
        output.roughness = saturate(surface.r * roughnessScale);
        output.metallic = saturate(surface.a);
        output.dielectricF0 = float3(saturate(surface.b));
        output.f0 = lerp(output.dielectricF0, output.albedo, output.metallic);
        output.ao = albedo.a;
        output.height = surface.g;
        output.emissive = float3(0.0, 0.0, 0.0);
        return output;
    }
};
