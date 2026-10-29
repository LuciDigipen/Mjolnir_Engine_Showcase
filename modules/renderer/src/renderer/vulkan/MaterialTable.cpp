/**
 * @file MaterialTable.cpp
 * @author Jackey Cen
 * @brief Shared records, images and samplers behind set 4 and its descriptor set.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "MaterialTable.h"
#include "Device.h"
#include "ResourceStore.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

namespace mir::vk
{
    bool MaterialTable::Initialize(Device &device)
    {
        mDevice = &device;
        if (!CreateLayout() || !CreatePool() || !CreateRecordBuffer())
            return false;

        mRecords = std::make_unique<MaterialRecordTable>(MaterialRecordTableDesc{
            .capacityBytes = kMaterialRecordCapacityBytes,
            .alignment = 16,
            .framesInFlight = kFramesInFlight });

        mImages.resize(kMaxSharedImages);
        mSamplers.resize(kMaxSharedSamplers);
        return true;
    }

    bool MaterialTable::CreateLayout()
    {
        constexpr std::array<VkDescriptorSetLayoutBinding, 3> bindings{ {
            { .binding = kMaterialRecordBinding,
              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = kMaterialImageBinding,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
              .descriptorCount = kMaxSharedImages,
              .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = kMaterialSamplerBinding,
              .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
              .descriptorCount = kMaxSharedSamplers,
              .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT } } };

        constexpr std::array<VkDescriptorBindingFlags, 3> bindingFlags{ {
            0,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT,
            VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT } };

        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(bindingFlags.size()),
            .pBindingFlags = bindingFlags.data() };

        const VkDescriptorSetLayoutCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flagsInfo,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings = bindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &info, nullptr, &mLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("MaterialTable: vkCreateDescriptorSetLayout failed");
            return false;
        }
        return true;
    }

    bool MaterialTable::CreatePool()
    {
        const std::array<VkDescriptorPoolSize, 3> sizes{ {
            { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = kFramesInFlight },
            { .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = kFramesInFlight * kMaxSharedImages },
            { .type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = kFramesInFlight * kMaxSharedSamplers } } };

        const VkDescriptorPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = static_cast<uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data() };

        if (vkCreateDescriptorPool(mDevice->Handle(), &info, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("MaterialTable: vkCreateDescriptorPool failed");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkDescriptorSetAllocateInfo allocateInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mLayout };

            if (vkAllocateDescriptorSets(mDevice->Handle(), &allocateInfo, &mSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("MaterialTable: descriptor set allocation failed for frame {}", i);
                return false;
            }
        }
        return true;
    }

    bool MaterialTable::CreateRecordBuffer()
    {
        VmaAllocationInfo info{};
        if (!CreateBuffer(mDevice->Allocator(), kMaterialRecordCapacityBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
            mRecordBuffer, mRecordAllocation, &info))
        {
            MIR_LOG_CRITICAL("MaterialTable: record buffer allocation failed");
            return false;
        }

        mRecordMapped = static_cast<std::byte *>(info.pMappedData);
        mRecordBufferInfo = VkDescriptorBufferInfo{
            .buffer = mRecordBuffer,
            .offset = 0,
            .range = kMaterialRecordCapacityBytes };
        mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mRecordBuffer),
            "Material records");
        return true;
    }

    void MaterialTable::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        if (mRecordBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->Allocator(), mRecordBuffer, mRecordAllocation);
            mRecordBuffer = VK_NULL_HANDLE;
            mRecordAllocation = VK_NULL_HANDLE;
            mRecordMapped = nullptr;
        }

        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }

        if (mLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice->Handle(), mLayout, nullptr);
            mLayout = VK_NULL_HANDLE;
        }

        Clear();
    }

    void MaterialTable::Clear()
    {
        mDevice = nullptr;
        mRecords.reset();
        mImages.clear();
        mSamplers.clear();
        mChanges.clear();
        mTableVersion = 0;
        std::fill(std::begin(mSyncedVersion), std::end(mSyncedVersion), 0);
    }

    MaterialRecordId MaterialTable::CreateRecord(std::span<const std::byte> bytes, uint32_t alignment)
    {
        if (mRecords == nullptr)
            return kNullMaterialRecord;

        const MaterialRecordId id = mRecords->Create(bytes, alignment);
        if (!id.IsNull())
            UploadWrites();
        return id;
    }

    bool MaterialTable::UpdateRecord(MaterialRecordId id, std::span<const std::byte> bytes)
    {
        return mRecords != nullptr && mRecords->Update(id, bytes);
    }

    bool MaterialTable::ReleaseRecord(MaterialRecordId id)
    {
        return mRecords != nullptr && mRecords->Release(id);
    }

    uint32_t MaterialTable::RecordOffset(MaterialRecordId id) const
    {
        return mRecords != nullptr ? mRecords->Offset(id) : kInvalidMaterialRecordOffset;
    }

    uint32_t MaterialTable::RecordSize(MaterialRecordId id) const
    {
        return mRecords != nullptr ? mRecords->Size(id) : 0;
    }

    std::span<const MaterialTable::RecordChange> MaterialTable::PublishRecords(uint64_t completedFrame, uint64_t frame)
    {
        mChanges.clear();
        if (mRecords == nullptr)
            return mChanges;

        mRecords->Publish(completedFrame, frame);
        UploadWrites();

        for (const MaterialRecordTable::OffsetChange &change : mRecords->OffsetChanges())
            mChanges.push_back(RecordChange{ .mId = change.mId, .mOffset = change.mOffset });
        return mChanges;
    }

    void MaterialTable::UploadWrites()
    {
        const std::span<const MaterialRecordTable::Write> writes = mRecords->Writes();
        if (writes.empty() || mRecordMapped == nullptr)
            return;

        for (const MaterialRecordTable::Write &write : writes)
            std::memcpy(mRecordMapped + write.mOffset, write.mBytes.data(), write.mBytes.size());

        vmaFlushAllocation(mDevice->Allocator(), mRecordAllocation, 0, VK_WHOLE_SIZE);
        // Consumed exactly once: a later Create must not replay ranges that an
        // in-flight frame may already read.
        mRecords->ClearWrites();
    }

    void MaterialTable::Sync(uint32_t frame, ResourceStore &resources)
    {
        const uint64_t version = resources.TextureTableVersion() ^ (resources.SamplerTableVersion() << 32);
        if (version != mTableVersion)
        {
            mTableVersion = version;
            for (uint32_t i = 0; i < kMaxSharedImages; ++i)
            {
                mImages[i] = VkDescriptorImageInfo{
                    .sampler = VK_NULL_HANDLE,
                    .imageView = resources.ImageViewAt(i),
                    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            }
            for (uint32_t i = 0; i < kMaxSharedSamplers; ++i)
            {
                mSamplers[i] = VkDescriptorImageInfo{
                    .sampler = resources.SamplerAt(i),
                    .imageView = VK_NULL_HANDLE,
                    .imageLayout = VK_IMAGE_LAYOUT_UNDEFINED };
            }
        }

        if (mSyncedVersion[frame] == mTableVersion)
            return;
        mSyncedVersion[frame] = mTableVersion;

        const VkWriteDescriptorSet writes[]{
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kMaterialRecordBinding,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
             .pBufferInfo = &mRecordBufferInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kMaterialImageBinding,
             .descriptorCount = kMaxSharedImages,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
             .pImageInfo = mImages.data() },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = mSets[frame],
             .dstBinding = kMaterialSamplerBinding,
             .descriptorCount = kMaxSharedSamplers,
             .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
             .pImageInfo = mSamplers.data() } };
        vkUpdateDescriptorSets(mDevice->Handle(), static_cast<uint32_t>(std::size(writes)), writes, 0, nullptr);
    }
}
