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

#include "Image2D.h"
#include "PostProcess.h"
#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <string>

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
        bool HasIntermediate() const { return mIntermediate[0].image != VK_NULL_HANDLE; }

        // Recreates the intermediate at the new swapchain extent, if it exists.
        bool PrepareShaderReload(std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();

        bool Resize(VkExtent2D extent);

        VkImage IntermediateImage(uint32_t frame) const { return mIntermediate[frame].image; }
        VkImageView IntermediateView(uint32_t frame) const { return mIntermediate[frame].view; }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, FilmGrain grain, VkExtent2D extent);

        VkPipeline mReloadPipeline = VK_NULL_HANDLE;
        bool mReloadPrepared = false;

    private:
        Device *mDevice = nullptr;

        // The swapchain's format, so the tonemap pipeline can target it unchanged.
        VkFormat mColorFormat = VK_FORMAT_UNDEFINED;

        Image2D mIntermediate[kFramesInFlight]{};

        VkSampler mSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
