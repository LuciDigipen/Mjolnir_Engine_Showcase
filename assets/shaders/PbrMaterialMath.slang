#ifndef MIR_PBR_MATERIAL_MATH
#define MIR_PBR_MATERIAL_MATH

float3 PbrDielectricFresnel(float3 f0, float f90, float VdotH)
{
    const float t = max(0.0f, min(1.0f, 1.0f - VdotH));
    const float t2 = t * t;
    return f0 + (f90 - f0) * (t2 * t2 * t);
}

float3 PbrDiffuseColor(float3 albedo, float metallic, float3 dielectricFresnel)
{
    const float weight = 1.0f - max(dielectricFresnel.x, max(dielectricFresnel.y, dielectricFresnel.z));
    return albedo * ((1.0f - metallic) * weight);
}

float3 PbrMixedF0(float3 dielectricF0, float3 albedo, float metallic)
{
    return dielectricF0 * (1.0f - metallic) + albedo * metallic;
}

#endif
