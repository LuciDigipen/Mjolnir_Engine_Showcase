/**
 * @file ParticleSpriteRenderer.h
 * @author sumin.park
 * @brief Particle sprite pipelines and per-view indirect drawing into renderer targets.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "particles_gpu/ParticleSubmission.h"

#include <renderer/FxSystem.h>

#include <vk_mem_alloc.h>

#include <array>
#include <map>
#include <utility>

namespace mir::gpu
{
    class ParticleDataset;
    class ParticleSpriteRenderer
    {
    public:
        ParticleSpriteRenderer() = default;
        ParticleSpriteRenderer(const ParticleSpriteRenderer &) = delete;
        ParticleSpriteRenderer &operator=(const ParticleSpriteRenderer &) = delete;
        void PrepareTextures(const FxFrameContext &context, const ParticleDataset &data);
        void Record(const FxRenderContext &context, const ParticleDataset &data);
        void Shutdown();
    private:
        bool Initialize(const FxRenderContext &context, const ParticleDataset &data);
        using TextureKey = std::pair<uint64_t, uint64_t>;
        struct Texture
        {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            VkImageView view = VK_NULL_HANDLE;
            VkBuffer staging = VK_NULL_HANDLE;
            VmaAllocation stagingAllocation = nullptr;
            uint64_t uploaded = 0;
            uint64_t lastUse = 0;
            VkDeviceSize bytes = 0;
        };
        void DestroyTexture(Texture &texture);
        void UploadTexture(const FxFrameContext &context, Texture &texture,
            uint32_t width, uint32_t height, const uint8_t *pixels);
        VmaAllocator mAllocator = nullptr;
        std::map<TextureKey, Texture> mTextures;
        std::array<TextureKey, kGpuParticleInstances> mSlotTextures{};
        uint64_t mSignals[kFxFramesInFlight]{};
        VkDeviceSize mTextureBytes = 0;
        VkDevice mDevice = VK_NULL_HANDLE;
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight][kGpuParticleInstances]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
    };
}
