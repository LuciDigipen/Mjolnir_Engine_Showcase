// NormalKind.w: 0 = background/unsupported, 1 = lit, 2 = unlit, 3 = error.
struct RasterSurfaceOutput
{
    float4 color : SV_Target0;
    float4 normalKind : SV_Target1;
    float4 albedo : SV_Target2;
    float4 diffuseRadiance : SV_Target3;
    float4 directLighting : SV_Target4;
};

RasterSurfaceOutput MakeRasterSurface(float4 color, float3 normal, float kind, float3 albedo,
    float3 diffuseRadiance, float3 directLighting, float albedoAlpha = 1.0)
{
    RasterSurfaceOutput output;
    output.color = color;
    output.normalKind = float4(normal, kind);
    output.albedo = float4(albedo, albedoAlpha);
    output.diffuseRadiance = float4(diffuseRadiance, 1);
    output.directLighting = float4(directLighting, 1);
    return output;
}

float3 RasterSurfaceFaceNormal(float3 position)
{
    float3 normal = cross(ddx(position), ddy(position));
    normal *= rsqrt(max(dot(normal, normal), 1e-20));
    return dot(normal, position) > 0 ? -normal : normal;
}
