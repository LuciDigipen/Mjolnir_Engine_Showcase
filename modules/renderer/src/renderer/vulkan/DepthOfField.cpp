/**
 * @file DepthOfField.cpp
 * @author Rahul Nair
 * @brief Depth of field: a half-res CoC pass, separate near/far gather blurs
 *        and a full-res composite, run on whatever linear-HDR color and
 *        depth/distance the raster or ray traced pipeline hands it, ahead of
 *        Bloom and PostProcess's tonemap pass.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DepthOfField.h"
#include "Device.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <format>

namespace mir::vk
{
    namespace
    {
        uint32_t RoundUpPushConstantSize(size_t size)
        {
            constexpr uint32_t kAlignment = 16;
            return (static_cast<uint32_t>(size) + kAlignment - 1) & ~(kAlignment - 1);
        }
    }

    bool DepthOfField::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;

        const VkSamplerCreateInfo depthSamplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0f };

        if (vkCreateSampler(mDevice->Handle(), &depthSamplerInfo, nullptr, &mDepthSampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateSampler (depth) failed");
            return false;
        }

        const VkSamplerCreateInfo colorSamplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
            .maxLod = 0.0f,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK };

        if (vkCreateSampler(mDevice->Handle(), &colorSamplerInfo, nullptr, &mColorSampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateSampler (color) failed");
            return false;
        }

        const VkDescriptorSetLayoutBinding cocBindings[]{
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
        const VkDescriptorSetLayoutCreateInfo cocSetLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = cocBindings };
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &cocSetLayoutInfo, nullptr, &mCocSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout (coc) failed");
            return false;
        }

        const VkDescriptorSetLayoutBinding blurBinding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        const VkDescriptorSetLayoutCreateInfo blurSetLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &blurBinding };
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &blurSetLayoutInfo, nullptr, &mBlurSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout (blur) failed");
            return false;
        }

        VkDescriptorSetLayoutBinding compositeBindings[4]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            compositeBindings[i] = VkDescriptorSetLayoutBinding{
                .binding = i,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        }
        const VkDescriptorSetLayoutCreateInfo compositeSetLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4,
            .pBindings = compositeBindings };
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &compositeSetLayoutInfo, nullptr, &mCompositeSetLayout) !=
            VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout (composite) failed");
            return false;
        }

        // Per frame: 2 (coc) + 1 (blur, shared by near+far) + 4 (composite) = 7 descriptors, 3 sets.
        constexpr uint32_t kDescriptorsPerFrame = 7;
        constexpr uint32_t kSetsPerFrame = 3;
        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kDescriptorsPerFrame * kFramesInFlight };
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kSetsPerFrame * kFramesInFlight,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };
        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorPool failed");
            return false;
        }

        const auto allocate = [&](VkDescriptorSetLayout layout, VkDescriptorSet &outSet)
            { return AllocateDescriptorSet(mDevice->Handle(), mPool, layout, outSet); };

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            if (!allocate(mCocSetLayout, mCocSets[frame]) ||
                !allocate(mBlurSetLayout, mBlurReadSets[frame]) ||
                !allocate(mCompositeSetLayout, mCompositeSets[frame]))
            {
                MIR_LOG_CRITICAL("Initialize: descriptor set allocation failed for frame {}", frame);
                return false;
            }
        }

        if (!CreatePipelines() || !CreateImages(extent))
            return false;

        return true;
    }

    bool DepthOfField::CreatePipelines()
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.vertexMain.spv"));
        VkShaderModule cocModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.cocMain.spv"));
        VkShaderModule nearModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.nearBlurMain.spv"));
        VkShaderModule farModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.farBlurMain.spv"));
        VkShaderModule compositeModule =
            CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.compositeMain.spv"));

        const bool allLoaded = vertModule != VK_NULL_HANDLE && cocModule != VK_NULL_HANDLE &&
            nearModule != VK_NULL_HANDLE && farModule != VK_NULL_HANDLE && compositeModule != VK_NULL_HANDLE;

        bool ok = allLoaded;

        if (ok)
        {
            const VkPushConstantRange cocRange{
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .offset = 0,
                .size = RoundUpPushConstantSize(sizeof(DepthOfFieldCocPushConstants)) };
            const VkPipelineLayoutCreateInfo cocLayoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &mCocSetLayout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &cocRange };
            ok = vkCreatePipelineLayout(mDevice->Handle(), &cocLayoutInfo, nullptr, &mCocPipelineLayout) ==
                VK_SUCCESS;

            const VkPushConstantRange blurRange{
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .offset = 0,
                .size = RoundUpPushConstantSize(sizeof(DepthOfFieldBlurPushConstants)) };
            const VkPipelineLayoutCreateInfo blurLayoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &mBlurSetLayout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &blurRange };
            ok = ok &&
                vkCreatePipelineLayout(mDevice->Handle(), &blurLayoutInfo, nullptr, &mBlurPipelineLayout) ==
                VK_SUCCESS;

        // Composite reuses the CoC push constant shape - it recomputes CoC itself.
            const VkPipelineLayoutCreateInfo compositeLayoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &mCompositeSetLayout,
                .pushConstantRangeCount = 1,
                .pPushConstantRanges = &cocRange };
            ok = ok &&
                vkCreatePipelineLayout(mDevice->Handle(), &compositeLayoutInfo, nullptr, &mCompositePipelineLayout) ==
                VK_SUCCESS;

            if (!ok)
                MIR_LOG_CRITICAL("CreatePipelines: vkCreatePipelineLayout failed");
        }

        if (ok)
        {
            const VkPipelineColorBlendAttachmentState overwriteBlend{
                .blendEnable = VK_FALSE,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

            const auto makePipeline = [&](VkShaderModule fragModule, const char *fragEntry,
                VkPipelineLayout layout, const char *debugName, VkPipeline &outPipeline)
                {
                    const FullscreenPipelineDesc desc{
                        .vertModule = vertModule,
                        .fragModule = fragModule,
                        .fragEntry = fragEntry,
                        .layout = layout,
                        .blendAttachment = &overwriteBlend,
                        .colorFormat = kHdrColorFormat };
                    return CreateFullscreenPipeline(*mDevice, desc, debugName, outPipeline);
                };

            ok = makePipeline(cocModule, "cocMain", mCocPipelineLayout, "DepthOfField CoC pipeline", mCocPipeline);
            if (ok)
                ok = makePipeline(nearModule, "nearBlurMain", mBlurPipelineLayout, "DepthOfField near-blur pipeline",
                    mNearBlurPipeline);
            if (ok)
                ok = makePipeline(farModule, "farBlurMain", mBlurPipelineLayout, "DepthOfField far-blur pipeline",
                    mFarBlurPipeline);
            if (ok)
                ok = makePipeline(compositeModule, "compositeMain", mCompositePipelineLayout,
                    "DepthOfField composite pipeline", mCompositePipeline);
        }

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), cocModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), nearModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), farModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), compositeModule, nullptr);

        return ok;
    }

    namespace
    {
        bool CreateWorkingImage(Device &device, VkExtent2D extent, const char *debugName,
            VkImage &outImage, VmaAllocation &outAllocation, VkImageView &outView)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kHdrColorFormat,
                .extent{ extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(device.Allocator(), &imageInfo, &allocInfo, &outImage, &outAllocation, nullptr) !=
                VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateImages: vmaCreateImage failed for {}", debugName);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = outImage,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kHdrColorFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(device.Handle(), &viewInfo, nullptr, &outView) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateImages: vkCreateImageView failed for {}", debugName);
                return false;
            }

            device.NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(outImage), debugName);
            return true;
        }
    }

    bool DepthOfField::CreateImages(VkExtent2D extent)
    {
        mHalfExtent = { std::max(1u, extent.width / 2), std::max(1u, extent.height / 2) };

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            if (!CreateWorkingImage(*mDevice, mHalfExtent, std::format("DoF CoC/color ({})", frame).c_str(),
                mCocColorImages[frame], mCocColorAllocations[frame], mCocColorViews[frame]) ||
                !CreateWorkingImage(*mDevice, mHalfExtent, std::format("DoF near result ({})", frame).c_str(),
                    mNearResultImages[frame], mNearResultAllocations[frame], mNearResultViews[frame]) ||
                !CreateWorkingImage(*mDevice, mHalfExtent, std::format("DoF far result ({})", frame).c_str(),
                    mFarResultImages[frame], mFarResultAllocations[frame], mFarResultViews[frame]) ||
                !CreateWorkingImage(*mDevice, extent, std::format("DoF output ({})", frame).c_str(),
                    mOutputImages[frame], mOutputAllocations[frame], mOutputViews[frame]))
                return false;
        }

        WriteStaticDescriptors();
        return true;
    }

    void DepthOfField::DestroyImages()
    {
        const auto destroy = [this](VkImage *images, VmaAllocation *allocations, VkImageView *views)
            {
                for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
                {
                    if (views[frame] != VK_NULL_HANDLE)
                    {
                        vkDestroyImageView(mDevice->Handle(), views[frame], nullptr);
                        views[frame] = VK_NULL_HANDLE;
                    }
                    if (images[frame] != VK_NULL_HANDLE)
                    {
                        vmaDestroyImage(mDevice->Allocator(), images[frame], allocations[frame]);
                        images[frame] = VK_NULL_HANDLE;
                        allocations[frame] = VK_NULL_HANDLE;
                    }
                }
            };

        destroy(mCocColorImages, mCocColorAllocations, mCocColorViews);
        destroy(mNearResultImages, mNearResultAllocations, mNearResultViews);
        destroy(mFarResultImages, mFarResultAllocations, mFarResultViews);
        destroy(mOutputImages, mOutputAllocations, mOutputViews);
    }

    void DepthOfField::WriteStaticDescriptors()
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            const VkDescriptorImageInfo cocColorInfo{
                .sampler = mColorSampler,
                .imageView = mCocColorViews[frame],
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkWriteDescriptorSet blurWrite{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = mBlurReadSets[frame],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &cocColorInfo };
            vkUpdateDescriptorSets(mDevice->Handle(), 1, &blurWrite, 0, nullptr);

            const VkDescriptorImageInfo nearInfo{
                .sampler = mColorSampler,
                .imageView = mNearResultViews[frame],
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkDescriptorImageInfo farInfo{
                .sampler = mColorSampler,
                .imageView = mFarResultViews[frame],
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            const VkWriteDescriptorSet compositeWrites[]{
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mCompositeSets[frame],
                 .dstBinding = 2,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = &nearInfo },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = mCompositeSets[frame],
                 .dstBinding = 3,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .pImageInfo = &farInfo } };
            vkUpdateDescriptorSets(mDevice->Handle(), 2, compositeWrites, 0, nullptr);
        }
    }

    bool DepthOfField::Resize(VkExtent2D extent)
    {
        DestroyImages();
        return CreateImages(extent);
    }

    void DepthOfField::DestroyPipelines()
    {
        if (mCocPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mCocPipeline, nullptr);
            mCocPipeline = VK_NULL_HANDLE;
        }
        if (mNearBlurPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mNearBlurPipeline, nullptr);
            mNearBlurPipeline = VK_NULL_HANDLE;
        }
        if (mFarBlurPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mFarBlurPipeline, nullptr);
            mFarBlurPipeline = VK_NULL_HANDLE;
        }
        if (mCompositePipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mCompositePipeline, nullptr);
            mCompositePipeline = VK_NULL_HANDLE;
        }

        if (mCocPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mCocPipelineLayout, nullptr);
            mCocPipelineLayout = VK_NULL_HANDLE;
        }
        if (mBlurPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mBlurPipelineLayout, nullptr);
            mBlurPipelineLayout = VK_NULL_HANDLE;
        }
        if (mCompositePipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mCompositePipelineLayout, nullptr);
            mCompositePipelineLayout = VK_NULL_HANDLE;
        }
    }

    void DepthOfField::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyPipelines();
        DestroyImages();

        // Freeing the pool frees every set above too.
        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }

        if (mCocSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mCocSetLayout, nullptr);
            mCocSetLayout = VK_NULL_HANDLE;
        }
        if (mBlurSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mBlurSetLayout, nullptr);
            mBlurSetLayout = VK_NULL_HANDLE;
        }
        if (mCompositeSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mCompositeSetLayout, nullptr);
            mCompositeSetLayout = VK_NULL_HANDLE;
        }

        if (mColorSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mColorSampler, nullptr);
            mColorSampler = VK_NULL_HANDLE;
        }
        if (mDepthSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mDepthSampler, nullptr);
            mDepthSampler = VK_NULL_HANDLE;
        }
    }

    void DepthOfField::Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkImageView depthOrDistance,
        bool isLinearDepth, float nearClip, float farClip, VkExtent2D extent) const
    {
        const VkDescriptorImageInfo sourceImageInfos[]{
            { .sampler = mColorSampler, .imageView = color, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
            { .sampler = mDepthSampler, .imageView = depthOrDistance,
             .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };

        const auto writeSourceBindings = [&](VkDescriptorSet set)
            {
                const VkWriteDescriptorSet writes[]{
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &sourceImageInfos[0] },
                    { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                     .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                     .pImageInfo = &sourceImageInfos[1] } };
                vkUpdateDescriptorSets(mDevice->Handle(), 2, writes, 0, nullptr);
            };

        const auto drawFullscreen = [&](VkImage targetImage, VkImageView targetView, VkExtent2D targetExtent,
            VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSet set,
            const void *pushConstants, uint32_t pushConstantSize)
            {
                ColorAttachmentBarrier(cmd, targetImage);

                const VkRenderingAttachmentInfo attachment{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                    .imageView = targetView,
                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
                const VkRenderingInfo renderingInfo{
                    .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                    .renderArea{ .offset{ 0, 0 }, .extent = targetExtent },
                    .layerCount = 1,
                    .colorAttachmentCount = 1,
                    .pColorAttachments = &attachment };
                vkCmdBeginRendering(cmd, &renderingInfo);

                const VkViewport viewport{
                    .x = 0.0f, .y = 0.0f,
                    .width = static_cast<float>(targetExtent.width),
                    .height = static_cast<float>(targetExtent.height),
                    .minDepth = 0.0f, .maxDepth = 1.0f };
                const VkRect2D scissor{ .offset{ 0, 0 }, .extent = targetExtent };
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
                vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushConstantSize, pushConstants);

                vkCmdDraw(cmd, 3, 1, 0, 0);

                vkCmdEndRendering(cmd);
                ShaderReadOnlyBarrier(cmd, targetImage);
            };

        // Pass 1: downsample color+depth to half res, pack signed CoC into alpha.
        writeSourceBindings(mCocSets[frame]);
        const DepthOfFieldCocPushConstants cocPush{
            mFocusDistance, mAperture, mMaxBlur * 0.5f, nearClip, farClip, isLinearDepth ? 1u : 0u,
            InvExtent(extent), InvExtent(mHalfExtent) };
        drawFullscreen(mCocColorImages[frame], mCocColorViews[frame], mHalfExtent, mCocPipeline, mCocPipelineLayout,
            mCocSets[frame], &cocPush, sizeof(cocPush));

        // Pass 2/3: near and far gather blurs, both reading the CoC buffer pass 1 just wrote.
        const DepthOfFieldBlurPushConstants blurPush{ mMaxBlur * 0.5f, 0.0f, InvExtent(mHalfExtent) };
        drawFullscreen(mNearResultImages[frame], mNearResultViews[frame], mHalfExtent, mNearBlurPipeline,
            mBlurPipelineLayout, mBlurReadSets[frame], &blurPush, sizeof(blurPush));
        drawFullscreen(mFarResultImages[frame], mFarResultViews[frame], mHalfExtent, mFarBlurPipeline,
            mBlurPipelineLayout, mBlurReadSets[frame], &blurPush, sizeof(blurPush));

        // Pass 4: full-res composite - recomputes CoC itself (cheaper than an
        // extra full-res buffer) and blends sharp -> far -> near.
        writeSourceBindings(mCompositeSets[frame]);
        const DepthOfFieldCocPushConstants compositePush{
            mFocusDistance, mAperture, mMaxBlur, nearClip, farClip, isLinearDepth ? 1u : 0u,
            InvExtent(extent), InvExtent(extent) };
        drawFullscreen(mOutputImages[frame], mOutputViews[frame], extent, mCompositePipeline,
            mCompositePipelineLayout, mCompositeSets[frame], &compositePush, sizeof(compositePush));
    }

    bool DepthOfField::BuildReloadCandidate(ReloadCandidate &candidate, std::string *error)
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.vertexMain.spv"));
        VkShaderModule cocModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.cocMain.spv"));
        VkShaderModule nearModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.nearBlurMain.spv"));
        VkShaderModule farModule = CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.farBlurMain.spv"));
        VkShaderModule compositeModule =
            CreateShaderModule(mDevice->Handle(), ShaderPath("depthOfField.compositeMain.spv"));
        if (vertModule == VK_NULL_HANDLE || cocModule == VK_NULL_HANDLE || nearModule == VK_NULL_HANDLE ||
            farModule == VK_NULL_HANDLE || compositeModule == VK_NULL_HANDLE)
        {
            for (VkShaderModule module : { vertModule, cocModule, nearModule, farModule, compositeModule })
                if (module != VK_NULL_HANDLE)
                    vkDestroyShaderModule(mDevice->Handle(), module, nullptr);
            if (error != nullptr)
                *error = "a depth-of-field shader module is missing";
            return false;
        }

        const VkPipelineColorBlendAttachmentState overwriteBlend{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        const auto makePipeline = [&](VkShaderModule fragModule, const char *fragEntry,
            VkPipelineLayout layout, const char *debugName, VkPipeline &outPipeline)
            {
                const FullscreenPipelineDesc desc{
                    .vertModule = vertModule,
                    .fragModule = fragModule,
                    .fragEntry = fragEntry,
                    .layout = layout,
                    .blendAttachment = &overwriteBlend,
                    .colorFormat = kHdrColorFormat };
                return CreateFullscreenPipeline(*mDevice, desc, debugName, outPipeline);
            };

        bool ok = makePipeline(cocModule, "cocMain", mCocPipelineLayout,
            "DepthOfField CoC pipeline", candidate.mCoc);
        if (ok)
            ok = makePipeline(nearModule, "nearBlurMain", mBlurPipelineLayout,
                "DepthOfField near-blur pipeline", candidate.mNearBlur);
        if (ok)
            ok = makePipeline(farModule, "farBlurMain", mBlurPipelineLayout,
                "DepthOfField far-blur pipeline", candidate.mFarBlur);
        if (ok)
            ok = makePipeline(compositeModule, "compositeMain", mCompositePipelineLayout,
                "DepthOfField composite pipeline", candidate.mComposite);

        for (VkShaderModule module : { vertModule, cocModule, nearModule, farModule, compositeModule })
            vkDestroyShaderModule(mDevice->Handle(), module, nullptr);
        if (!ok && error != nullptr)
            *error = "a depth-of-field pipeline could not be created";
        return ok;
    }

    void DepthOfField::DestroyCandidate(ReloadCandidate &candidate)
    {
        if (mDevice == nullptr)
            return;
        for (VkPipeline *pipeline : { &candidate.mCoc, &candidate.mNearBlur, &candidate.mFarBlur,
                 &candidate.mComposite })
        {
            if (*pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }

    bool DepthOfField::PrepareShaderReload(std::string *error)
    {
        DiscardShaderReload();
        if (!BuildReloadCandidate(mReloadCandidate, error))
        {
            DiscardShaderReload();
            return false;
        }
        mReloadPrepared = true;
        return true;
    }

    void DepthOfField::CommitShaderReload()
    {
        if (!mReloadPrepared)
            return;
        for (VkPipeline *pipeline : { &mCocPipeline, &mNearBlurPipeline, &mFarBlurPipeline, &mCompositePipeline })
            if (*pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice->Handle(), *pipeline, nullptr);
        mCocPipeline = mReloadCandidate.mCoc;
        mNearBlurPipeline = mReloadCandidate.mNearBlur;
        mFarBlurPipeline = mReloadCandidate.mFarBlur;
        mCompositePipeline = mReloadCandidate.mComposite;
        mReloadCandidate = {};
        mReloadPrepared = false;
    }

    void DepthOfField::DiscardShaderReload()
    {
        DestroyCandidate(mReloadCandidate);
        mReloadPrepared = false;
    }
}
