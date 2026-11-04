#include "AtmosphereCommon.slang"

struct VSOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

[[vk::binding(0, 0)]]
cbuffer CameraData
{
    float4x4 viewProj;
    float4 lightDirWS;
    float4 lightColor;
    float4 ambientColor;
    float4x4 invViewProj;
    float4 cameraPos; // the origin of camera-relative render space
    float4 fogColor;
    float4 fogRange;       // x = start distance, y = end distance (unused here, kept for layout parity)
    float4 cameraWorldPos; // absolute camera position, for the altitude only
    float4 clipPlanes;
    float4 atmosphereScattering;
    float4 atmosphereRadii;
    float4 atmosphereMisc;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };

    float2 pos = positions[vertexId];

    VSOutput output;
    // Depth 0 is the far end of the reverse-Z range, where the clear value is.
    output.position = float4(pos, 0.0, 1.0);
    output.ndc = pos;
    return output;
}

#ifdef MIR_SSGI_SURFACE
#include "RasterSurfaceCommon.slang"
#endif

[shader("fragment")]
#ifdef MIR_SSGI_SURFACE
RasterSurfaceOutput fragmentMain(VSOutput input)
#else
float4 fragmentMain(VSOutput input) : SV_Target
#endif
{
    AtmosphereParams params;
    params.rayleighCoeff = atmosphereScattering.xyz;
    params.mieCoeff = atmosphereScattering.w;
    params.planetRadius = atmosphereRadii.x;
    params.atmosphereRadius = atmosphereRadii.y;
    params.rayleighScaleHeight = atmosphereRadii.z;
    params.mieScaleHeight = atmosphereRadii.w;
    params.mieG = atmosphereMisc.x;
    params.sunIntensity = atmosphereMisc.y;

    // Depth 1 is the near plane, a finite point even with no far plane.
    float4 onNearPlane = mul(invViewProj, float4(input.ndc, 1.0, 1.0));
    float3 worldPos = onNearPlane.xyz / onNearPlane.w;
    float3 rayDir = normalize(worldPos - cameraPos.xyz);

    // Camera position relative to the planet's center
    float3 rayOrigin = float3(cameraWorldPos.x, params.planetRadius + cameraWorldPos.y, cameraWorldPos.z);

    float3 color = Atmosphere(rayDir, rayOrigin, normalize(lightDirWS.xyz), params);
    const float4 hdrColor = float4(color, 1.0);
#ifdef MIR_SSGI_SURFACE
    RasterSurfaceOutput output;
    output.color = hdrColor;
    output.normalKind = float4(0.0);
    output.albedo = float4(0.0);
    output.diffuseRadiance = float4(0.0);
    output.directLighting = float4(0.0);
    return output;
#else
    return hdrColor;
#endif
}
