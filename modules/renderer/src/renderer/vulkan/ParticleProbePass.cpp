/**
 * @file ParticleProbePass.cpp
 * @author sumin.park
 * @brief HDR composition experiment shared by raster and ray tracing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleProbePass.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <renderer/ParticleCompositionProbe.h>

#include <array>

namespace mir::vk
{
    bool ParticleProbePass::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;
        const VkSamplerCreateInfo sampler{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        if (vkCreateSampler(device.Handle(), &sampler, nullptr, &mSampler) != VK_SUCCESS)
            return false;
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(device.Handle(), &setInfo, nullptr, &mSetLayout) != VK_SUCCESS)
            return false;
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kFramesInFlight};
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight, .poolSizeCount = 1, .pPoolSizes = &poolSize};
        if (vkCreateDescriptorPool(device.Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
            return false;
        for (uint32_t f = 0; f < kFramesInFlight; ++f)
            if (!AllocateDescriptorSet(device.Handle(), mPool, mSetLayout, mSets[f]))
                return false;
        const VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ParticleProbeView)};
        const VkPipelineLayoutCreateInfo layout{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout,
            .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        if (vkCreatePipelineLayout(device.Handle(), &layout, nullptr, &mLayout) != VK_SUCCESS)
            return false;
        const auto vert = CreateShaderModule(device.Handle(), ShaderPath("particleProbe.vertexMain.spv"));
        const auto frag = CreateShaderModule(device.Handle(), ShaderPath("particleProbe.fragmentMain.spv"));
        const VkPipelineColorBlendAttachmentState overwrite{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        const bool ok = vert != VK_NULL_HANDLE && frag != VK_NULL_HANDLE &&
            CreateFullscreenPipeline(device, {vert, frag, "fragmentMain", mLayout, &overwrite, kHdrColorFormat},
                "Particle composition probe", mPipeline);
        if (vert) vkDestroyShaderModule(device.Handle(), vert, nullptr);
        if (frag) vkDestroyShaderModule(device.Handle(), frag, nullptr);
        return ok && Resize(extent);
    }

    bool ParticleProbePass::Resize(VkExtent2D extent)
    {
        if (mDevice == nullptr)
            return true;
        for (Image2D &image : mOutput)
        {
            DestroyImage2D(*mDevice, image);
            if (!CreateImage2D(*mDevice, kHdrColorFormat, extent,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    "Particle probe HDR output", image))
                return false;
        }
        return true;
    }

    void ParticleProbePass::Shutdown()
    {
        if (mDevice == nullptr)
            return;
        for (Image2D &image : mOutput) DestroyImage2D(*mDevice, image);
        const auto device = mDevice->Handle();
        if (mPipeline) vkDestroyPipeline(device, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(device, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(device, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(device, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(device, mSampler, nullptr);
        mPipeline = VK_NULL_HANDLE;
        mLayout = VK_NULL_HANDLE;
        mPool = VK_NULL_HANDLE;
        mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE;
        mDevice = nullptr;
    }

    VkImageView ParticleProbePass::Record(Device &device, VkCommandBuffer cmd, uint32_t frame,
        VkImageView color, VkImageView depth, std::span<const RenderView> views,
        bool rayDistance, VkExtent2D extent, glm::vec2 depthJitter)
    {
        if (!mAnchor || views.empty())
            return color;
        if (mDevice == nullptr && !Initialize(device, extent))
        {
            MIR_LOG_ERROR("Particle composition probe initialization failed; disabling the probe");
            Shutdown();
            mAnchor.reset();
            return color;
        }
        const VkDescriptorImageInfo infos[]{
            {mSampler, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {mSampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t i = 0; i < writes.size(); ++i)
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[frame],
                .dstBinding = i, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &infos[i]};
        vkUpdateDescriptorSets(device.Handle(), 2, writes.data(), 0, nullptr);

        device.BeginLabel(cmd, "Particle composition probe", {0.2f, 0.8f, 1.0f, 1.0f});
        ColorAttachmentBarrier(cmd, mOutput[frame].image);
        BeginFullscreenPass(cmd, mOutput[frame].view, extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        // Copy outside view rects too, never leave the HDR intermediate undefined.
        ParticleProbeView copy{};
        copy.sampling = glm::vec4(InvExtent(extent), 0.0f, 0.0f);
        vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(copy), &copy);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        for (const RenderView &view : views)
        {
            const VkRect2D rect = ResolveViewRect(view.rect, extent);
            const auto data = BuildParticleProbeView(*mAnchor, view.cameraPosition, view.viewProj,
                {float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height)},
                InvExtent(extent), rayDistance);
            if (!data)
                continue;
            ParticleProbeView push = *data;
            push.depthJitter = glm::vec4(depthJitter, 0.0f, 0.0f);
            const VkViewport viewport{float(rect.offset.x), float(rect.offset.y),
                float(rect.extent.width), float(rect.extent.height), 0.0f, 1.0f};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
        ShaderReadOnlyBarrier(cmd, mOutput[frame].image);
        device.EndLabel(cmd);
        return mOutput[frame].view;
    }
}
