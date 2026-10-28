/**
 * @file FxCompositionPass.h
 * @author sumin.park
 * @brief Renderer-owned HDR composition target and generic FX draw hook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"

#include <renderer/FxSystem.h>

namespace mir::vk
{
    class Device;
    class FxCompositionPass
    {
    public:
        FxCompositionPass() = default;
        FxCompositionPass(const FxCompositionPass &) = delete;
        FxCompositionPass &operator=(const FxCompositionPass &) = delete;
        VkImageView Record(Device &device, IFxSystem *system, const FxRenderContext &context);
        // The renderer completes all image uses before resizing or shutdown.
        bool Resize(VkExtent2D extent);
        void Shutdown();
    private:
        bool Initialize(Device &device, VkExtent2D extent);
        Device *mDevice = nullptr;
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        Image2D mOutput[kFramesInFlight]{};
    };
}
