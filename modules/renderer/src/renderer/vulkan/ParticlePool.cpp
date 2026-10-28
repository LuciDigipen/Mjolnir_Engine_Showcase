/**
 * @file ParticlePool.cpp
 * @author sumin.park
 * @brief Persistent per-instance GPU ranges and frame-safe indirect composition.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ParticlePool.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/SetupError.h>

#include <glm/matrix.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mir::vk
{
    void ParticlePool::Submit(std::span<const ParticleSubmission> inputs)
    {
        if (mSubmitted)
            for (const auto &pending : mPending) mStats.droppedSubmitSpawns += pending.spawns;
        mPending = {};
        uint32_t budget = kGpuParticleSpawnsPerFrame;
        if (inputs.size() > kGpuParticleInstances)
        {
            mStats.rejectedSubmissions += inputs.size();
            mSubmitted = true;
            return;
        }
        for (const auto &input : inputs)
        {
            if (input.index >= kGpuParticleInstances || input.generation == 0 ||
                input.capacity > kGpuParticleCapacity || input.capacity == 0 ||
                !std::isfinite(input.seconds) || input.seconds < 0 || input.seconds > 4.0f / 60.0f + 1e-6f ||
                !std::isfinite(input.lifetime) || input.lifetime <= 0 || input.lifetime > 120 ||
                !std::isfinite(input.size) || input.size <= 0 || input.size > 1024 ||
                !std::isfinite(input.velocity.x) || !std::isfinite(input.velocity.y) || !std::isfinite(input.velocity.z) ||
                !std::isfinite(input.color.x) || !std::isfinite(input.color.y) ||
                !std::isfinite(input.color.z) || !std::isfinite(input.color.w) ||
                input.color.w < 0 || input.color.w > 1 ||
                mPending[input.index].generation != 0)
            {
                ++mStats.rejectedSubmissions;
                continue;
            }
            mPending[input.index] = input;
            mPending[input.index].spawns = std::min(input.spawns, budget);
            budget -= mPending[input.index].spawns;
            mStats.droppedSubmitSpawns += input.spawns - mPending[input.index].spawns;
        }
        mSubmitted = true;
    }

    bool ParticlePool::MakeBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool read)
    {
        VmaAllocationInfo info{};
        const auto flags = host ? VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (read ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) : 0;
        if (!CreateBuffer(mDevice->Allocator(), size, usage, flags, buffer.handle, buffer.allocation, &info))
            return false;
        buffer.mapped = info.pMappedData;
        return true;
    }

    bool ParticlePool::Initialize(Device &device)
    {
        mDevice = &device;
        if (!MakeBuffer(mParticles, sizeof(GpuParticle) * kGpuParticleCapacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false) ||
            !MakeBuffer(mCounters, sizeof(Counters) * kGpuParticleInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
            return false;
        for (uint32_t f = 0; f < kFramesInFlight; ++f)
            if (!MakeBuffer(mInputs[f], sizeof(Parameters) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !MakeBuffer(mReadback[f], sizeof(Counters) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
                return false;
        const VkSamplerCreateInfo sampler{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        if (vkCreateSampler(device.Handle(), &sampler, nullptr, &mSampler) != VK_SUCCESS) return false;
        const VkDescriptorSetLayoutBinding bindings[]{
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        const VkDescriptorSetLayoutCreateInfo set{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 6, .pBindings = bindings};
        if (vkCreateDescriptorSetLayout(device.Handle(), &set, nullptr, &mSetLayout) != VK_SUCCESS) return false;
        const VkDescriptorPoolSize sizes[]{
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kFramesInFlight},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * kFramesInFlight}};
        const VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 2 * kFramesInFlight, .poolSizeCount = 2, .pPoolSizes = sizes};
        if (vkCreateDescriptorPool(device.Handle(), &pool, nullptr, &mPool) != VK_SUCCESS) return false;
        for (uint32_t f = 0; f < kFramesInFlight; ++f)
        {
            if (!AllocateDescriptorSet(device.Handle(), mPool, mSetLayout, mSets[f]) ||
                !AllocateDescriptorSet(device.Handle(), mPool, mSetLayout, mComputeSets[f])) return false;
            const VkDescriptorBufferInfo buffers[]{
                {mParticles.handle, 0, VK_WHOLE_SIZE}, {mCounters.handle, 0, VK_WHOLE_SIZE},
                {mInputs[f].handle, 0, VK_WHOLE_SIZE}, {mParticles.handle, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet writes[4]{};
            for (uint32_t i = 0; i < 4; ++i)
                writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[f],
                    .dstBinding = i + 2, .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buffers[i]};
            vkUpdateDescriptorSets(device.Handle(), 4, writes, 0, nullptr);
            for (auto &write : writes) write.dstSet = mComputeSets[f];
            vkUpdateDescriptorSets(device.Handle(), 4, writes, 0, nullptr);
        }
        const VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        const VkPipelineLayoutCreateInfo layout{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1, .pSetLayouts = &mSetLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &range};
        if (vkCreatePipelineLayout(device.Handle(), &layout, nullptr, &mLayout) != VK_SUCCESS) return false;
        const auto compute = CreateShaderModule(device.Handle(), ShaderPath("particlePool.computeMain.spv"));
        const auto vert = CreateShaderModule(device.Handle(), ShaderPath("particlePool.vertexMain.spv"));
        const auto frag = CreateShaderModule(device.Handle(), ShaderPath("particlePool.fragmentMain.spv"));
        const VkComputePipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = compute, .pName = "computeMain"}, .layout = mLayout};
        const VkPipelineColorBlendAttachmentState blend{
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
        const bool ok = compute && vert && frag &&
            vkCreateComputePipelines(device.Handle(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &mCompute) == VK_SUCCESS &&
            CreateFullscreenPipeline(device, {vert, frag, "fragmentMain", mLayout, &blend, kHdrColorFormat},
                "Particle indirect sprites", mDraw);
        if (compute) vkDestroyShaderModule(device.Handle(), compute, nullptr);
        if (vert) vkDestroyShaderModule(device.Handle(), vert, nullptr);
        if (frag) vkDestroyShaderModule(device.Handle(), frag, nullptr);
        device.NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mCounters.handle), "Particle GPU indirect counts");
        device.NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mParticles.handle), "Particle persistent pool");
        return ok;
    }

    void ParticlePool::Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess)
    {
        const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = src, .srcAccessMask = srcAccess, .dstStageMask = dst, .dstAccessMask = dstAccess};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &memory};
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    void ParticlePool::Simulate(Device &device, VkCommandBuffer cmd, uint32_t frame, uint64_t signal)
    {
        if (!mDevice)
        {
            if (std::none_of(mPending.begin(), mPending.end(), [](const auto &p) { return p.generation != 0; })) return;
            if (!Initialize(device))
                throw SetupError(ErrorCategory::System, "Initialize GPU particles", "particlePool",
                    "Vulkan pool or pipeline creation failed", "Rebuild the shaders and check the GPU driver and log.");
        }
        // BeginFrame has waited for this frame slot's timeline value, read only its small counters.
        if (mSignals[frame])
        {
            vmaInvalidateAllocation(device.Allocator(), mReadback[frame].allocation, 0, VK_WHOLE_SIZE);
            const auto *c = static_cast<const Counters *>(mReadback[frame].mapped);
            mStats.alive = mStats.spawned = mStats.dropped = 0;
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            {
                mStats.alive += c[i].alive;
                mStats.spawned += c[i].spawned;
                mStats.dropped += c[i].dropped;
            }
            mStats.completedFrame = mSignals[frame];
        }
        if (!mSubmitted)
            for (auto &p : mPending) { p.seconds = 0; p.spawns = 0; }
        // Removed generations release their ranges. Reuse is ordered after all previous draws
        // on the same queue by the barrier below, no host wait or per-instance allocation.
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            if (mSlots[i].allocated && (mPending[i].generation != mSlots[i].input.generation ||
                mPending[i].capacity != mSlots[i].input.capacity))
                mSlots[i].allocated = false;
        auto *parameters = static_cast<Parameters *>(mInputs[frame].mapped);
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
        {
            auto &slot = mSlots[i];
            auto input = mPending[i];
            const bool fresh = !slot.allocated;
            if (!slot.allocated && input.generation)
            {
                uint32_t start = 0;
                bool moved;
                do
                {
                    moved = false;
                    for (const auto &other : mSlots)
                        if (other.allocated && start < other.start + other.input.capacity &&
                            other.start < start + input.capacity)
                        { start = other.start + other.input.capacity; moved = true; }
                } while (moved && start + input.capacity <= kGpuParticleCapacity);
                if (start + input.capacity <= kGpuParticleCapacity)
                {
                    slot.start = start;
                    slot.anchor = input.anchor;
                    slot.allocated = true;
                }
                else
                {
                    ++mStats.rejectedSubmissions;
                    mStats.droppedSubmitSpawns += input.spawns;
                    input = {};
                }
            }
            slot.input = input;
            Parameters p{};
            if (slot.allocated)
            {
                const auto offset = TryCameraRelativeFloat(input.anchor, slot.anchor);
                const bool bounded = offset && glm::all(glm::lessThanEqual(glm::abs(*offset), glm::vec3(1024)));
                if (!bounded) mStats.droppedSubmitSpawns += input.spawns;
                p.range = {slot.start, input.capacity, input.generation, bounded ? input.spawns : 0};
                p.time = {input.seconds, input.lifetime, input.size, fresh ? 1.0f : 0.0f};
                p.velocity = glm::vec4(input.velocity, 0);
                p.color = input.color;
                p.emission = glm::vec4(bounded ? *offset : glm::vec3(0), 0);
            }
            parameters[i] = p;
        }
        vmaFlushAllocation(device.Allocator(), mInputs[frame].allocation, 0, VK_WHOLE_SIZE);
        device.BeginLabel(cmd, "Particle GPU update and recycling", {1, 0.5f, 0.1f, 1});
        Barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
        if (mFirst)
        {
            vkCmdFillBuffer(cmd, mCounters.handle, 0, VK_WHOLE_SIZE, 0);
            Barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
            mFirst = false;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompute);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mLayout, 0, 1, &mComputeSets[frame], 0, nullptr);
        vkCmdDispatch(cmd, 1, 1, 1);
        Barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0, 0, sizeof(Counters) * kGpuParticleInstances};
        vkCmdCopyBuffer(cmd, mCounters.handle, mReadback[frame].handle, 1, &copy);
        Barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        mSignals[frame] = signal;
        mSubmitted = false;
        device.EndLabel(cmd);
    }

    bool ParticlePool::Resize(VkExtent2D extent)
    {
        if (!mDevice) return true;
        for (auto &image : mOutput)
        {
            DestroyImage2D(*mDevice, image);
            if (!CreateImage2D(*mDevice, kHdrColorFormat, extent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Particle HDR output", image)) return false;
        }
        mExtent = extent;
        return true;
    }

    VkImageView ParticlePool::Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color,
        VkImageView depth, std::span<const RenderView> views, bool rayDistance, VkExtent2D extent, glm::vec2 jitter)
    {
        if (!mDevice || views.empty()) return color;
        // First allocation only, later resizes run through the renderer's existing idle resize path.
        if (mExtent.width == 0 && !Resize(extent))
            throw SetupError(ErrorCategory::System, "Allocate particle HDR output", "particlePool",
                "Vulkan image creation failed", "Check GPU memory availability and the driver log.");
        const VkDescriptorImageInfo images[]{
            {mSampler, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {mSampler, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i)
            writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = mSets[frame],
                .dstBinding = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &images[i]};
        vkUpdateDescriptorSets(mDevice->Handle(), 2, writes, 0, nullptr);
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
        mDevice->BeginLabel(cmd, "Particle indirect sprite composition", {1, 0.5f, 0.1f, 1});
        ColorAttachmentBarrier(cmd, mOutput[frame].image);
        BeginFullscreenPass(cmd, mOutput[frame].view, extent);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mDraw);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mLayout, 0, 1, &mSets[frame], 0, nullptr);
        Push push;
        push.sampling = {InvExtent(extent), 0, 1};
        const auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        vkCmdPushConstants(cmd, mLayout, stages, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        for (const auto &view : views)
        {
            const auto rect = ResolveViewRect(view.rect, extent);
            const VkViewport viewport{float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0, 1};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
            const auto inverse = glm::inverse(view.viewProj);
            // Projection scales cancel on normalization, these are the world camera axes.
            push.right = glm::vec4(glm::normalize(glm::vec3(inverse[0])), jitter.x);
            push.up = glm::vec4(glm::normalize(glm::vec3(inverse[1])), jitter.y);
            push.viewProj = view.viewProj;
            push.sampling = {InvExtent(extent), rayDistance ? 1.0f : 0.0f, 0};
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            {
                const auto &slot = mSlots[i];
                if (!slot.allocated) continue;
                const auto anchor = TryCameraRelativeFloat(slot.anchor, view.cameraPosition);
                if (!anchor) continue;
                push.anchorStart = glm::vec4(*anchor, float(slot.start));
                vkCmdPushConstants(cmd, mLayout, stages, 0, sizeof(push), &push);
                vkCmdDrawIndirect(cmd, mCounters.handle, i * sizeof(Counters), 1, sizeof(Counters));
            }
        }
        vkCmdEndRendering(cmd);
        ShaderReadOnlyBarrier(cmd, mOutput[frame].image);
        mDevice->EndLabel(cmd);
        return mOutput[frame].view;
    }

    void ParticlePool::Shutdown()
    {
        if (!mDevice) return;
        const auto device = mDevice->Handle();
        for (auto &image : mOutput) DestroyImage2D(*mDevice, image);
        for (auto *buffer : {&mParticles, &mCounters})
        {
            if (buffer->handle) vmaDestroyBuffer(mDevice->Allocator(), buffer->handle, buffer->allocation);
            *buffer = {};
        }
        for (uint32_t f = 0; f < kFramesInFlight; ++f)
            for (auto *buffer : {&mInputs[f], &mReadback[f]})
            {
                if (buffer->handle) vmaDestroyBuffer(mDevice->Allocator(), buffer->handle, buffer->allocation);
                *buffer = {};
            }
        if (mDraw) vkDestroyPipeline(device, mDraw, nullptr);
        if (mCompute) vkDestroyPipeline(device, mCompute, nullptr);
        if (mLayout) vkDestroyPipelineLayout(device, mLayout, nullptr);
        if (mPool) vkDestroyDescriptorPool(device, mPool, nullptr);
        if (mSetLayout) vkDestroyDescriptorSetLayout(device, mSetLayout, nullptr);
        if (mSampler) vkDestroySampler(device, mSampler, nullptr);
        mDraw = mCompute = VK_NULL_HANDLE;
        mLayout = VK_NULL_HANDLE;
        mPool = VK_NULL_HANDLE;
        mSetLayout = VK_NULL_HANDLE;
        mSampler = VK_NULL_HANDLE;
        mDevice = nullptr;
        mExtent = {};
        mSlots = {};
        mSignals[0] = mSignals[1] = 0;
        mFirst = true;
    }
}
