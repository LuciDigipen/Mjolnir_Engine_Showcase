/**
 * @file FxCompositionPass.cpp
 * @author sumin.park
 * @brief HDR scene copy, generic FX composition and renderer-owned image transitions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "FxCompositionPass.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/SetupError.h>

#include <vk_mem_alloc.h>

namespace mir::vk
{
    bool FxCompositionPass::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;
        const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (vkCreateSampler(device.Handle(), &sampler, nullptr, &mSampler) != VK_SUCCESS) return false;
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &binding};
        if (vkCreateDescriptorSetLayout(device.Handle(), &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight, .poolSizeCount = 1, .pPoolSizes = &size};
        if (vkCreateDescriptorPool(device.Handle(), &pool, nullptr, &mPool) != VK_SUCCESS) return false;
        for (auto &descriptor : mSets)
            if (!AllocateDescriptorSet(device.Handle(), mPool, mSetLayout, descriptor)) return false;
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout};
        if (vkCreatePipelineLayout(device.Handle(), &layout, nullptr, &mLayout) != VK_SUCCESS) return false;
        const auto vert = CreateShaderModule(device.Handle(), ShaderPath("fxComposition.vertexMain.spv"));
        const auto frag = CreateShaderModule(device.Handle(), ShaderPath("fxComposition.fragmentMain.spv"));
        const VkPipelineColorBlendAttachmentState blend{.blendEnable = VK_FALSE, .colorWriteMask = 15};
        const bool ok = vert && frag && CreateFullscreenPipeline(device,
            {vert, frag, "fragmentMain", mLayout, &blend, kHdrColorFormat}, "FX composition copy", mPipeline);
        if (vert) vkDestroyShaderModule(device.Handle(), vert, nullptr);
        if (frag) vkDestroyShaderModule(device.Handle(), frag, nullptr);
        return ok && Resize(extent);
    }
    VkImageView FxCompositionPass::Record(Device &device, IFxSystem *system, const FxRenderContext &context)
    {
        if (!system || !system->HasRenderData() || context.views.empty()) return context.sceneColor;
        if (!mDevice && !Initialize(device, context.extent))
            throw SetupError(ErrorCategory::System, "Initialize FX composition", "fxComposition",
                "Vulkan composition pipeline or image allocation failed", "Rebuild shaders and check GPU memory and driver logs.");
        system->PrepareDraw(context);
        const auto frame = context.frame.frame;
        const auto cmd = context.frame.commands;
        const VkDescriptorImageInfo image{mSampler, context.sceneColor, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mSets[frame], .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
        vkUpdateDescriptorSets(device.Handle(), 1, &write, 0, nullptr);
        device.BeginLabel(cmd, "FX composition", {1, 0.5f, 0.1f, 1});
        ColorAttachmentBarrier(cmd, mOutput[frame].image);
        BeginFullscreenPass(cmd, mOutput[frame].view, context.extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        system->RecordDraw(context);
        vkCmdEndRendering(cmd);
        ShaderReadOnlyBarrier(cmd, mOutput[frame].image);
        system->FinishDraw(context);
        device.EndLabel(cmd);
        return mOutput[frame].view;
    }
    bool FxCompositionPass::Resize(VkExtent2D extent)
    {
        if (!mDevice) return true;
        for (auto &image : mOutput)
        {
            DestroyImage2D(*mDevice, image);
            if (!CreateImage2D(*mDevice, kHdrColorFormat, extent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "FX HDR composition", image)) return false;
        }
        return true;
    }
    uint64_t FxCompositionPass::MemoryBytes() const
    {
        uint64_t bytes=0;
        if(mDevice) for(const auto &image:mOutput) if(image.allocation)
        {
            VmaAllocationInfo info{};
            vmaGetAllocationInfo(mDevice->Allocator(),image.allocation,&info);
            bytes+=info.size;
        }
        return bytes;
    }
    void FxCompositionPass::Shutdown()
    {
        if (!mDevice) return;
        for (auto &image : mOutput) DestroyImage2D(*mDevice, image);
        const auto device = mDevice->Handle();
        if (mPipeline) vkDestroyPipeline(device, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(device, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(device, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(device, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(device, mSampler, nullptr);
        mPipeline = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE; mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE; mDevice = nullptr;
    }
}
