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
Sampler2D<float4> gVelocity;

[[vk::binding(1, 0)]]
Sampler2D<float4> gColor;

struct MotionBlurPushConstants
{
    float shutterStrength;
    uint sampleCount;
    float2 invExtent;
}

[[vk::push_constant]]
MotionBlurPushConstants pushData;

[shader("fragment")]
float4 compositeMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * pushData.invExtent;

    // A single NEAREST tap at the pixel center: the exact vector this pixel
    // resolved to, not one blended with a neighbor across a silhouette.
    const float2 velocity = gVelocity.SampleLevel(uv, 0).xy * pushData.shutterStrength;

    const uint count = max(pushData.sampleCount, 1u);
    float3 colorSum = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < count; i++)
    {
        // Spans -0.5..+0.5 of the vector, centered on this frame (count == 1
        // samples the center only, i.e. no blur).
        const float t = count > 1 ? (float(i) / float(count - 1) - 0.5) : 0.0;
        colorSum += gColor.SampleLevel(uv + velocity * t, 0).rgb;
    }

    return float4(colorSum / float(count), 1.0);
}
