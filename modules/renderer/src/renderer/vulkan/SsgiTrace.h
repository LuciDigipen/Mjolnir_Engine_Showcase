/**
 * @file SsgiTrace.h
 * @author Rahul Nair
 * @brief Per-view depth pyramids and raw screen-space irradiance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"
#include <renderer/VulkanRenderer.h>
#include <array>
#include <vector>

namespace mir::vk
{
    class SsgiTrace
    {
    public:
        void Initialize(Device &device, VkDescriptorSetLayout cameraLayout);
        void Configure(bool requested, VkExtent2D extent, std::span<const RenderView> views, bool halfResolution);
        void Shutdown(); // Caller has idled the device.
        void Record(VkCommandBuffer cmd, uint32_t frame, VkImage depth, VkImageView depthView,
            const class RasterSurfaces &surfaces, VkDescriptorSet cameraSet,
            std::span<const uint32_t> cameraOffsets, const SsgiSettings &settings);
        bool Active() const { return mPool != VK_NULL_HANDLE; }
        const Image2D &Raw(uint32_t view, uint32_t frame) const { return mViews[view].frames[frame].raw; }
        VkExtent2D OutputExtent(uint32_t view) const { return mViews[view].output; }
        static uint32_t MipCount(VkExtent2D extent);
        static VkExtent2D TraceExtent(VkExtent2D extent, bool halfResolution);
        static bool FitsBudget(std::span<const VkRect2D> rects, bool halfResolution,
            const VkPhysicalDeviceLimits &limits);

    private:
        struct Frame
        {
            Image2D pyramid, raw;
            std::vector<VkImageView> mips;
            std::vector<VkDescriptorSet> sets;
        };
        struct View
        {
            VkRect2D rect{};
            VkExtent2D output{};
            std::array<Frame, kFramesInFlight> frames;
        };
        bool Create();
        void Destroy();
        Device *mDevice = nullptr;
        VkDescriptorSetLayout mCameraLayout = VK_NULL_HANDLE, mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::array<VkPipeline, 3> mPipelines{};
        std::vector<View> mViews;
        bool mHalfResolution = true, mFailed = false;
        uint32_t mSeed = 0;
    };
}
