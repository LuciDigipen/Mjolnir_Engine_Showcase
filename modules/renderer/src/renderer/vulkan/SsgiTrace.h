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
        void ResetHistory();
        void Record(VkCommandBuffer cmd, uint32_t frame, VkImage depth, VkImageView depthView,
            const class RasterSurfaces &surfaces, VkDescriptorSet cameraSet,
            std::span<const uint32_t> cameraOffsets, const SsgiSettings &settings);
        bool Active() const { return mPool != VK_NULL_HANDLE; }
        const Image2D &Raw(uint32_t view, uint32_t frame) const { return mViews[view].frames[frame].raw; }
        const Image2D &Filtered(uint32_t view, uint32_t frame) const { return mViews[view].frames[frame].resolved; }
        VkExtent2D OutputExtent(uint32_t view) const { return mViews[view].output; }
        static uint32_t MipCount(VkExtent2D extent);
        static VkExtent2D TraceExtent(VkExtent2D extent, bool halfResolution);
        static bool FitsBudget(std::span<const VkRect2D> rects, bool halfResolution,
            const VkPhysicalDeviceLimits &limits);
        static bool HistoryCompatible(const glm::mat4 &previous, WorldPosition previousCamera,
            const glm::mat4 &current, WorldPosition currentCamera, float maxDistance);

    private:
        struct Frame
        {
            Image2D pyramid, raw, history, geometry, scratch, resolved;
            bool historyLayout = false;
            std::array<VkDescriptorSet, 3> filterSets{};
            std::vector<VkImageView> mips;
            std::vector<VkDescriptorSet> sets;
        };
        struct View
        {
            VkRect2D rect{};
            VkExtent2D output{};
            std::array<Frame, kFramesInFlight> frames;
            glm::mat4 currentMatrix{ 1 }, previousMatrix{ 1 };
            WorldPosition currentCamera, previousCamera;
            SsgiSettings previousSettings;
            uint32_t previousFrame = 0;
            uint64_t currentId = 0, previousId = 0;
            bool historyValid = false;
        };
        bool Create();
        bool CreateFilterPipelines();
        void Filter(VkCommandBuffer cmd, uint32_t frame, View &view, VkImageView depthView,
            VkImageView normalView, VkDescriptorSet cameraSet, uint32_t cameraOffset, const SsgiSettings &settings);
        void Destroy();
        Device *mDevice = nullptr;
        VkDescriptorSetLayout mCameraLayout = VK_NULL_HANDLE, mSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout mFilterSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mFilterLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::array<VkPipeline, 3> mPipelines{};
        std::array<VkPipeline, 3> mFilterPipelines{};
        std::vector<View> mViews;
        bool mHalfResolution = true, mFailed = false;
        uint32_t mSeed = 0;
    };
}
