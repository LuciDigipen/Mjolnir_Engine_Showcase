/**
 * @file MaterialTable.h
 * @author Jackey Cen
 * @brief Shared records, images and samplers behind set 4 and its descriptor set.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/MaterialRecordTable.h>

#include "MaterialTableLayout.h"
#include "VulkanConstants.h"

#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;
    class ResourceStore;

    // Only constructed on a device with the descriptor-indexing features.
    class MaterialTable
    {
    public:
        bool Initialize(Device &device);
        void Shutdown();

        VkDescriptorSetLayout SetLayout() const { return mLayout; }
        VkDescriptorSet Set(uint32_t frame) const { return mSets[frame]; }

        MaterialRecordId CreateRecord(std::span<const std::byte> bytes, uint32_t alignment = 0);
        bool UpdateRecord(MaterialRecordId id, std::span<const std::byte> bytes);
        bool ReleaseRecord(MaterialRecordId id);
        uint32_t RecordOffset(MaterialRecordId id) const;
        uint32_t RecordSize(MaterialRecordId id) const;

        struct RecordChange
        {
            MaterialRecordId mId;
            uint32_t mOffset = kInvalidMaterialRecordOffset;
        };

        // After BeginFrame's wait: publishes staged bytes and returns records
        // whose set-0 offset changed.
        std::span<const RecordChange> PublishRecords(uint64_t completedFrame, uint64_t frame);

        // Rebuilds the shared row data when resources changed, then writes that
        // frame's set. Call after BeginFrame's wait, before recording.
        void Sync(uint32_t frame, ResourceStore &resources);

        std::span<const VkDescriptorImageInfo> ImageTable() const { return mImages; }
        std::span<const VkDescriptorImageInfo> SamplerTable() const { return mSamplers; }
        const VkDescriptorBufferInfo &RecordBufferInfo() const { return mRecordBufferInfo; }
        uint64_t TableVersion() const { return mTableVersion; }

    private:
        bool CreateLayout();
        bool CreatePool();
        bool CreateRecordBuffer();
        void UploadWrites();
        void Clear();

        Device *mDevice = nullptr;

        VkDescriptorSetLayout mLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};

        VkBuffer mRecordBuffer = VK_NULL_HANDLE;
        VmaAllocation mRecordAllocation = VK_NULL_HANDLE;
        std::byte *mRecordMapped = nullptr;
        VkDescriptorBufferInfo mRecordBufferInfo{};

        std::unique_ptr<MaterialRecordTable> mRecords;
        std::vector<VkDescriptorImageInfo> mImages;
        std::vector<VkDescriptorImageInfo> mSamplers;
        std::vector<RecordChange> mChanges;
        uint64_t mTableVersion = 0;
        uint64_t mSyncedVersion[kFramesInFlight]{};
    };
}
