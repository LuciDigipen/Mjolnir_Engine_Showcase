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

#include <cstddef>
#include <map>
#include <span>
#include <vector>

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
        VkPipeline Pipeline(uint64_t key, std::span<const std::byte> code);
        struct ScriptPipeline
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            uint64_t lastUse = 0;
            std::vector<std::byte> code;
        };
        VkDevice mDevice = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        std::map<uint64_t, ScriptPipeline> mPipelines;
        bool mFirst = true;
    };
}
