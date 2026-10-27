// A second mesh material type for the RT hit-group registration fixture.
struct DiagnosticRedMaterial : IMaterial
{
    float4 color;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        SurfaceOutput output;
        output.albedo = color.rgb * input.vertexColor.rgb;
        output.normalWS = normalize(input.normalWS);
        output.roughness = 0.7;
        output.f0 = 0.04;
        output.ao = 1.0;
        output.height = 0.0;
        output.emissive = float3(0.0, 0.0, 0.0);
        return output;
    }
};
