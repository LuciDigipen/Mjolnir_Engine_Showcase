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

#include <algorithm>
#include <cstring>
#include <set>
#include <utility>

namespace mir::gpu
{
    bool ParticleDispatch::Initialize(const FxDeviceContext &device, ParticleDataset &data)
    {
        mDevice = device.device;
        const VkDescriptorSetLayoutBinding bindings[]{
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 4, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(mDevice, &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFxFramesInFlight};
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
                {data.mInputs[f].handle, 0, VK_WHOLE_SIZE}, {data.mBirths[f].handle, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t i = 0; i < 4; ++i)
                writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f], .dstBinding = i == 3 ? 6u : i + 2,
                    .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
            vkUpdateDescriptorSets(mDevice, 4, writes, 0, nullptr);
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
        return vkCreatePipelineLayout(mDevice, &layout, nullptr, &mLayout) == VK_SUCCESS;
    }
    VkPipeline ParticleDispatch::Pipeline(uint64_t key, std::span<const std::byte> code)
    {
        auto invalid = []
        {
            throw SetupError(ErrorCategory::BuildOutput, "Load cooked particle script", "particle script dependency",
                "Missing, malformed or conflicting cooked SPIR-V", "Recook the particle emitter assets.");
        };
        uint32_t magic = 0;
        if (code.size() >= sizeof(magic)) std::memcpy(&magic, code.data(), sizeof(magic));
        if (!key || code.size() < 5 * sizeof(uint32_t) || code.size() % sizeof(uint32_t) != 0 || magic != 0x07230203u)
            invalid();
        if (auto found = mPipelines.find(key); found != mPipelines.end())
        {
            if (!std::equal(code.begin(), code.end(), found->second.code.begin(), found->second.code.end())) invalid();
            return found->second.pipeline;
        }
        // Own aligned words for Vulkan, byte-vector storage has no alignment contract.
        std::vector<uint32_t> words(code.size() / sizeof(uint32_t));
        std::memcpy(words.data(), code.data(), code.size());
        const VkShaderModuleCreateInfo module{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = code.size(), .pCode = words.data()};
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(mDevice, &module, nullptr, &shader) != VK_SUCCESS) invalid();
        const VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "computeMain"}, .layout = mLayout};
        VkPipeline handle = VK_NULL_HANDLE;
        const auto result = vkCreateComputePipelines(mDevice, VK_NULL_HANDLE, 1, &pipeline, nullptr, &handle);
        vkDestroyShaderModule(mDevice, shader, nullptr);
        if (result != VK_SUCCESS)
        {
            if (handle) vkDestroyPipeline(mDevice, handle, nullptr);
            throw SetupError(ErrorCategory::System, "Create particle script pipeline", "cooked particle SPIR-V",
                "Vulkan compute pipeline creation failed", "Recook particle assets and inspect the GPU driver log.");
        }
        ScriptPipeline entry;
        entry.code.assign(code.begin(), code.end());
        entry.pipeline = handle;
        mPipelines.emplace(key, std::move(entry));
        return handle;
    }
    void ParticleDispatch::Record(const FxFrameContext &context, ParticleDataset &data)
    {
        if (!data.Prepare(context)) return;
        if (!mDevice && !Initialize(context.device, data))
            throw SetupError(ErrorCategory::System, "Initialize particle dispatch", "particle dispatch resources",
                "Vulkan descriptor or pipeline layout creation failed", "Inspect the GPU driver log.");
        const auto cmd = context.commands;
        const auto frame = context.frame;
        const auto completed = data.mSignals[frame];
        std::set<uint64_t> used;
        for (const auto &slot : data.mSlots)
            if (slot.allocated)
            {
                Pipeline(slot.input.scriptKey, slot.input.scriptCode);
                used.insert(slot.input.scriptKey);
            }
        // This frame slot's wait proves completion of at least its previous signal.
        // Keep every unused pipeline until its final recorded use has completed.
        for (auto it = mPipelines.begin(); it != mPipelines.end();)
            if (!used.contains(it->first) && it->second.lastUse <= completed)
            {
                vkDestroyPipeline(mDevice, it->second.pipeline, nullptr);
                it = mPipelines.erase(it);
            }
            else ++it;
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        if (mFirst)
        {
            vkCmdFillBuffer(cmd, data.mCounters.handle, 0, VK_WHOLE_SIZE, 0);
            mFirst = false;
        }
        else
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
                if (!data.mSlots[i].allocated)
                    vkCmdFillBuffer(cmd, data.mCounters.handle, i * sizeof(ParticleDataset::Counters),
                        sizeof(ParticleDataset::Counters), 0);
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            if (data.mSlots[i].allocated)
            {
                auto &script = mPipelines.at(data.mSlots[i].input.scriptKey);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, script.pipeline);
                vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(i), &i);
                vkCmdDispatch(cmd, 1, 1, 1);
                script.lastUse = context.signalValue;
                // Descriptors span the pool, order dispatches even though emitter ranges are disjoint.
                MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, 0, sizeof(ParticleDataset::Counters) * kGpuParticleInstances};
        vkCmdCopyBuffer(cmd, data.mCounters.handle, data.mReadback[frame].handle, 1, &copy);
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            if (data.mSlots[i].allocated)
            {
                const VkBufferCopy sample{data.mSlots[i].start * sizeof(GpuParticle),
                    sizeof(ParticleDataset::Counters) * kGpuParticleInstances + i * sizeof(GpuParticle), sizeof(GpuParticle)};
                vkCmdCopyBuffer(cmd, data.mParticles.handle, data.mReadback[frame].handle, 1, &sample);
            }
        MemoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        data.mSignals[frame] = context.signalValue;
        data.mSubmitted = false;
    }
    void ParticleDispatch::Shutdown()
    {
        if (!mDevice) return;
        for (const auto &[key, script] : mPipelines) vkDestroyPipeline(mDevice, script.pipeline, nullptr);
        mPipelines.clear();
        if (mLayout) vkDestroyPipelineLayout(mDevice, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(mDevice, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(mDevice, mSetLayout, nullptr);
        mLayout = VK_NULL_HANDLE; mPool = VK_NULL_HANDLE; mSetLayout = VK_NULL_HANDLE;
        mDevice = VK_NULL_HANDLE;
        mFirst = true;
    }
}
