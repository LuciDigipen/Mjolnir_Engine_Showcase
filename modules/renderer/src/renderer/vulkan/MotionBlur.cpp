/**
 * @file MotionBlur.cpp
 * @author Rahul Nair
 * @brief Motion blur's composite - see MotionBlur.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MotionBlur.h"
#include "Device.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <array>
#include <format>

namespace mir::vk
{
    bool MotionBlur::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;

        const VkSamplerCreateInfo velocitySamplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0f };
        if (vkCreateSampler(mDevice->Handle(), &velocitySamplerInfo, nullptr, &mVelocitySampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateSampler (velocity) failed");
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

        const VkDescriptorSetLayoutBinding bindings[]{
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2,
            .pBindings = bindings };
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 2 * kFramesInFlight };
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };
        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorPool failed");
            return false;
        }

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            if (!AllocateDescriptorSet(mDevice->Handle(), mPool, mSetLayout, mSets[frame]))
            {
                MIR_LOG_CRITICAL("Initialize: descriptor set allocation failed for frame {}", frame);
                return false;
            }

        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("motionBlur.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath("motionBlur.compositeMain.spv"));
        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
            return false;
        }

        const VkPushConstantRange pushRange{
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(MotionBlurPushConstants) };
        const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &mSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushRange };
        if (vkCreatePipelineLayout(mDevice->Handle(), &pipelineLayoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreatePipelineLayout failed");
            vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
            return false;
        }

        const VkPipelineColorBlendAttachmentState overwriteBlend{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        const FullscreenPipelineDesc desc{
            .vertModule = vertModule,
            .fragModule = fragModule,
            .fragEntry = "compositeMain",
            .layout = mPipelineLayout,
            .blendAttachment = &overwriteBlend,
            .colorFormat = kHdrColorFormat };
        const bool pipelineOk = CreateFullscreenPipeline(*mDevice, desc, "MotionBlur composite pipeline", mPipeline);

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);

        if (!pipelineOk)
            return false;

        return CreateImages(extent);
    }

    bool MotionBlur::CreateImages(VkExtent2D extent)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
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

            if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
                &mOutputImages[frame], &mOutputAllocations[frame], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateImages: vmaCreateImage failed for MotionBlur output {}", frame);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mOutputImages[frame],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kHdrColorFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &mOutputViews[frame]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateImages: vkCreateImageView failed for MotionBlur output {}", frame);
                return false;
            }

            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mOutputImages[frame]),
                std::format("MotionBlur output ({})", frame).c_str());
        }

        return true;
    }

    void MotionBlur::DestroyImages()
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            if (mOutputViews[frame] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice->Handle(), mOutputViews[frame], nullptr);
                mOutputViews[frame] = VK_NULL_HANDLE;
            }
            if (mOutputImages[frame] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mDevice->Allocator(), mOutputImages[frame], mOutputAllocations[frame]);
                mOutputImages[frame] = VK_NULL_HANDLE;
                mOutputAllocations[frame] = VK_NULL_HANDLE;
            }
        }
    }

    bool MotionBlur::Resize(VkExtent2D extent)
    {
        DestroyImages();
        return CreateImages(extent);
    }

    void MotionBlur::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyImages();

        if (mPipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice->Handle(), mPipeline, nullptr);
            mPipeline = VK_NULL_HANDLE;
        }
        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(mDevice->Handle(), mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }
        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }
        if (mSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
            mSetLayout = VK_NULL_HANDLE;
        }
        if (mColorSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mColorSampler, nullptr);
            mColorSampler = VK_NULL_HANDLE;
        }
        if (mVelocitySampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mVelocitySampler, nullptr);
            mVelocitySampler = VK_NULL_HANDLE;
        }
    }

    void MotionBlur::Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkImageView velocity,
        VkExtent2D extent)
    {
        const VkDescriptorImageInfo velocityInfo{
            .sampler = mVelocitySampler, .imageView = velocity,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo colorInfo{
            .sampler = mColorSampler, .imageView = color,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const std::array<VkWriteDescriptorSet, 2> writes{ {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame], .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &velocityInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame], .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &colorInfo } } };
        vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        ColorAttachmentBarrier(cmd, mOutputImages[frame]);

        const VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mOutputViews[frame],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{ 0, 0 }, .extent = extent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment };
        vkCmdBeginRendering(cmd, &renderingInfo);

        const VkViewport viewport{
            .x = 0.0f, .y = 0.0f,
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .minDepth = 0.0f, .maxDepth = 1.0f };
        const VkRect2D scissor{ .offset{ 0, 0 }, .extent = extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &mSets[frame], 0, nullptr);

        const MotionBlurPushConstants pushData{ mShutterStrength, mSampleCount, InvExtent(extent) };
        vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), &pushData);

        vkCmdDraw(cmd, 3, 1, 0, 0);

        vkCmdEndRendering(cmd);
        ShaderReadOnlyBarrier(cmd, mOutputImages[frame]);
    }

    bool MotionBlur::PrepareShaderReload(std::string *error)
    {
        DiscardShaderReload();
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("motionBlur.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath("motionBlur.compositeMain.spv"));
        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
            if (error != nullptr)
                *error = "a motion blur shader module is missing";
            return false;
        }

        const VkPipelineColorBlendAttachmentState overwriteBlend{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
        const FullscreenPipelineDesc desc{
            .vertModule = vertModule,
            .fragModule = fragModule,
            .fragEntry = "compositeMain",
            .layout = mPipelineLayout,
            .blendAttachment = &overwriteBlend,
            .colorFormat = kHdrColorFormat };
        const bool ok = CreateFullscreenPipeline(*mDevice, desc, "MotionBlur composite pipeline", mReloadPipeline);
        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
        if (!ok)
        {
            mReloadPipeline = VK_NULL_HANDLE;
            if (error != nullptr)
                *error = "the motion blur pipeline could not be created";
            return false;
        }
        mReloadPrepared = true;
        return true;
    }

    void MotionBlur::CommitShaderReload()
    {
        if (!mReloadPrepared)
            return;
        if (mPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), mPipeline, nullptr);
        mPipeline = mReloadPipeline;
        mReloadPipeline = VK_NULL_HANDLE;
        mReloadPrepared = false;
    }

    void MotionBlur::DiscardShaderReload()
    {
        if (mReloadPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), mReloadPipeline, nullptr);
        mReloadPipeline = VK_NULL_HANDLE;
        mReloadPrepared = false;
    }
}
