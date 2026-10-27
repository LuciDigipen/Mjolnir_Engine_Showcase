// A third mesh material type; punctuation in its identity must not alias red.
struct DiagnosticBlueMaterial : IMaterial
{
    float2 uvScale;
    float4 color;

    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context)
    {
        SurfaceOutput output;
        output.albedo = color.rgb * input.vertexColor.rgb * uvScale.x;
        output.normalWS = normalize(input.normalWS);
        output.roughness = 0.3;
        output.f0 = 0.04;
        output.ao = 1.0;
        output.height = 0.0;
        output.emissive = float3(0.0, 0.0, 0.0);
        return output;
    }
};
