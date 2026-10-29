/**
 * @file ParticleViewSort.h
 * @author sumin.park
 * @brief View-local GPU culling, bitonic sorting and indirect draw counts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles_gpu/ParticleSubmission.h"

#include <renderer/FxSystem.h>

#include <vk_mem_alloc.h>

namespace mir::gpu
{
    class ParticleDataset;
    class ParticleViewSort
    {
    public:
        ParticleViewSort() = default;
        ParticleViewSort(const ParticleViewSort &) = delete;
        ParticleViewSort &operator=(const ParticleViewSort &) = delete;
        void Prepare(const FxRenderContext &context, const ParticleDataset &data);
        VkBuffer Indices(uint32_t frame) const { return mNodes[frame].handle; }
        VkBuffer Arguments(uint32_t frame) const { return mArguments[frame].handle; }
        void Stats(ParticleGpuStats &stats) const;
        uint64_t MemoryBytes() const { return mDevice ? kFxFramesInFlight * (uint64_t(kFxMaxViews) * kGpuParticleCapacity * 16 + kFxMaxViews * kGpuParticleInstances * 32 + kFxMaxViews * 8) : 0; }
        void Shutdown();
    private:
        struct Buffer { VkBuffer handle = VK_NULL_HANDLE; VmaAllocation memory = nullptr; void *mapped = nullptr; };
        void Initialize(const FxFrameContext &context, const ParticleDataset &data);
        VkDevice mDevice = VK_NULL_HANDLE;
        VmaAllocator mAllocator = nullptr;
        Buffer mNodes[kFxFramesInFlight], mArguments[kFxFramesInFlight], mReadback[kFxFramesInFlight];
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        uint64_t mSignals[kFxFramesInFlight]{};
        uint64_t mCompleted = 0;
        uint32_t mVisible[kFxMaxViews]{}, mFirst[kFxMaxViews]{};
    };
}
