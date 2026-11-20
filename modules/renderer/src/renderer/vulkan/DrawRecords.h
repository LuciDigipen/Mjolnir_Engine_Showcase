/**
 * @file DrawRecords.h
 * @author Jackey Cen
 * @brief Set 5: one host-visible draw record buffer per frame in flight.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/DrawRecord.h>
#include <renderer/DrawRecordAllocator.h>

#include "VulkanConstants.h"

#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    class DrawRecords
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        VkDescriptorSetLayout SetLayout() const { return mSetLayout; }

        // The frame slot must have been waited on; starts a new sequence of indices.
        void ResetFrame(uint32_t frame);
        // Null once this frame has used its capacity; that frame logs one warning.
        std::optional<uint32_t> Acquire(uint32_t frame);
        void Write(uint32_t frame, uint32_t index, const DrawRecord &record);
        void Bind(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame) const;

    private:
        struct Frame
        {
            VkBuffer mBuffer = VK_NULL_HANDLE;
            VmaAllocation mAllocation = VK_NULL_HANDLE;
            std::byte *mMapped = nullptr;
            DrawRecordAllocator mAllocator;
            bool mWarned = false;
        };

        Device *mDevice = nullptr;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, kFramesInFlight> mSets{};
        std::array<Frame, kFramesInFlight> mFrames{};
    };
}
