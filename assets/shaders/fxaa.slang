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
Sampler2D<float4> gColor;

struct FxaaPushConstants
{
    float2 invExtent;
}

[[vk::push_constant]]
FxaaPushConstants pushData;

// Below this local contrast, an edge is too subtle to matter - skip AA entirely.
static const float kEdgeThresholdMin = 0.0312;
// Relative-to-brightness contrast that flags a real edge worth smoothing.
static const float kEdgeThreshold = 0.125;
static const float kSubpixelQuality = 0.75;

float Luma(float3 rgb)
{
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * pushData.invExtent;
    const float3 colorCenter = gColor.SampleLevel(uv, 0).rgb;

    const float lumaCenter = Luma(colorCenter);
    const float lumaDown = Luma(gColor.SampleLevel(uv + float2(0.0, pushData.invExtent.y), 0).rgb);
    const float lumaUp = Luma(gColor.SampleLevel(uv - float2(0.0, pushData.invExtent.y), 0).rgb);
    const float lumaLeft = Luma(gColor.SampleLevel(uv - float2(pushData.invExtent.x, 0.0), 0).rgb);
    const float lumaRight = Luma(gColor.SampleLevel(uv + float2(pushData.invExtent.x, 0.0), 0).rgb);

    const float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    const float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    const float lumaRange = lumaMax - lumaMin;

    // Flat region or sub-threshold contrast: cheapest exit, color untouched.
    if (lumaRange < max(kEdgeThresholdMin, lumaMax * kEdgeThreshold))
        return float4(colorCenter, 1.0);

    // Diagonal taps, for edge orientation.
    const float lumaDownLeft = Luma(gColor.SampleLevel(uv + float2(-pushData.invExtent.x, pushData.invExtent.y), 0).rgb);
    const float lumaUpRight = Luma(gColor.SampleLevel(uv + float2(pushData.invExtent.x, -pushData.invExtent.y), 0).rgb);
    const float lumaUpLeft = Luma(gColor.SampleLevel(uv + float2(-pushData.invExtent.x, -pushData.invExtent.y), 0).rgb);
    const float lumaDownRight = Luma(gColor.SampleLevel(uv + float2(pushData.invExtent.x, pushData.invExtent.y), 0).rgb);

    const float edgeHorizontal = abs(lumaUpLeft + lumaUpRight - 2.0 * lumaUp) +
        2.0 * abs(lumaLeft + lumaRight - 2.0 * lumaCenter) +
        abs(lumaDownLeft + lumaDownRight - 2.0 * lumaDown);
    const float edgeVertical = abs(lumaUpLeft + lumaDownLeft - 2.0 * lumaLeft) +
        2.0 * abs(lumaUp + lumaDown - 2.0 * lumaCenter) +
        abs(lumaUpRight + lumaDownRight - 2.0 * lumaRight);
    const bool isHorizontal = edgeHorizontal >= edgeVertical;

    // The two neighbors straddling the edge, perpendicular to it - whichever
    // one differs more from the center decides the step's sign and the local
    // average this pixel's subpixel blend factor is measured against.
    const float luma1 = isHorizontal ? lumaDown : lumaLeft;
    const float luma2 = isHorizontal ? lumaUp : lumaRight;
    const float gradient1 = luma1 - lumaCenter;
    const float gradient2 = luma2 - lumaCenter;
    const bool is1Steepest = abs(gradient1) >= abs(gradient2);

    float stepLength = isHorizontal ? pushData.invExtent.y : pushData.invExtent.x;
    float lumaLocalAverage;
    if (is1Steepest)
    {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    }
    else
    {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    const float subpixelOffset = clamp(abs(lumaCenter - lumaLocalAverage) / lumaRange, 0.0, 1.0);
    const float finalOffset = (-2.0 * subpixelOffset + 3.0) * subpixelOffset * subpixelOffset * kSubpixelQuality;

    float2 finalUv = uv;
    if (isHorizontal)
        finalUv.y += stepLength * finalOffset;
    else
        finalUv.x += stepLength * finalOffset;

    return float4(gColor.SampleLevel(finalUv, 0).rgb, 1.0);
}
