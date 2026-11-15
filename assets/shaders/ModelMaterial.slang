#ifndef MIR_MODEL_MATERIAL
#define MIR_MODEL_MATERIAL
#include "SurfaceCommon.slang"
#include "NormalMapping.slang"

// Immutable record: four float4 factors followed by ten uint4 texture bindings.
float4 SampleModelChannel(uint recordOffset, uint channel, float2 uv, MaterialContext context, float4 fallback)
{
    const uint4 binding = gSurfaceRecords.Load4(recordOffset + 64 + 16 * channel);
    if (binding.z == 0)
        return fallback;
    TextureRef ref;
    ref.image = binding.x;
    ref.sampler = binding.y;
    return context.Sample(ref, uv);
}

SurfaceOutput EvaluateModelMaterial(uint recordOffset, float4 baseColor, float3 normal, float4 tangent, float2 uv,
                                    bool backFace, MaterialContext context)
{
    const float4 emission = asfloat(gSurfaceRecords.Load4(recordOffset + 16));
    const float4 specular = asfloat(gSurfaceRecords.Load4(recordOffset + 32));
    const float4 coat = asfloat(gSurfaceRecords.Load4(recordOffset + 48));
    SurfaceOutput s = (SurfaceOutput)0;
    const float4 p = asfloat(gSurfaceRecords.Load4(recordOffset));
    const float4 mr = SampleModelChannel(recordOffset, 2, uv, context, float4(1));
    s.metallic = saturate(p.x * mr.b);
    s.roughness = saturate(p.y * mr.g);
    s.albedo = baseColor.rgb;
    s.normalWS = SafeNormalize(normal, float3(0, 1, 0));
    s.clearcoatNormalWS = s.normalWS;
    if (gSurfaceRecords.Load(recordOffset + 64 + 16 + 8) != 0 && p.z != 0)
        s.normalWS = ApplyTangentNormal(
            normal, tangent, SampleModelChannel(recordOffset, 1, uv, context, float4(.5, .5, 1, 1)).xyz, p.z);
    s.ao = lerp(1.0, SampleModelChannel(recordOffset, 3, uv, context, float4(1)).r, p.w);
    s.emissive = emission.rgb * SampleModelChannel(recordOffset, 4, uv, context, float4(1)).rgb;
    s.unlit = emission.w > 0;
    const float weight = saturate(specular.w * SampleModelChannel(recordOffset, 5, uv, context, float4(1)).a);
    const float3 specColor = specular.rgb * SampleModelChannel(recordOffset, 6, uv, context, float4(1)).rgb;
    const float ior = coat.x;
    const float dielectric = pow((ior - 1) / (ior + 1), 2);
    s.dielectricF0 = min(dielectric * specColor, float3(1)) * weight;
    s.dielectricF90 = weight;
    s.f0 = lerp(s.dielectricF0, s.albedo, s.metallic);
    s.f90 = lerp(float3(weight), float3(1), s.metallic);
    s.clearcoat = saturate(coat.y * SampleModelChannel(recordOffset, 7, uv, context, float4(1)).r);
    s.clearcoatRoughness = saturate(coat.z * SampleModelChannel(recordOffset, 8, uv, context, float4(1)).g);
    if (gSurfaceRecords.Load(recordOffset + 64 + 16 * 9 + 8) != 0 && coat.w != 0)
        s.clearcoatNormalWS = ApplyTangentNormal(
            normal, tangent, SampleModelChannel(recordOffset, 9, uv, context, float4(.5, .5, 1, 1)).xyz, coat.w);
    if (backFace)
    {
        s.normalWS = -s.normalWS;
        s.clearcoatNormalWS = -s.clearcoatNormalWS;
    }
    return s;
}
#endif
