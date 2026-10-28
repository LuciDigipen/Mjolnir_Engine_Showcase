/**
 * @file ParticleDataset.h
 * @author sumin.park
 * @brief Persistent emitter ranges, GPU storage and timeline-completed diagnostics.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <particles_gpu/ParticleSubmission.h>
#include <renderer/FxSystem.h>

#include <vk_mem_alloc.h>

#include <array>
#include <optional>

namespace mir::gpu
{
    class ParticleDispatch;
    class ParticleSpriteRenderer;
    class ParticleDataset
    {
    public:
        ParticleDataset() = default;
        ParticleDataset(const ParticleDataset &) = delete;
        ParticleDataset &operator=(const ParticleDataset &) = delete;
        void Submit(std::span<const ParticleSubmission> inputs);
        bool Prepare(const FxFrameContext &context);
        void Shutdown();
        bool Initialized() const { return mDevice.has_value(); }
        ParticleGpuStats Stats() const { return mStats; }
    private:
        friend class ParticleDispatch;
        friend class ParticleSpriteRenderer;
        struct Buffer
        {
            VkBuffer handle = VK_NULL_HANDLE;
            VmaAllocation allocation = nullptr;
            void *mapped = nullptr;
        };
        struct Parameters
        {
            glm::uvec4 range{};
            glm::vec4 time{}, velocity{}, color{}, emission{};
            glm::vec4 emissionStart{}, attachmentVelocity{}, tileDelta{}, wind{};
            glm::uvec4 birthRange{};
        };
        static_assert(sizeof(Parameters) == 160);
        static_assert(offsetof(Parameters, time) == 16);
        static_assert(offsetof(Parameters, velocity) == 32);
        static_assert(offsetof(Parameters, color) == 48);
        static_assert(offsetof(Parameters, emission) == 64);
        static_assert(offsetof(Parameters, emissionStart) == 80);
        static_assert(offsetof(Parameters, attachmentVelocity) == 96);
        static_assert(offsetof(Parameters, tileDelta) == 112);
        static_assert(offsetof(Parameters, wind) == 128);
        static_assert(offsetof(Parameters, birthRange) == 144);
        struct Counters
        {
            uint32_t vertices, alive, firstVertex, firstInstance;
            uint32_t spawned, dropped, generation, reserved;
        };
        static_assert(sizeof(Counters) == 32);
        static_assert(offsetof(Counters, alive) == 4);
        static_assert(offsetof(Counters, spawned) == sizeof(VkDrawIndirectCommand));
        struct Slot
        {
            ParticleSubmission input;
            WorldPosition anchor;
            uint32_t start = 0;
            bool allocated = false;
        };
        bool Initialize(const FxDeviceContext &device);
        bool MakeBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool read = false);
        std::optional<FxDeviceContext> mDevice;
        std::array<ParticleSubmission, kGpuParticleInstances> mPending{};
        std::array<Slot, kGpuParticleInstances> mSlots{};
        bool mSubmitted = false;
        Buffer mParticles, mCounters;
        Buffer mInputs[kFxFramesInFlight], mBirths[kFxFramesInFlight], mReadback[kFxFramesInFlight];
        std::array<WorldPosition, kGpuParticleInstances> mReadbackOrigins[kFxFramesInFlight];
        uint64_t mSignals[kFxFramesInFlight]{};
        ParticleGpuStats mStats;
    };
}
