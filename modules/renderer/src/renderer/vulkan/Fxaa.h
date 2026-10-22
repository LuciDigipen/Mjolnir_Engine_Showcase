/**
 * @file Fxaa.h
 * @author Rahul Nair
 * @brief FXAA: a single fullscreen pass, luma-edge-detect anti-aliasing for
 *        the raster pipeline. Owns the LDR intermediate the tonemap pass
 *        writes into ahead of it, and draws into whatever the caller
 *        already bound.
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

    class Fxaa
    {
    public:
        bool Initialize(Device &device, VkFormat colorFormat);
        void Shutdown();

        // The intermediate exists only while Fxaa is the active mode; the caller idles the
        // device before either call.
        bool CreateIntermediate(VkExtent2D extent);
        void DestroyIntermediate();
        bool HasIntermediate() const { return mIntermediateImages[0] != VK_NULL_HANDLE; }

        // Recreates the intermediate at the new swapchain extent, if it exists.
        bool Resize(VkExtent2D extent);

        VkImage IntermediateImage(uint32_t frame) const { return mIntermediateImages[frame]; }
        VkImageView IntermediateView(uint32_t frame) const { return mIntermediateViews[frame]; }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkExtent2D extent);

    private:
        Device *mDevice = nullptr;

        // The swapchain's format, so the tonemap pipeline can target it unchanged.
        VkFormat mColorFormat = VK_FORMAT_UNDEFINED;

        VkImage mIntermediateImages[kFramesInFlight]{};
        VmaAllocation mIntermediateAllocations[kFramesInFlight]{};
        VkImageView mIntermediateViews[kFramesInFlight]{};

        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
