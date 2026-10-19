/**
 * @file Bloom.h
 * @author Rahul Nair
 * @brief Bloom: a bright-pass extract, a downsample chain and an additive
 *        upsample chain, run on whatever linear-HDR source the raster or ray
 *        traced pipeline hands it, ahead of PostProcess's tonemap pass.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    class Bloom
    {
    public:
        bool Initialize(Device &device, VkExtent2D extent);
        void Shutdown();

        bool Resize(VkExtent2D extent);

        void SetParams(float threshold, float intensity, float radius)
        {
            mThreshold = threshold;
            mIntensity = intensity;
            mRadius = radius;
        }

        float Intensity() const { return mIntensity; }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkExtent2D extent) const;

        VkImageView OutputView(uint32_t frame) const { return mMipViews[frame][0]; }

    private:
        bool CreateMipChain(VkExtent2D extent);
        void DestroyMipChain();
        bool CreatePipelines();
        void DestroyPipelines();

        void WriteStaticDescriptors();

        Device *mDevice = nullptr;

        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;

        VkDescriptorSet mExtractSets[kFramesInFlight]{};

        VkDescriptorSet mDownsampleSets[kFramesInFlight][kBloomMipLevels - 1]{};

        VkDescriptorSet mUpsampleSets[kFramesInFlight][kBloomMipLevels - 1]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mExtractPipeline = VK_NULL_HANDLE;
        VkPipeline mDownsamplePipeline = VK_NULL_HANDLE;
        VkPipeline mUpsamplePipeline = VK_NULL_HANDLE;

        VkImage mMipImages[kFramesInFlight][kBloomMipLevels]{};
        VmaAllocation mMipAllocations[kFramesInFlight][kBloomMipLevels]{};
        VkImageView mMipViews[kFramesInFlight][kBloomMipLevels]{};

        VkExtent2D mMipExtents[kBloomMipLevels]{};

        float mThreshold = 1.0f;
        float mIntensity = 0.5f;
        float mRadius = 1.0f;
    };
}
