/**
 * @file Fxaa.h
 * @author Rahul Nair
 * @brief FXAA: a single fullscreen pass, luma-edge-detect anti-aliasing for
 *        the raster pipeline. Same hand-off shape as PostProcess's tonemap -
 *        no owned output image, draws into whatever the caller already
 *        bound.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

namespace mir::vk
{
    class Device;

    class Fxaa
    {
    public:
        bool Initialize(Device &device, VkFormat colorFormat);
        void Shutdown();

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkExtent2D extent);

    private:
        Device *mDevice = nullptr;

        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
