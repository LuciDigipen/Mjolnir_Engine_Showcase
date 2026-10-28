/**
 * @file ParticleRibbonRenderer.h
 * @author sumin.park
 * @brief Frame-slot protected ribbon uploads and FX drawing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles_gpu/ParticleSubmission.h"

#include <renderer/FxSystem.h>

#include <vk_mem_alloc.h>

#include <vector>

namespace mir::gpu
{
    class ParticleRibbonRenderer
    {
    public:
        ParticleRibbonRenderer() = default;
        ParticleRibbonRenderer(const ParticleRibbonRenderer &) = delete;
        ParticleRibbonRenderer &operator=(const ParticleRibbonRenderer &) = delete;
        void Submit(std::span<const RibbonSubmission> ribbons);
        bool HasData() const { return !mRibbons.empty(); }
        void Prepare(const FxFrameContext &context);
        void Record(const FxRenderContext &context);
        void Shutdown();
    private:
        void Initialize(const FxFrameContext &context);
        std::vector<RibbonSubmission> mRibbons;
        VkDevice mDevice = VK_NULL_HANDLE;
        VmaAllocator mAllocator = nullptr;
        VkBuffer mBuffers[kFxFramesInFlight]{};
        VmaAllocation mMemory[kFxFramesInFlight]{};
        void *mMapped[kFxFramesInFlight]{};
        uint32_t mUsed[kFxFramesInFlight]{};
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight]{};
        VkSampler mSampler = VK_NULL_HANDLE;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
