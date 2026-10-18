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

struct PushConstants
{
    float exposure;
    uint tonemapOperator;
    float2 invExtent;
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
    float3 hdr = gHdrColor.Sample(uv).rgb * pushData.exposure;

    float3 ldr;
    if (pushData.tonemapOperator == kTonemapReinhard)
        ldr = TonemapReinhard(hdr);
    else if (pushData.tonemapOperator == kTonemapAces)
        ldr = TonemapAces(hdr);
    else
        ldr = saturate(hdr);

    return float4(ldr, 1.0);
}
