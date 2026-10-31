struct SsgiPush
{
    uint2 viewOffset;
    uint2 viewSize;
    uint2 outputSize;
    uint mipCount;
    uint mipLevel;
    uint rays;
    uint steps;
    uint seed;
    uint scale;
    float maxDistance;
    float thickness;
    float nearPlane;
    float pad;
};
[[vk::push_constant]] ConstantBuffer<SsgiPush> pc;
[[vk::binding(0, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(1, 0)]] Texture2D<float> pyramid;
[[vk::binding(2, 0)]] [vk::image_format("r32f")] RWTexture2D<float> depthOutput;
[[vk::binding(3, 0)]] Texture2D<float4> normalKind;
[[vk::binding(4, 0)]] Texture2D<float4> diffuseRadiance;
[[vk::binding(5, 0)]] [vk::image_format("rgba32f")] RWTexture2D<float4> rawOutput;
