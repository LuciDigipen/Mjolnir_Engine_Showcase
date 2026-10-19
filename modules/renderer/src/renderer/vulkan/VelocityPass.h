/**
 * @file VelocityPass.h
 * @author Rahul Nair
 * @brief Raster motion blur's velocity buffer: a second, depth-tested walk of
 *        the same DrawItems the Scene Pass already drew, through a dedicated
 *        shader that only needs this frame's and last frame's model matrix -
 *        the same shape as ShadowMaps' depth-only pass, not Bloom/DepthOfField's
 *        screen-space ones, since it needs real per-object transforms.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <renderer/VulkanRenderer.h>

#include <volk.h>

#include <cstdint>
#include <span>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;

    class VelocityPass
    {
    public:
        bool Initialize(Device &device, VkDescriptorSetLayout cameraSetLayout, VkExtent2D extent);
        void Shutdown();

        bool Resize(VkExtent2D extent);

        void Record(VkCommandBuffer cmd, uint32_t frame, std::span<const RenderView> views,
            VkDescriptorSet cameraSet, std::span<const uint32_t> cameraOffsets, VkImageView depthView,
            VkExtent2D extent, const ResourceStore &resources);

        VkImageView OutputView(uint32_t frame) const { return mOutputViews[frame]; }

    private:
        bool CreateImages(VkExtent2D extent);
        void DestroyImages();

        Device *mDevice = nullptr;

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        VkImage mOutputImages[kFramesInFlight]{};
        VmaAllocation mOutputAllocations[kFramesInFlight]{};
        VkImageView mOutputViews[kFramesInFlight]{};
    };
}
