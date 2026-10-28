/**
 * @file ParticleDispatch.h
 * @author sumin.park
 * @brief GPU emitter update, compaction, births and indirect argument dispatch.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/FxSystem.h>

namespace mir::gpu
{
    class ParticleDataset;
    class ParticleDispatch
    {
    public:
        ParticleDispatch() = default;
        ParticleDispatch(const ParticleDispatch &) = delete;
        ParticleDispatch &operator=(const ParticleDispatch &) = delete;
        void Record(const FxFrameContext &context, ParticleDataset &data);
        void Shutdown();
    private:
        bool Initialize(const FxDeviceContext &device, ParticleDataset &data);
        VkDevice mDevice = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        bool mFirst = true;
    };
}
