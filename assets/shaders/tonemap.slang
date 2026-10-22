#include "FilmGrain.slang"

struct VSOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertexMain(uint vertexId: SV_VertexID)
{
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(3.0, -1.0),
        float2(-1.0, 3.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

[[vk::binding(0, 0)]]
Sampler2D<float4> gHdrColor;

[[vk::binding(1, 0)]]
Sampler2D<float4> gBloom;

struct PushConstants
{
    float exposure;
    uint tonemapOperator;
    float2 invExtent;
    float bloomIntensity;
    float grain;
    float contrast;
    float saturation;
    float vignetteStrength;
    float vignetteRadius;
    float grainSeed;
}

[[vk::push_constant]]
PushConstants pushData;

static const uint kTonemapNone = 0;
static const uint kTonemapReinhard = 1;
static const uint kTonemapAces = 2;

float3 TonemapReinhard(float3 color)
{
    return color / (color + 1.0);
}

// Narkowicz 2015's fit of the ACES reference tonemapping curve.
float3 TonemapAces(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    float2 uv = input.position.xy * pushData.invExtent;
    float3 hdr = gHdrColor.Sample(uv).rgb + gBloom.Sample(uv).rgb * pushData.bloomIntensity;
    hdr *= pushData.exposure;

    float3 ldr;
    if (pushData.tonemapOperator == kTonemapReinhard)
        ldr = TonemapReinhard(hdr);
    else if (pushData.tonemapOperator == kTonemapAces)
        ldr = TonemapAces(hdr);
    else
        ldr = saturate(hdr);

    // Film look, applied after tonemap: contrast -> saturation -> vignette -> grain.
    ldr = (ldr - 0.5) * pushData.contrast + 0.5;

    float luma = dot(ldr, float3(0.2126, 0.7152, 0.0722));
    ldr = lerp(float3(luma, luma, luma), ldr, pushData.saturation);

    float2 vig2 = uv * (1.0 - uv);
    float vig = saturate(pow(vig2.x * vig2.y * 16.0, pushData.vignetteRadius));
    ldr *= lerp(1.0, vig, pushData.vignetteStrength);

    // grain is 0 here while FXAA runs - it would smear the noise, so fxaa.slang applies it instead.
    ldr = ApplyFilmGrain(ldr, input.position.xy, pushData.grain, pushData.grainSeed);

    return float4(saturate(ldr), 1.0);
}
