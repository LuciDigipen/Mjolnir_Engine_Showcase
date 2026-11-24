/**
 * @file PostProcess.h
 * @author Rahul Nair
 * @brief The tonemap pass: the one shared post-process hook point both the
 *        raster and ray traced pipelines funnel their own linear-HDR color
 *        target through on the way to the swapchain.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>

#include <cstdint>
#include <string>

namespace mir::vk
{
    class Device;

    // One frame's film grain. Whichever pass runs last before the swapchain applies it -
    // tonemap normally, FXAA when active (grain after AA, so the edge blend can't smear it).
    struct FilmGrain
    {
        float amount = 0.0f;
        float seed = 0.0f;
    };

    class PostProcess
    {
    public:
        bool Initialize(Device &device, VkFormat swapchainFormat);
        bool PrepareShaderReload(std::string *error = nullptr);
        void CommitShaderReload();
        void DiscardShaderReload();

        void Shutdown();

        void SetParams(float exposure, int tonemapOperator)
        {
            mExposure = exposure;
            mTonemapOperator = tonemapOperator;
        }

        void SetFilmParams(float grain, float contrast, float saturation, float vignetteStrength,
            float vignetteRadius)
        {
            mGrain = grain;
            mContrast = contrast;
            mSaturation = saturation;
            mVignetteStrength = vignetteStrength;
            mVignetteRadius = vignetteRadius;
        }

        // Call once per frame; the seed advances so grain reads as animated noise.
        FilmGrain NextFilmGrain() { return { mGrain, static_cast<float>(mGrainFrame++) }; }

        // grain.amount 0 leaves grain to a later pass.
        void Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkImageView bloom,
            float bloomIntensity, FilmGrain grain, VkExtent2D extent);

        // A render target's own set, written once, so its pass never rewrites the frame's
        VkDescriptorSetLayout SetLayout() const { return mSetLayout; }
        VkSampler Sampler() const { return mSampler; }
        // Into a render target: no bloom, grain or vignette, and the camera's exposure while HDR is on
        void RecordTarget(VkCommandBuffer cmd, VkDescriptorSet set, float exposure, VkExtent2D extent) const;

        VkPipeline mReloadPipeline = VK_NULL_HANDLE;
        bool mReloadPrepared = false;
        VkFormat mSwapchainFormat = VK_FORMAT_UNDEFINED;

    private:
        VkPipeline BuildPipeline();

        Device *mDevice = nullptr;

        VkSampler mSampler = VK_NULL_HANDLE;

        VkSampler mBloomSampler = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;

        float mExposure = 1.0f;
        int mTonemapOperator = 0;

        float mGrain = 0.0f;
        float mContrast = 1.0f;
        float mSaturation = 1.0f;
        float mVignetteStrength = 0.0f;
        float mVignetteRadius = 1.0f;

        // Advanced by NextFilmGrain.
        uint32_t mGrainFrame = 0;
    };
}
