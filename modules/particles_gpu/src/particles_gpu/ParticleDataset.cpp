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
        uint32_t budget = mBudgets.birthUploads;
        uint64_t bytes = 0;
        mStats.snapshotBytes = 0;
        if (inputs.size() > kGpuParticleInstances)
        {
            mStats.rejectedSubmissions += inputs.size();
            mSubmitted = true;
            return;
        }
        for (const auto &input : inputs)
        {
            const uint64_t inputBytes = sizeof(ParticleSubmission) + input.scriptCode.size() + input.texturePixels.size() +
                input.births.size() * sizeof(ParticleGpuBirth);
            if (inputBytes > mBudgets.snapshotBytes - bytes) { ++mStats.rejectedSubmissions; mStats.droppedSubmitSpawns += input.spawns; continue; }
            if (!ValidParticleRenderProperties(input.renderer) || (!kLargeWorld && (glm::any(glm::greaterThan(glm::abs(input.anchor.Meters()), WorldVec3(65536))) ||
                    (input.explicitTile && glm::any(glm::greaterThan(glm::abs(input.tileOrigin.Meters()), WorldVec3(65536)))))) ||
                input.scriptKey == 0 || input.scriptCode.size() < 20 || input.scriptCode.size() % 4 != 0 ||
                input.index >= kGpuParticleInstances || input.generation == 0 ||
                input.births.size() > kGpuParticleSpawnsPerFrame ||
                (!input.births.empty() && input.births.size() != input.spawns) ||
                !std::isfinite(input.inheritVelocity) || input.inheritVelocity < 0 || input.inheritVelocity > 16 ||
                !std::isfinite(input.wind.x) || !std::isfinite(input.wind.y) || !std::isfinite(input.wind.z) ||
                !std::isfinite(input.attachmentVelocity.x) || !std::isfinite(input.attachmentVelocity.y) || !std::isfinite(input.attachmentVelocity.z) ||
                std::any_of(input.births.begin(), input.births.end(), [&](const auto &birth)
                    { return !std::isfinite(birth.fraction) || birth.fraction < 0 || birth.fraction > 1 ||
                        !std::isfinite(birth.age) || birth.age < 0 || birth.age > input.seconds + 1e-6f; }) ||
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
            bytes += inputBytes;
            mStats.snapshotBytes = bytes;
            mPending[input.index] = input;
            mPending[input.index].spawns = std::min(input.spawns, budget);
            if (!mPending[input.index].births.empty()) mPending[input.index].births.resize(mPending[input.index].spawns);
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
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
            !MakeBuffer(mCounters, sizeof(Counters) * kGpuParticleInstances,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
            return false;
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f)
            if (!MakeBuffer(mBirths[f], sizeof(ParticleGpuBirth) * kGpuParticleSpawnsPerFrame,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !MakeBuffer(mInputs[f], sizeof(Parameters) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
                !MakeBuffer(mReadback[f], (sizeof(Counters) + sizeof(GpuParticle)) * kGpuParticleInstances,
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
                return false;
        NameBuffer(device, mCounters.handle, "Particle GPU indirect counts");
        NameBuffer(device, mParticles.handle, "Particle persistent pool");
        mStats.gpuBytes = sizeof(GpuParticle) * kGpuParticleCapacity + sizeof(Counters) * kGpuParticleInstances +
            kFxFramesInFlight * (sizeof(ParticleGpuBirth) * kGpuParticleSpawnsPerFrame + sizeof(Parameters) * kGpuParticleInstances +
                (sizeof(Counters) + sizeof(GpuParticle)) * kGpuParticleInstances);
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
            const auto *samples = reinterpret_cast<const GpuParticle *>(c + kGpuParticleInstances);
            for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
                if (c[i].alive)
                {
                    const auto &sample = samples[i];
                    const auto position = WorldPosition::FromMeters(mReadbackOrigins[frame][i].Meters() + WorldVec3(sample.positionAge));
                    if (position) mStats.firstPosition = *position;
                    mStats.firstVelocity = glm::vec3(sample.velocityLifetime);
                    mStats.firstAge = sample.positionAge.w;
                    mStats.firstSize = sample.sizeRotation.x;
                    mStats.firstAlpha = sample.color.a;
                    mStats.firstFrame = static_cast<uint32_t>(sample.sizeRotation.z);
                    mStats.firstSequence = sample.identity.z;
                    break;
                }
            mStats.completedFrame = mSignals[frame];
        }
        if (!mSubmitted)
            for (auto &p : mPending) { p.seconds = 0; p.spawns = 0; p.births.clear(); p.emissionStart = p.anchor; }
        // Removed generations release their ranges. Reuse is ordered after all previous draws
        // on the same queue by the barrier below, no host wait or per-instance allocation.
        for (uint32_t i = 0; i < kGpuParticleInstances; ++i)
            if (mSlots[i].allocated && (mPending[i].generation != mSlots[i].input.generation ||
                mPending[i].capacity != mSlots[i].input.capacity))
                mSlots[i].allocated = false;
        auto *parameters = static_cast<Parameters *>(mInputs[frame].mapped);
        auto *births = static_cast<ParticleGpuBirth *>(mBirths[frame].mapped);
        uint32_t birthCursor = 0;
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
                const auto previousOrigin = slot.anchor;
                if (input.explicitTile) slot.anchor = input.tileOrigin;
                else
                {
                    const auto displacement = TryCameraRelativeFloat(input.anchor, slot.anchor);
                    if (!displacement || glm::any(glm::greaterThan(glm::abs(*displacement), glm::vec3(64))))
                        slot.anchor = input.anchor;
                }
                const auto delta = TryCameraRelativeFloat(previousOrigin, slot.anchor);
                const auto offset = TryCameraRelativeFloat(input.anchor, slot.anchor);
                const auto start = TryCameraRelativeFloat(input.swept ? input.emissionStart : input.anchor, slot.anchor);
                const bool bounded = delta && offset && start &&
                    glm::all(glm::lessThanEqual(glm::abs(*offset), glm::vec3(65536))) &&
                    glm::all(glm::lessThanEqual(glm::abs(*start), glm::vec3(65536)));
                if (delta && glm::any(glm::notEqual(*delta, glm::vec3(0)))) ++mStats.tileShifts;
                if (!bounded) mStats.droppedSubmitSpawns += input.spawns;
                p.range = {slot.start, input.capacity, input.generation, bounded ? input.spawns : 0};
                p.time = {input.seconds, input.lifetime, input.size, fresh ? 1.0f : 0.0f};
                p.velocity = glm::vec4(input.velocity, 0);
                p.color = input.color;
                p.emission = glm::vec4(offset.value_or(glm::vec3(0)), 0);
                p.emissionStart = glm::vec4(start.value_or(glm::vec3(0)), input.inheritVelocity);
                p.attachmentVelocity = glm::vec4(input.attachmentVelocity, 0);
                p.tileDelta = glm::vec4(delta.value_or(glm::vec3(0)), 0);
                p.wind = glm::vec4(input.wind, 0);
                const uint32_t count = bounded && !input.births.empty() ? input.spawns : 0;
                p.birthRange = {birthCursor, count, input.seed, 0};
                for (uint32_t birth = 0; birth < count; ++birth) births[birthCursor++] = input.births[birth];
                mReadbackOrigins[frame][i] = slot.anchor;
            }
            parameters[i] = p;
        }
        mStats.uploadBytes = sizeof(Parameters) * kGpuParticleInstances + birthCursor * sizeof(ParticleGpuBirth);
        vmaFlushAllocation(device.allocator, mInputs[frame].allocation, 0, VK_WHOLE_SIZE);
        vmaFlushAllocation(device.allocator, mBirths[frame].allocation, 0, VK_WHOLE_SIZE);
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
        for (uint32_t f = 0; f < kFxFramesInFlight; ++f) { destroy(mInputs[f]); destroy(mBirths[f]); destroy(mReadback[f]); }
        mDevice.reset();
        mSlots = {};
        for (auto &signal : mSignals) signal = 0;
    }
}
