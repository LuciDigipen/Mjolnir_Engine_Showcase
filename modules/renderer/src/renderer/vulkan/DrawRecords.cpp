/**
 * @file DrawRecords.cpp
 * @author Jackey Cen
 * @brief Set 5: one host-visible draw record buffer per frame in flight.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "DrawRecords.h"
#include "Device.h"
#include "DrawRecordLayout.h"

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <cstring>

namespace mir::vk
{
    bool DrawRecords::Initialize(Device &device)
    {
        mDevice = &device;

        const VkDescriptorSetLayoutBinding binding{
            .binding = kDrawRecordBinding,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT };
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding };
        if (vkCreateDescriptorSetLayout(mDevice->Handle(), &layoutInfo, nullptr, &mSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DrawRecords: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = kFramesInFlight };
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };
        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DrawRecords: vkCreateDescriptorPool failed");
            return false;
        }

        std::array<VkDescriptorSetLayout, kFramesInFlight> layouts{};
        layouts.fill(mSetLayout);
        const VkDescriptorSetAllocateInfo setInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mPool,
            .descriptorSetCount = kFramesInFlight,
            .pSetLayouts = layouts.data() };
        if (vkAllocateDescriptorSets(mDevice->Handle(), &setInfo, mSets.data()) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DrawRecords: vkAllocateDescriptorSets failed");
            return false;
        }

        const VkDeviceSize bytes = VkDeviceSize{ kMaxDrawRecords } * sizeof(DrawRecord);
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            Frame &records = mFrames[frame];
            const VkBufferCreateInfo bufferInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = bytes,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
            const VmaAllocationCreateInfo allocationInfo{
                .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                .usage = VMA_MEMORY_USAGE_AUTO,
                .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT };
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(mDevice->Allocator(), &bufferInfo, &allocationInfo,
                &records.mBuffer, &records.mAllocation, &info) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("DrawRecords: buffer allocation failed for frame {}", frame);
                return false;
            }
            records.mMapped = static_cast<std::byte *>(info.pMappedData);

            const VkDescriptorBufferInfo bufferDescriptor{
                .buffer = records.mBuffer,
                .offset = 0,
                .range = VK_WHOLE_SIZE };
            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = mSets[frame],
                .dstBinding = kDrawRecordBinding,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &bufferDescriptor };
            vkUpdateDescriptorSets(mDevice->Handle(), 1, &write, 0, nullptr);

            mDevice->NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(records.mBuffer),
                "Draw records");
        }
        return true;
    }

    void DrawRecords::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        for (Frame &records : mFrames)
        {
            if (records.mBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), records.mBuffer, records.mAllocation);
            records = {};
        }
        mSets = {};

        vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
        mPool = VK_NULL_HANDLE;
        vkDestroyDescriptorSetLayout(mDevice->Handle(), mSetLayout, nullptr);
        mSetLayout = VK_NULL_HANDLE;
        mDevice = nullptr;
    }

    void DrawRecords::ResetFrame(uint32_t frame)
    {
        Frame &records = mFrames[frame];
        records.mAllocator.Reset();
        records.mWarned = false;
    }

    std::optional<uint32_t> DrawRecords::Acquire(uint32_t frame)
    {
        Frame &records = mFrames[frame];
        const std::optional<uint32_t> index = records.mAllocator.Acquire();
        if (!index && !records.mWarned)
        {
            records.mWarned = true;
            MIR_LOG_WARN("DrawRecords: {} draws this frame is the capacity; the rest are skipped",
                kMaxDrawRecords);
        }
        return index;
    }

    void DrawRecords::Write(uint32_t frame, uint32_t index, const DrawRecord &record)
    {
        Frame &records = mFrames[frame];
        std::memcpy(records.mMapped + VkDeviceSize{ index } * sizeof(DrawRecord), &record, sizeof(record));
    }

    void DrawRecords::Bind(VkCommandBuffer cmd, VkPipelineLayout layout, uint32_t frame) const
    {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
            kDrawRecordSetIndex, 1, &mSets[frame], 0, nullptr);
    }
}
