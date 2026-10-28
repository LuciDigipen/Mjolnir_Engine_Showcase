/**
 * @file ParticleDataset.cpp
 * @author sumin.park
 * @brief Bounded range allocation and frame-slot uploads without rendering ownership.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ParticleDataset.h"
#include "ParticleVulkan.h"

#include <core/log/SetupError.h>

#include <algorithm>
#include <cmath>

namespace mir::gpu
{
    void ParticleDataset::Submit(std::span<const ParticleSubmission> inputs)
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
                !std::isfinite(input.size) || input.size < 0 || input.size > 1024 ||
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

    bool ParticleDataset::MakeBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool read)
    {
        VmaAllocationInfo info{};
        const auto flags = host ? VMA_ALLOCATION_CREATE_MAPPED_BIT |
            (read ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) : 0;
        if (!CreateParticleBuffer(mDevice->allocator, size, usage, flags, buffer.handle, buffer.allocation, info))
            return false;
        buffer.mapped = info.pMappedData;
        return true;
    }

    bool ParticleDataset::Initialize(const FxDeviceContext &device)
    {
        mDevice = device;
        if (!MakeBuffer(mParticles, sizeof(GpuParticle) * kGpuParticleCapacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false) ||
            !MakeBuffer(mCounters, sizeof(Counters) * kGpuParticleInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
            return false;
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
            if (!MakeBuffer(mInputs[f], sizeof(Parameters) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !MakeBuffer(mReadback[f], sizeof(Counters) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
                return false;
        NameBuffer(device, mCounters.handle, "Particle GPU indirect counts");
        NameBuffer(device, mParticles.handle, "Particle persistent pool");
        return true;
    }

    bool ParticleDataset::Prepare(const FxFrameContext &context)

    {
        const auto &device = context.device;
        const auto frame = context.frame;
        if (!mDevice)
        {
            if (std::none_of(mPending.begin(), mPending.end(), [](const auto &p) { return p.generation != 0; })) return false;
            if (!Initialize(device))
                throw SetupError(ErrorCategory::System, "Initialize GPU particles", "particlePool",
                    "Vulkan pool or pipeline creation failed", "Rebuild the shaders and check the GPU driver and log.");
        }
        // BeginFrame has waited for this frame slot's timeline value, read only its small counters.
        if (mSignals[frame])
        {
            vmaInvalidateAllocation(device.allocator, mReadback[frame].allocation, 0, VK_WHOLE_SIZE);
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
        vmaFlushAllocation(device.allocator, mInputs[frame].allocation, 0, VK_WHOLE_SIZE);
        return true;
    }

    void ParticleDataset::Shutdown()
    {
        if (!mDevice) return;
        auto destroy = [this](Buffer &buffer)
        {
            if (buffer.handle) vmaDestroyBuffer(mDevice->allocator, buffer.handle, buffer.allocation);
            buffer = {};
        };
        destroy(mParticles);
        destroy(mCounters);
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f) { destroy(mInputs[f]); destroy(mReadback[f]); }
        mDevice.reset();
        mSlots = {};
        for (auto &signal : mSignals) signal = 0;
    }
}
