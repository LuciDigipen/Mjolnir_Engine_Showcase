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
Sampler2D<float4> gSource;

float3 SampleBox4(float2 uv, float2 offset)
{
    float3 s0 = gSource.Sample(uv + float2(-offset.x, -offset.y)).rgb;
    float3 s1 = gSource.Sample(uv + float2(offset.x, -offset.y)).rgb;
    float3 s2 = gSource.Sample(uv + float2(-offset.x, offset.y)).rgb;
    float3 s3 = gSource.Sample(uv + float2(offset.x, offset.y)).rgb;
    return (s0 + s1 + s2 + s3) * 0.25;
}

struct ExtractPushConstants
{
    float threshold;
    float knee;
    float2 invSrcExtent;
    float2 invTargetExtent;
}

[[vk::push_constant]]
ExtractPushConstants extractData;

[shader("fragment")]
float4 brightpassMain(VSOutput input) : SV_Target
{
    float2 uv = input.position.xy * extractData.invTargetExtent;
    float3 color = SampleBox4(uv, extractData.invSrcExtent * 0.5);

    const float knee = max(extractData.knee, 1e-5);
    const float brightness = max(color.r, max(color.g, color.b));

    float soft = clamp(brightness - extractData.threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    const float contribution = max(soft, brightness - extractData.threshold);
    const float scale = contribution / max(brightness, 1e-5);

    return float4(color * scale, 1.0);
}

struct DownsamplePushConstants
{
    float2 invSrcExtent;
    float2 invTargetExtent;
}

[[vk::push_constant]]
DownsamplePushConstants downsampleData;

[shader("fragment")]
float4 downsampleMain(VSOutput input) : SV_Target
{
    float2 uv = input.position.xy * downsampleData.invTargetExtent;
    return float4(SampleBox4(uv, downsampleData.invSrcExtent * 0.5), 1.0);
}

struct UpsamplePushConstants
{
    float2 invSrcExtent;
    float2 invTargetExtent;
    float radius;
}

[[vk::push_constant]]
UpsamplePushConstants upsampleData;

[shader("fragment")]
float4 upsampleMain(VSOutput input) : SV_Target
{
    float2 uv = input.position.xy * upsampleData.invTargetExtent;
    return float4(SampleBox4(uv, upsampleData.invSrcExtent * max(upsampleData.radius, 0.0)), 1.0);
}
