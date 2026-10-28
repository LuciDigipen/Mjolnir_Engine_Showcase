/**
 * @file ParticlePool.h
 * @author sumin.park
 * @brief Bounded persistent GPU simulation, indirect sprites and delayed counters.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"

#include <renderer/ParticleSubmission.h>
#include <renderer/VulkanRenderer.h>

#include <vk_mem_alloc.h>

#include <array>

namespace mir::vk
{
    class Device;
    class ParticlePool
    {
    public:
        ParticlePool() = default;
        ParticlePool(const ParticlePool &) = delete;
        ParticlePool &operator=(const ParticlePool &) = delete;
        void Submit(std::span<const ParticleSubmission> inputs);
        ParticleGpuStats Stats() const { return mStats; }
        void Simulate(Device &device, VkCommandBuffer cmd, uint32_t frame, uint64_t signal);
        VkImageView Record(VkCommandBuffer cmd, uint32_t frame, VkImageView color,
            VkImageView depth, std::span<const RenderView> views, bool rayDistance,
            VkExtent2D extent, glm::vec2 jitter = {});
        // Caller has completed all GPU uses before resizing or destroying images.
        bool Resize(VkExtent2D extent);
        void Shutdown();
    private:
        struct Buffer
        {
            VkBuffer handle = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            void *mapped = nullptr;
        };
        struct Parameters
        {
            glm::uvec4 range{};
            glm::vec4 time{};
            glm::vec4 velocity{};
            glm::vec4 color{};
            glm::vec4 emission{};
        };
        static_assert(sizeof(Parameters) == 80);
        static_assert(offsetof(Parameters, time) == 16);
        static_assert(offsetof(Parameters, velocity) == 32);
        static_assert(offsetof(Parameters, color) == 48);
        static_assert(offsetof(Parameters, emission) == 64);
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
        bool Initialize(Device &device);
        bool MakeBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool host, bool read = false);
        void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess);
        Device *mDevice = nullptr;
        std::array<ParticleSubmission, kGpuParticleInstances> mPending{};
        std::array<Slot, kGpuParticleInstances> mSlots{};
        bool mSubmitted = false;
        bool mFirst = true;
        Buffer mParticles, mCounters;
        Buffer mInputs[kFramesInFlight], mReadback[kFramesInFlight];
        uint64_t mSignals[kFramesInFlight]{};
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{}, mComputeSets[kFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mCompute = VK_NULL_HANDLE, mDraw = VK_NULL_HANDLE;
        Image2D mOutput[kFramesInFlight]{};
        VkExtent2D mExtent{};
        ParticleGpuStats mStats;
    };
}
