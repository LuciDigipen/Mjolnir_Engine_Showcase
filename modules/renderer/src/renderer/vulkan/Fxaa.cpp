/**
 * @file Fxaa.cpp
 * @author Rahul Nair
 * @brief FXAA's single fullscreen pass - see Fxaa.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "Fxaa.h"
#include "Device.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <format>

namespace mir::vk
{
    bool Fxaa::Initialize(Device &device, VkFormat colorFormat)
    {
        mDevice = &device;
        mColorFormat = colorFormat;

        const VkSamplerCreateInfo samplerInfo{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 0.0f };

        if (vkCreateSampler(mDevice->Handle(), &samplerInfo, nullptr, &mSampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateSampler failed");
            return false;
        }

        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &setLayoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("Initialize: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kFramesInFlight };

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

        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("fxaa.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath("fxaa.fragmentMain.spv"));
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
            .size = sizeof(FxaaPushConstants) };
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
            .fragEntry = "fragmentMain",
            .layout = mPipelineLayout,
            .blendAttachment = &overwriteBlend,
            .colorFormat = colorFormat };
        const bool pipelineOk = CreateFullscreenPipeline(*mDevice, desc, "FXAA pipeline", mPipeline);

        vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);

        return pipelineOk;
    }

    bool Fxaa::CreateIntermediate(VkExtent2D extent)
    {
        if (HasIntermediate())
            return true;

        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            if (!CreateImage2D(*mDevice, mColorFormat, extent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    std::format("FXAA LDR intermediate ({})", frame).c_str(), mIntermediate[frame]))
            {
                DestroyIntermediate();
                return false;
            }

        return true;
    }

    void Fxaa::DestroyIntermediate()
    {
        if (mDevice == nullptr)
            return;

        for (Image2D &image : mIntermediate)
            DestroyImage2D(*mDevice, image);
    }

    bool Fxaa::Resize(VkExtent2D extent)
    {
        if (!HasIntermediate())
            return true;
        DestroyIntermediate();
        return CreateIntermediate(extent);
    }

    void Fxaa::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyIntermediate();

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
        // Freeing the pool frees its descriptor sets too.
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
        if (mSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(mDevice->Handle(), mSampler, nullptr);
            mSampler = VK_NULL_HANDLE;
        }
    }

    void Fxaa::Record(VkCommandBuffer cmd, uint32_t frame, VkImageView source, VkExtent2D extent)
    {
        const VkDescriptorImageInfo imageInfo{
            .sampler = mSampler, .imageView = source, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        const VkWriteDescriptorSet write{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mSets[frame],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &imageInfo };
        vkUpdateDescriptorSets(mDevice->Handle(), 1, &write, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout, 0, 1, &mSets[frame], 0, nullptr);

        const FxaaPushConstants pushData{ InvExtent(extent) };
        vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), &pushData);

        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
}
