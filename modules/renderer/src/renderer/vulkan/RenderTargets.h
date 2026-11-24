/**
 * @file RenderTargets.h
 * @author Quy Dong Ta
 * @brief The images each render target owns, by handle, and the frames they wait on before they are freed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"

#include <renderer/MeshStreaming.h>
#include <renderer/RenderTarget.h>

#include <volk.h>

#include <cstdint>
#include <vector>

namespace mir::vk
{
    class Device;

    struct GpuRenderTarget
    {
        VkExtent2D extent{};
        // the scene in HDR and its depth, then the tonemapped image the UI samples
        Image2D color;
        Image2D depth;
        Image2D output;
        // the tonemap's inputs, written once for color
        VkDescriptorSet tonemapSet = VK_NULL_HANDLE;
        // output has not been cleared into a readable layout yet
        bool fresh = true;
    };

    class RenderTargets
    {
    public:
        // tonemapLayout and sampler are the tonemap pass's, outputFormat the format it writes
        bool Initialize(Device &device, VkFormat outputFormat, VkDescriptorSetLayout tonemapLayout,
            VkSampler sampler);
        // The caller idles the device first
        void Shutdown();

        // Null for a size outside 1 to kMaxRenderTargetSize, past kMaxRenderTargets, or when Vulkan fails
        RenderTargetHandle Create(VkExtent2D extent);
        // The handle stays, the old images go once lastUse completes. False leaves the target as it was
        bool Resize(RenderTargetHandle handle, VkExtent2D extent, uint64_t lastUse);
        bool Release(RenderTargetHandle handle, uint64_t lastUse);

        GpuRenderTarget *Find(RenderTargetHandle handle);
        const GpuRenderTarget *Find(RenderTargetHandle handle) const;
        uint32_t LiveCount() const;

        // Clears each target made since the last call and leaves its output readable, so the UI can show it
        // before a camera draws into it. Also the outputs a resize or release retired before that, which
        // the UI built for this frame may still show
        void RecordFresh(VkCommandBuffer cmd, VkClearColorValue clear);
        void Collect(uint64_t completed);

    private:
        bool Make(VkExtent2D extent, GpuRenderTarget &out);
        void Destroy(GpuRenderTarget &target);
        void Retire(uint64_t lastUse, const GpuRenderTarget &target);
        void RecordClear(VkCommandBuffer cmd, const Image2D &output, VkExtent2D extent, VkClearColorValue clear);

        struct Slot
        {
            GpuRenderTarget target;
            uint32_t generation = 0;
            bool live = false;
        };

        Device *mDevice = nullptr;
        VkFormat mOutputFormat = VK_FORMAT_UNDEFINED;
        VkDescriptorSetLayout mTonemapLayout = VK_NULL_HANDLE;
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::vector<Slot> mSlots;
        FrameRetireList<GpuRenderTarget> mRetired;
        // retired while still fresh, they stay alive until the next frame's RecordFresh is done with them
        std::vector<GpuRenderTarget> mFreshRetired;
    };
}
