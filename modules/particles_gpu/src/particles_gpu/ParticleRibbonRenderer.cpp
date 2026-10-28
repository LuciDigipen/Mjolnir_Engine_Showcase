/**
 * @file ParticleRibbonRenderer.cpp
 * @author sumin.park
 * @brief Bounded per-view ribbon uploads with sampled opaque depth.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleRibbonRenderer.h"
#include "particles_gpu/ParticleRibbonGeometry.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>
#include <renderer/VulkanRenderer.h>

#include <glm/matrix.hpp>

#include <cstring>

namespace mir::gpu
{
    namespace
    {
        void RequireRibbon(bool success)
        {
            if (!success) throw SetupError(ErrorCategory::System, "Initialize ribbon renderer", "CPU ribbon FX",
                "Vulkan ribbon resource creation failed", "Rebuild shaders and inspect the GPU driver log.");
        }
    }
    void ParticleRibbonRenderer::Submit(std::span<const RibbonSubmission> ribbons)
    {
        mRibbons.clear();
        size_t remaining = kGpuParticleCapacity;
        for (const auto &ribbon : ribbons)
        {
            if (mRibbons.size() == kGpuParticleInstances || ribbon.points.size() > remaining) break;
            mRibbons.push_back(ribbon);
            remaining -= ribbon.points.size();
        }
    }
    void ParticleRibbonRenderer::Initialize(const FxFrameContext &context)
    {
        mDevice = context.device.device;
        mAllocator = context.device.allocator;
        const VkDescriptorSetLayoutBinding bindings[]{
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {8, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = bindings};
        RequireRibbon(vkCreateDescriptorSetLayout(mDevice, &set, nullptr, &mSetLayout) == VK_SUCCESS);
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFxFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFxFramesInFlight}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFxFramesInFlight, .poolSizeCount = 2, .pPoolSizes = sizes};
        RequireRibbon(vkCreateDescriptorPool(mDevice, &pool, nullptr, &mPool) == VK_SUCCESS);
        const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        RequireRibbon(vkCreateSampler(mDevice, &sampler, nullptr, &mSampler) == VK_SUCCESS);
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
        {
            VmaAllocationInfo info{};
            RequireRibbon(CreateParticleBuffer(mAllocator, kRibbonVerticesPerFrame * sizeof(RibbonVertex),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
                mBuffers[f], mMemory[f], info));
            mMapped[f] = info.pMappedData;
            NameBuffer(context.device, mBuffers[f], "CPU ribbon vertices");
            const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool, .descriptorSetCount = 1, .pSetLayouts = &mSetLayout};
            RequireRibbon(vkAllocateDescriptorSets(mDevice, &allocate, &mSets[f]) == VK_SUCCESS);
            const VkDescriptorBufferInfo buffer{mBuffers[f], 0, VK_WHOLE_SIZE};
            const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f],
                .dstBinding = 8, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffer};
            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        RequireRibbon(vkCreatePipelineLayout(mDevice, &layout, nullptr, &mLayout) == VK_SUCCESS);
    }
    void ParticleRibbonRenderer::Prepare(const FxFrameContext &context)
    {
        if (!HasData()) return;
        if (!mDevice) Initialize(context);
        // The renderer has waited for this frame slot before calling Prepare.
        mUsed[context.frame] = 0;
    }
    void ParticleRibbonRenderer::Record(const FxRenderContext &context)
    {
        if (!HasData() || !mDevice) return;
        if (!mPipeline) RequireRibbon(CreateSpritePipeline(mDevice, mLayout, context.colorFormat, mPipeline, true));
        const uint32_t frame = context.frame.frame;
        const auto cmd = context.frame.commands;
        const VkDescriptorImageInfo depth{mSampler, context.depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[frame],
            .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &depth};
        vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        struct Push { glm::mat4 viewProj; glm::vec4 anchorStart, right, up, sampling; };
        static_assert(sizeof(Push) == 128);
        for (const auto &view : context.views)
        {
            const auto inverse = glm::inverse(view.viewProj);
            const auto vertices = BuildRibbonGeometry(mRibbons, view.cameraPosition, glm::vec3(inverse[0]),
                kRibbonVerticesPerFrame - mUsed[frame]);
            if (vertices.empty()) continue;
            const auto bytes = vertices.size() * sizeof(RibbonVertex);
            const auto offset = mUsed[frame] * sizeof(RibbonVertex);
            std::memcpy(static_cast<std::byte *>(mMapped[frame]) + offset, vertices.data(), bytes);
            vmaFlushAllocation(mAllocator, mMemory[frame], offset, bytes);
            const auto rect = ResolveRect(view.rect, context.extent);
            const VkViewport viewport{float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            const Push push{view.viewProj, {}, {0,0,0,context.jitter.x}, {0,0,0,context.jitter.y},
                {1.0f / context.extent.width, 1.0f / context.extent.height, context.rayDistance ? 1.0f : 0.0f, 0}};
            vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, static_cast<uint32_t>(vertices.size()), 1, mUsed[frame], 0);
            mUsed[frame] += static_cast<uint32_t>(vertices.size());
        }
    }
    void ParticleRibbonRenderer::Shutdown()
    {
        if (!mDevice) return;
        if (mPipeline) vkDestroyPipeline(mDevice, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(mDevice, mSampler, nullptr);
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
            if (mBuffers[f]) vmaDestroyBuffer(mAllocator, mBuffers[f], mMemory[f]);
        mDevice = VK_NULL_HANDLE;
        mPipeline = VK_NULL_HANDLE;
        mRibbons.clear();
    }
}
