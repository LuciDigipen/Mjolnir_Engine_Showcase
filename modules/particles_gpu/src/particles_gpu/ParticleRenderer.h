/**
 * @file ParticleRenderer.h
 * @author sumin.park
 * @brief Shared sprite and ribbon materials, view data and ordered FX draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "ParticleViewSort.h"
#include "particles_gpu/ParticleRibbonGeometry.h"

#include <renderer/FxSystem.h>

#include <glm/mat4x4.hpp>
#include <vk_mem_alloc.h>

#include <array>
#include <map>
#include <utility>
#include <vector>

namespace mir::gpu
{
    class ParticleDataset;
    class ParticleRenderer
    {
    public:
        ParticleRenderer() = default;
        ParticleRenderer(const ParticleRenderer &) = delete;
        ParticleRenderer &operator=(const ParticleRenderer &) = delete;
        void Submit(std::span<const RibbonSubmission> ribbons);
        bool HasRibbons() const { return !mRibbons.empty(); }
        void PrepareTextures(const FxFrameContext &context,const ParticleDataset &data);
        void PrepareDraw(const FxRenderContext &context,const ParticleDataset &data);
        void Record(const FxRenderContext &context,const ParticleDataset &data);
        void Stats(ParticleGpuStats &stats) const { mSort.Stats(stats); }
        void Shutdown();
    private:
        static constexpr uint32_t kGroups = kGpuParticleInstances*2;
        struct ViewData
        {
            glm::mat4 viewProj,inverseViewProj;
            glm::vec4 anchorStart,right,up,sampling,rect;
            glm::vec4 sunDirection,sunColor,ambient,fogColor,fogRange,material;
            glm::uvec4 options;
            glm::vec4 texture;
        };
        static_assert(sizeof(ViewData)==336);
        struct Draw { uint32_t slot,view,first,count; bool ribbon; float distance; };
        struct Buffer { VkBuffer handle=VK_NULL_HANDLE; VmaAllocation memory=nullptr; void *mapped=nullptr; };
        void Initialize(const FxRenderContext &context);
        using TextureKey = std::pair<uint64_t,uint64_t>;
        struct Texture
        {
            VkImage image=VK_NULL_HANDLE;
            VmaAllocation allocation=nullptr;
            VkImageView view=VK_NULL_HANDLE;
            VkBuffer staging=VK_NULL_HANDLE;
            VmaAllocation stagingAllocation=nullptr;
            uint64_t uploaded=0,lastUse=0;
            VkDeviceSize bytes=0;
        };
        void DestroyTexture(Texture &texture);
        void UploadTexture(const FxFrameContext &context,Texture &texture,uint32_t width,uint32_t height,const uint8_t *pixels);
        ParticleViewSort mSort;
        std::vector<RibbonSubmission> mRibbons;
        std::vector<Draw> mDraws;
        VmaAllocator mAllocator=nullptr;
        VkDevice mDevice=VK_NULL_HANDLE;
        std::map<TextureKey,Texture> mTextures;
        std::array<TextureKey,kGroups> mSlotTextures{};
        uint64_t mSignals[kFxFramesInFlight]{};
        VkDeviceSize mTextureBytes=0;
        Buffer mVertices[kFxFramesInFlight],mViews[kFxFramesInFlight];
        VkSampler mSampler=VK_NULL_HANDLE,mTextureSampler=VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout=VK_NULL_HANDLE;
        VkDescriptorPool mPool=VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFxFramesInFlight][kGroups]{};
        VkPipelineLayout mLayout=VK_NULL_HANDLE;
        VkPipeline mPipelines[2][2]{};
    };
}
