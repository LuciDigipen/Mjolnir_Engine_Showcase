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

float LinearizeDepth(float depthOrDistance, uint isLinearDepth, float near, float far)
{
    if (isLinearDepth != 0)
        return depthOrDistance;
    if (far > 0.0)
        return (near * far) / max(near + depthOrDistance * (far - near), 1e-6);
    return near / max(depthOrDistance, 1e-6);
}

float ComputeCoc(float viewDist, float focusDistance, float aperture, float maxBlurTexels)
{
    const float coc = aperture * (viewDist - focusDistance) / max(viewDist, 1e-4);
    return clamp(coc * maxBlurTexels, -maxBlurTexels, maxBlurTexels);
}

float2 VogelDisk(int index, int count)
{
    const float kGoldenAngle = 2.39996323;
    const float r = sqrt((float(index) + 0.5) / float(count));
    const float theta = float(index) * kGoldenAngle;
    return r * float2(cos(theta), sin(theta));
}

[[vk::binding(0, 0)]]
Sampler2D<float4> gColor;

[[vk::binding(1, 0)]]
Sampler2D<float> gDepth;

struct DepthOfFieldCocPushConstants
{
    float focusDistance;
    float aperture;
    float maxBlurTexels;
    float near;
    float far;
    uint isLinearDepth;
    float2 invSrcExtent;
    float2 invTargetExtent;
}

[[vk::push_constant]]
DepthOfFieldCocPushConstants cocData;

[shader("fragment")]
float4 cocMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * cocData.invTargetExtent;
    const float3 color = gColor.Sample(uv).rgb;
    const float depthOrDistance = gDepth.Sample(uv);

    const float viewDist = LinearizeDepth(depthOrDistance, cocData.isLinearDepth, cocData.near, cocData.far);
    const float coc = ComputeCoc(viewDist, cocData.focusDistance, cocData.aperture, cocData.maxBlurTexels);

    return float4(color, coc);
}

struct DepthOfFieldBlurPushConstants
{
    float maxBlurTexels;
    float2 invTargetExtent;
}

[[vk::binding(0, 0)]]
Sampler2D<float4> gCocColor;

[[vk::push_constant]]
DepthOfFieldBlurPushConstants blurData;

static const int kTapCount = 12;

[shader("fragment")]
float4 nearBlurMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * blurData.invTargetExtent;

    float3 colorSum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;

    for (int i = 0; i < kTapCount; ++i)
    {
        const float2 offset = VogelDisk(i, kTapCount) * blurData.maxBlurTexels * blurData.invTargetExtent;
        const float4 tap = gCocColor.Sample(uv + offset);
        const float weight = saturate(max(-tap.a, 0.0) / max(blurData.maxBlurTexels, 1e-4));
        colorSum += tap.rgb * weight;
        weightSum += weight;
    }

    const float4 center = gCocColor.Sample(uv);
    const float3 color = weightSum > 1e-4 ? colorSum / weightSum : center.rgb;
    const float coverage = saturate(weightSum / float(kTapCount));
    return float4(color, coverage);
}

[shader("fragment")]
float4 farBlurMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * blurData.invTargetExtent;
    const float4 center = gCocColor.Sample(uv);
    const float centerFar = max(center.a, 0.0);

    float3 colorSum = center.rgb;
    float weightSum = 1.0;

    for (int i = 0; i < kTapCount; ++i)
    {
        const float2 diskOffset = VogelDisk(i, kTapCount);
        const float2 offset = diskOffset * centerFar * blurData.invTargetExtent;
        const float4 tap = gCocColor.Sample(uv + offset);
        const float tapFar = max(tap.a, 0.0);

        const float reach = length(diskOffset) * centerFar;
        const float weight = reach > 1e-4 ? saturate(tapFar / reach) : 1.0;

        colorSum += tap.rgb * weight;
        weightSum += weight;
    }

    return float4(colorSum / weightSum, 1.0);
}

[[vk::binding(0, 0)]]
Sampler2D<float4> gCompositeColor;

[[vk::binding(1, 0)]]
Sampler2D<float> gCompositeDepth;

[[vk::binding(2, 0)]]
Sampler2D<float4> gNearResult;

[[vk::binding(3, 0)]]
Sampler2D<float4> gFarResult;

[[vk::push_constant]]
DepthOfFieldCocPushConstants compositeData;

[shader("fragment")]
float4 compositeMain(VSOutput input) : SV_Target
{
    const float2 uv = input.position.xy * compositeData.invTargetExtent;

    const float3 sharp = gCompositeColor.Sample(uv).rgb;
    const float depthOrDistance = gCompositeDepth.Sample(uv);
    const float viewDist =
        LinearizeDepth(depthOrDistance, compositeData.isLinearDepth, compositeData.near, compositeData.far);
    const float coc =
        ComputeCoc(viewDist, compositeData.focusDistance, compositeData.aperture, compositeData.maxBlurTexels);

    const float farT = saturate(coc / max(compositeData.maxBlurTexels, 1e-4));
    const float3 far = gFarResult.Sample(uv).rgb;
    const float3 base = lerp(sharp, far, farT);

    const float4 near = gNearResult.Sample(uv);
    const float3 result = lerp(base, near.rgb, near.a);

    return float4(result, 1.0);
}
