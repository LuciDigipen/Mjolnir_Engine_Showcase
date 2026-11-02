/**
 * @file DepthOfField.h
 * @author Rahul Nair
 * @brief Depth of field: a half-res CoC pass, separate near/far gather blurs
 *        and a full-res composite, run on whatever linear-HDR color and
 *        depth/distance the raster or ray traced pipeline hands it, ahead of
 *        Bloom and PostProcess's tonemap pass.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <string>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    class DepthOfField
    {
    public:
        bool Initialize(Device &device, VkExtent2D extent);
        void Shutdown();

        bool PrepareShaderReload(std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();

        bool Resize(VkExtent2D extent);

        void SetParams(float focusDistance, float aperture, float maxBlur)
        {
            mFocusDistance = focusDistance;
            mAperture = aperture;
            mMaxBlur = maxBlur;
        }

        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkImageView depthOrDistance,
            bool isLinearDepth, float nearClip, float farClip, VkExtent2D extent) const;

        VkImageView OutputView(uint32_t frame) const { return mOutputViews[frame]; }

        struct ReloadCandidate
        {
            VkPipeline mCoc = VK_NULL_HANDLE;
            VkPipeline mNearBlur = VK_NULL_HANDLE;
            VkPipeline mFarBlur = VK_NULL_HANDLE;
            VkPipeline mComposite = VK_NULL_HANDLE;
        };

        bool BuildReloadCandidate(ReloadCandidate &candidate, std::string *error);
        void DestroyCandidate(ReloadCandidate &candidate);

        ReloadCandidate mReloadCandidate;
        bool mReloadPrepared = false;

    private:
        bool CreateImages(VkExtent2D extent);
        void DestroyImages();
        bool CreatePipelines();
        void DestroyPipelines();
        void WriteStaticDescriptors();

        Device *mDevice = nullptr;

        // NEAREST: depth/distance must never blend across a discontinuity -
        // bilinearly averaging a near and far sample produces a nonsensical
        // mid-value right at an object's silhouette.
        VkSampler mDepthSampler = VK_NULL_HANDLE;
        // LINEAR + CLAMP_TO_BORDER(black): the near/far blur passes gather
        // with tap offsets that reach past the working buffers' edges - see
        // the same fix in Bloom.cpp for why CLAMP_TO_EDGE would flicker there.
        VkSampler mColorSampler = VK_NULL_HANDLE;

        // CoC pass: reads the external color+depth source (2 bindings).
        VkDescriptorSetLayout mCocSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mCocPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mCocPipeline = VK_NULL_HANDLE;
        // Rewritten every Record() call - the source view changes frame to frame.
        VkDescriptorSet mCocSets[kFramesInFlight]{};

        // Near/far blur: both just gather over the half-res CoC+color buffer (1 binding).
        VkDescriptorSetLayout mBlurSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mBlurPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mNearBlurPipeline = VK_NULL_HANDLE;
        VkPipeline mFarBlurPipeline = VK_NULL_HANDLE;
        // Written once (points at this frame's own CoC buffer, which never
        // changes identity between resizes) and shared by both pipelines.
        VkDescriptorSet mBlurReadSets[kFramesInFlight]{};

        // Composite: external color+depth (dynamic, bindings 0-1) plus this
        // frame's own near/far results (static, bindings 2-3) - 4 bindings.
        VkDescriptorSetLayout mCompositeSetLayout = VK_NULL_HANDLE;
        VkPipelineLayout mCompositePipelineLayout = VK_NULL_HANDLE;
        VkPipeline mCompositePipeline = VK_NULL_HANDLE;
        VkDescriptorSet mCompositeSets[kFramesInFlight]{};

        VkDescriptorPool mPool = VK_NULL_HANDLE;

        VkImage mCocColorImages[kFramesInFlight]{};
        VmaAllocation mCocColorAllocations[kFramesInFlight]{};
        VkImageView mCocColorViews[kFramesInFlight]{};

        VkImage mNearResultImages[kFramesInFlight]{};
        VmaAllocation mNearResultAllocations[kFramesInFlight]{};
        VkImageView mNearResultViews[kFramesInFlight]{};

        VkImage mFarResultImages[kFramesInFlight]{};
        VmaAllocation mFarResultAllocations[kFramesInFlight]{};
        VkImageView mFarResultViews[kFramesInFlight]{};

        // Full resolution: what Bloom and PostProcess read once DoF is enabled.
        VkImage mOutputImages[kFramesInFlight]{};
        VmaAllocation mOutputAllocations[kFramesInFlight]{};
        VkImageView mOutputViews[kFramesInFlight]{};

        VkExtent2D mHalfExtent{};

        float mFocusDistance = 10.0f;
        float mAperture = 0.1f;
        float mMaxBlur = 16.0f;
    };
}
