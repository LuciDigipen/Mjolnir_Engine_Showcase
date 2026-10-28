/**
 * @file ParticleDispatch.cpp
 * @author sumin.park
 * @brief Compute pipelines and synchronized update/compaction with delayed readback.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleDispatch.h"
#include "ParticleDataset.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>

namespace mir::gpu
{
    bool ParticleDispatch::Initialize(const FxDeviceContext &device, ParticleDataset &data)
    {
        mDevice = device.device;
        const VkDescriptorSetLayoutBinding bindings[]{
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 3, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(mDevice, &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * kFxFramesInFlight};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFxFramesInFlight, .poolSizeCount = 1, .pPoolSizes = &size};
        if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &mPool) != VK_SUCCESS) return false;
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
        {
            const VkDescriptorSetAllocateInfo allocate{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool, .descriptorSetCount = 1, .pSetLayouts = &mSetLayout};
            if (vkAllocateDescriptorSets(mDevice, &allocate, &mSets[f]) != VK_SUCCESS) return false;
            const VkDescriptorBufferInfo buffers[]{
                {data.mParticles.handle, 0, VK_WHOLE_SIZE}, {data.mCounters.handle, 0, VK_WHOLE_SIZE},
                {data.mInputs[f].handle, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet writes[3]{};
            for (uint32_t i = 0; i < 3; ++i)
                writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f], .dstBinding = i + 2,
                    .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
            vkUpdateDescriptorSets(mDevice, 3, writes, 0, nullptr);
        }
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout};
        if (vkCreatePipelineLayout(mDevice, &layout, nullptr, &mLayout) != VK_SUCCESS) return false;
        const auto shader = LoadShader(mDevice, "particlePool.computeMain.spv");
        const VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "computeMain"}, .layout = mLayout};
        const bool ok = shader && vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline, nullptr, &mPipeline) == VK_SUCCESS;
        if (shader) vkDestroyShaderModule(mDevice, shader, nullptr);
        return ok;
    }
    void ParticleDispatch::Record(const FxFrameContext &context, ParticleDataset &data)
    {
        if (!data.Prepare(context)) return;
        if (!mDevice && !Initialize(context.device, data))
            throw SetupError(ErrorCategory::System, "Initialize particle dispatch", "particlePool.computeMain.spv",
                "Vulkan compute pipeline creation failed", "Rebuild shaders and inspect the GPU driver log.");
        const auto cmd = context.commands;
        const auto frame = context.frame;
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        if (mFirst)
        {
            vkCmdFillBuffer(cmd, data.mCounters.handle, 0, VK_WHOLE_SIZE, 0);
            MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            mFirst = false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, 0, sizeof(ParticleDataset::Counters) * kGpuParticleInstances};
        vkCmdCopyBuffer(cmd, data.mCounters.handle, data.mReadback[frame].handle, 1, &copy);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        data.mSignals[frame] = context.signalValue;
        data.mSubmitted = false;
    }
    void ParticleDispatch::Shutdown()
    {
        if (!mDevice) return;
        if (mPipeline) vkDestroyPipeline(mDevice, mPipeline, nullptr);
        if (mLayout) vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        mPipeline = VK_NULL_HANDLE; mLayout = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE; mSetLayout = VK_NULL_HANDLE;
        mDevice = VK_NULL_HANDLE;
        mFirst = true;
    }
}
