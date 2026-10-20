/**
 * @file PostProcess.h
 * @author Rahul Nair
 * @brief The tonemap pass: the one shared post-process hook point both the
 *        raster and ray traced pipelines funnel their own linear-HDR color
 *        target through on the way to the swapchain.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>

namespace mir::vk
{
    class Device;

    class PostProcess
    {
    public:
        bool Initialize(Device &device, VkFormat swapchainFormat);
        void Shutdown();

        void SetParams(float exposure, int tonemapOperator)
        {
            mExposure = exposure;
            mTonemapOperator = tonemapOperator;
        }

        void SetFilmParams(float grain, float contrast, float saturation, float vignetteStrength,
            float vignetteRadius)
        {
            mGrain = grain;
            mContrast = contrast;
            mSaturation = saturation;
            mVignetteStrength = vignetteStrength;
            mVignetteRadius = vignetteRadius;
        }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkImageView bloom,
            float bloomIntensity, VkExtent2D extent);

    private:
        Device *mDevice = nullptr;

        VkSampler mSampler = VK_NULL_HANDLE;

        VkSampler mBloomSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        float mExposure = 1.0f;
        int mTonemapOperator = 0;

        float mGrain = 0.0f;
        float mContrast = 1.0f;
        float mSaturation = 1.0f;
        float mVignetteStrength = 0.0f;
        float mVignetteRadius = 1.0f;

        // Advances once per Record() so grain reads as animated noise, not a static pattern.
        uint32_t mGrainFrame = 0;
    };
}
