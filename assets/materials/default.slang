// The default opaque mesh material. Parameters are reflected fields; the
// renderer packs them into the shared record buffer. The pass supplies
// SurfaceCommon, so this file stays a pure material definition.

struct DefaultMaterial : IMaterial
{
    float4 albedoRoughness; // rgb = albedo, a = roughness
    float4 surfaceParams;   // x = dielectric F0, y = ambient occlusion

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        SurfaceOutput output = (SurfaceOutput)0;
        output.f90 = float3(1);
        output.dielectricF90 = 1;
        // The legacy default ignores authored vertex colors.
        output.albedo = albedoRoughness.rgb;
        output.normalWS = normalize(input.normalWS);
        output.roughness = albedoRoughness.a;
        output.dielectricF0 = surfaceParams.x;
        output.f0 = output.dielectricF0;
        output.ao = surfaceParams.y;
        output.height = 0.0;
        output.emissive = float3(0.0, 0.0, 0.0);
        return output;
    }
};
