/**
 * @file MotionBlur.h
 * @author Rahul Nair
 * @brief Motion blur's composite: a single fullscreen pass that samples
 *        color along whatever velocity buffer the raster or ray traced
 *        pipeline hands it (VelocityPass's output, or RayTracer's gVelocity) -
 *        the same hand-off shape Bloom and DepthOfField already use.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <string>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    class MotionBlur
    {
    public:
        bool Initialize(Device &device, VkExtent2D extent);
        void Shutdown();

        bool PrepareShaderReload(std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();

        bool Resize(VkExtent2D extent);

        void SetParams(float shutterStrength, uint32_t sampleCount)
        {
            mShutterStrength = shutterStrength;
            mSampleCount = sampleCount;
        }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkImageView velocity,
            VkExtent2D extent);

        VkImageView OutputView(uint32_t frame) const { return mOutputViews[frame]; }

        VkPipeline mReloadPipeline = VK_NULL_HANDLE;
        bool mReloadPrepared = false;

    private:
        bool CreateImages(VkExtent2D extent);
        void DestroyImages();

        Device *mDevice = nullptr;

        // NEAREST: the exact per-pixel vector, not blended across a silhouette
        // with its neighbor - the same reasoning as DepthOfField's depth sampler.
        VkSampler mVelocitySampler = VK_NULL_HANDLE;
        // LINEAR + CLAMP_TO_BORDER(black): taps walk off either end of the
        // vector and may reach past the image edge - see Bloom/DepthOfField.
        VkSampler mColorSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        VkImage mOutputImages[kFramesInFlight]{};
        VmaAllocation mOutputAllocations[kFramesInFlight]{};
        VkImageView mOutputViews[kFramesInFlight]{};

        float mShutterStrength = 0.0f;
        uint32_t mSampleCount = 8;
    };
}
