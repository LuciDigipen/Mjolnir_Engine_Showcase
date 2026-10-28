/**
 * @file ParticleSpriteRenderer.cpp
 * @author sumin.park
 * @brief Indirect sprite rendering through the renderer-owned composition hook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleSpriteRenderer.h"
#include "ParticleDataset.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>
#include <renderer/VulkanRenderer.h>

#include <glm/matrix.hpp>

namespace mir::gpu
{
    bool ParticleSpriteRenderer::Initialize(const FxRenderContext &context, const ParticleDataset &data)
    {
        mDevice = context.frame.device.device;
        const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (vkCreateSampler(mDevice, &sampler, nullptr, &mSampler) != VK_SUCCESS) return false;
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(mDevice, &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kFxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFxFramesInFlight}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFxFramesInFlight, .poolSizeCount = 2, .pPoolSizes = sizes};
        if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &mPool) != VK_SUCCESS) return false;
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
        {
            const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool, .descriptorSetCount = 1, .pSetLayouts = &mSetLayout};
            if (vkAllocateDescriptorSets(mDevice, &allocate, &mSets[f]) != VK_SUCCESS) return false;
            const VkDescriptorBufferInfo buffer{data.mParticles.handle, 0, VK_WHOLE_SIZE};
            const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f],
                .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer};
            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        return vkCreatePipelineLayout(mDevice, &layout, nullptr, &mLayout) == VK_SUCCESS &&
            CreateSpritePipeline(mDevice, mLayout, context.colorFormat, mPipeline);
    }
    void ParticleSpriteRenderer::Record(const FxRenderContext &context, const ParticleDataset &data)
    {
        if (!data.Initialized() || context.views.empty()) return;
        if (!mDevice && !Initialize(context, data))
            throw SetupError(ErrorCategory::System, "Initialize particle sprite renderer", "particlePool",
                "Vulkan graphics pipeline creation failed", "Rebuild shaders and inspect the GPU driver log.");
        const auto cmd = context.frame.commands;
        const auto frame = context.frame.frame;
        const auto color = context.sceneColor;
        const auto depth = context.depth;
        const auto extent = context.extent;
        const auto views = context.views;
        const auto jitter = context.jitter;
        const auto rayDistance = context.rayDistance;
        const VkDescriptorImageInfo images[]{
            {mSampler, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {mSampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i)
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[frame],
                .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &images[i]};
        vkUpdateDescriptorSets(mDevice, 2, writes, 0, nullptr);
        struct Push
        {
            glm::mat4 viewProj{1};
            glm::vec4 anchorStart{}, right{}, up{}, sampling{};
        };
        static_assert(sizeof(Push) == 128);
        static_assert(offsetof(Push, anchorStart) == 64);
        static_assert(offsetof(Push, right) == 80);
        static_assert(offsetof(Push, up) == 96);
        static_assert(offsetof(Push, sampling) == 112);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        Push push;
        const auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (const auto &view : views)
        {
            const auto rect = ResolveRect(view.rect, extent);
            const VkViewport viewport{float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            const auto inverse = glm::inverse(view.viewProj);
            // Projection scales cancel on normalization, these are the world camera axes.
            push.right = glm::vec4(glm::normalize(glm::vec3(inverse[0])), jitter.x);
            push.up = glm::vec4(glm::normalize(glm::vec3(inverse[1])), jitter.y);
            push.viewProj = view.viewProj;
            push.sampling = {glm::vec2(1.0f / extent.width, 1.0f / extent.height), rayDistance ? 1.0f : 0.0f, 0};
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            {
                const auto &slot = data.mSlots[i];
                if (!slot.allocated) continue;
                const auto anchor = TryCameraRelativeFloat(slot.anchor, view.cameraPosition);
                if (!anchor) continue;
                push.anchorStart = glm::vec4(*anchor, float(slot.start));
                vkCmdPushConstants(cmd, mLayout, stages, 0, sizeof(push), &push);
                vkCmdDrawIndirect(cmd, data.mCounters.handle, i * sizeof(ParticleDataset::Counters), 1, sizeof(ParticleDataset::Counters));
            }
        }
    }
    void ParticleSpriteRenderer::Shutdown()
    {
        if (!mDevice) return;
        if (mPipeline) vkDestroyPipeline(mDevice, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(mDevice, mSampler, nullptr);
        mPipeline = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE; mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE; mDevice = VK_NULL_HANDLE;
    }
}
