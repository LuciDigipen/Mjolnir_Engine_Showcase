/**
 * @file ParticleSpriteRenderer.h
 * @author sumin.park
 * @brief Particle sprite pipelines and per-view indirect drawing into renderer targets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/FxSystem.h>

namespace mir::gpu
{
    class ParticleDataset;
    class ParticleSpriteRenderer
    {
    public:
        ParticleSpriteRenderer() = default;
        ParticleSpriteRenderer(const ParticleSpriteRenderer &) = delete;
        ParticleSpriteRenderer &operator=(const ParticleSpriteRenderer &) = delete;
        void Record(const FxRenderContext &context, const ParticleDataset &data);
        void Shutdown();
    private:
        bool Initialize(const FxRenderContext &context, const ParticleDataset &data);
        VkDevice mDevice = VK_NULL_HANDLE;
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
