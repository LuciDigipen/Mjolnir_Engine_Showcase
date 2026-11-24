/**
 * @file RenderTargets.cpp
 * @author Quy Dong Ta
 * @brief The images each render target owns, by handle, and the frames they wait on before they are freed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "RenderTargets.h"
#include "Device.h"
#include "VulkanConstants.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

namespace mir::vk
{
    namespace
    {
        // live targets and the ones a resize or release left waiting on the GPU
        constexpr uint32_t kTonemapSets = kMaxRenderTargets * 4;

        bool ValidExtent(VkExtent2D extent)
        {
            return extent.width >= 1 && extent.height >= 1 && extent.width <= kMaxRenderTargetSize &&
                extent.height <= kMaxRenderTargetSize;
        }

        uint64_t Bytes(VkExtent2D extent)
        {
            // HDR color, depth and an 8-bit output, 16 bytes a pixel
            return uint64_t(extent.width) * extent.height * 16;
        }
    }

    bool RenderTargets::Initialize(Device &device, VkFormat outputFormat, VkDescriptorSetLayout tonemapLayout,
        VkSampler sampler)
    {
        mDevice = &device;
        mOutputFormat = outputFormat;
        mTonemapLayout = tonemapLayout;
        mSampler = sampler;

        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = kTonemapSets * 2 };
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = kTonemapSets,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };
        if (vkCreateDescriptorPool(mDevice->Handle(), &poolInfo, nullptr, &mPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("RenderTargets::Initialize: vkCreateDescriptorPool failed");
            return false;
        }
        return true;
    }

    void RenderTargets::Shutdown()
    {
        if (mDevice == nullptr)
            return;
        mFreshRetired.clear();
        mRetired.ReleaseAll([this](GpuRenderTarget &target) { Destroy(target); });
        for (Slot &slot : mSlots)
            if (slot.live)
                Destroy(slot.target);
        mSlots.clear();
        if (mPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(mDevice->Handle(), mPool, nullptr);
        mPool = VK_NULL_HANDLE;
        mDevice = nullptr;
    }

    RenderTargetHandle RenderTargets::Create(VkExtent2D extent)
    {
        if (mDevice == nullptr)
            return kNullRenderTarget;
        if (!ValidExtent(extent))
        {
            MIR_LOG_ERROR("CreateRenderTarget: {}x{} is outside 1 to {} pixels a side", extent.width, extent.height,
                kMaxRenderTargetSize);
            return kNullRenderTarget;
        }
        if (LiveCount() >= kMaxRenderTargets)
        {
            MIR_LOG_ERROR("CreateRenderTarget: {} render targets are already live", kMaxRenderTargets);
            return kNullRenderTarget;
        }

        GpuRenderTarget target;
        if (!Make(extent, target))
        {
            Destroy(target);
            return kNullRenderTarget;
        }

        uint32_t index = 0;
        while (index < mSlots.size() && mSlots[index].live)
            ++index;
        if (index == mSlots.size())
            mSlots.emplace_back();
        Slot &slot = mSlots[index];
        slot.target = target;
        slot.live = true;
        return RenderTargetHandle{ .mIndex = index, .mGeneration = slot.generation };
    }

    bool RenderTargets::Resize(RenderTargetHandle handle, VkExtent2D extent, uint64_t lastUse)
    {
        GpuRenderTarget *const current = Find(handle);
        if (current == nullptr)
            return false;
        if (current->extent.width == extent.width && current->extent.height == extent.height)
            return true;
        if (!ValidExtent(extent))
        {
            MIR_LOG_ERROR("ResizeRenderTarget: {}x{} is outside 1 to {} pixels a side", extent.width, extent.height,
                kMaxRenderTargetSize);
            return false;
        }

        GpuRenderTarget resized;
        if (!Make(extent, resized))
        {
            Destroy(resized);
            return false;
        }
        Retire(lastUse, *current);
        *current = resized;
        return true;
    }

    bool RenderTargets::Release(RenderTargetHandle handle, uint64_t lastUse)
    {
        GpuRenderTarget *const target = Find(handle);
        if (target == nullptr)
            return false;
        Slot &slot = mSlots[handle.mIndex];
        Retire(lastUse, slot.target);
        slot.target = {};
        slot.live = false;
        ++slot.generation;
        return true;
    }

    GpuRenderTarget *RenderTargets::Find(RenderTargetHandle handle)
    {
        if (handle.mIndex >= mSlots.size())
            return nullptr;
        Slot &slot = mSlots[handle.mIndex];
        return slot.live && slot.generation == handle.mGeneration ? &slot.target : nullptr;
    }

    const GpuRenderTarget *RenderTargets::Find(RenderTargetHandle handle) const
    {
        return const_cast<RenderTargets *>(this)->Find(handle);
    }

    uint32_t RenderTargets::LiveCount() const
    {
        uint32_t count = 0;
        for (const Slot &slot : mSlots)
            count += slot.live ? 1 : 0;
        return count;
    }

    void RenderTargets::RecordFresh(VkCommandBuffer cmd, VkClearColorValue clear)
    {
        for (Slot &slot : mSlots)
        {
            if (!slot.live || !slot.target.fresh)
                continue;
            RecordClear(cmd, slot.target.output, slot.target.extent, clear);
            slot.target.fresh = false;
        }
        for (const GpuRenderTarget &target : mFreshRetired)
            RecordClear(cmd, target.output, target.extent, clear);
        mFreshRetired.clear();
    }

    void RenderTargets::RecordClear(VkCommandBuffer cmd, const Image2D &output, VkExtent2D extent,
        VkClearColorValue clear)
    {
        ColorAttachmentBarrier(cmd, output.image);
        BeginFullscreenPass(cmd, output.view, extent, VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
        vkCmdEndRendering(cmd);
        ShaderReadOnlyBarrier(cmd, output.image);
    }

    void RenderTargets::Retire(uint64_t lastUse, const GpuRenderTarget &target)
    {
        // the frame lastUse names records the next RecordFresh, so the images outlive it
        if (target.fresh)
            mFreshRetired.push_back(target);
        mRetired.Push(lastUse, Bytes(target.extent), target);
    }

    void RenderTargets::Collect(uint64_t completed)
    {
        mRetired.Collect(completed, [this](GpuRenderTarget &target) { Destroy(target); });
    }

    bool RenderTargets::Make(VkExtent2D extent, GpuRenderTarget &out)
    {
        out.extent = extent;
        if (!CreateImage2D(*mDevice, kHdrColorFormat, extent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Render target color", out.color) ||
            !CreateImage2D(*mDevice, kDepthFormat, extent, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                "Render target depth", out.depth, VK_IMAGE_ASPECT_DEPTH_BIT) ||
            !CreateImage2D(*mDevice, mOutputFormat, extent,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "Render target output", out.output))
            return false;

        if (!AllocateDescriptorSet(mDevice->Handle(), mPool, mTonemapLayout, out.tonemapSet))
        {
            MIR_LOG_ERROR("CreateRenderTarget: no tonemap descriptor set left");
            out.tonemapSet = VK_NULL_HANDLE;
            return false;
        }

        // the tonemap's bloom input is never read here, it is weighted 0
        const VkDescriptorImageInfo image{
            .sampler = mSampler, .imageView = out.color.view, .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkWriteDescriptorSet writes[]{
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out.tonemapSet, .dstBinding = 0,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = out.tonemapSet, .dstBinding = 1,
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image } };
        vkUpdateDescriptorSets(mDevice->Handle(), 2, writes, 0, nullptr);
        out.fresh = true;
        return true;
    }

    void RenderTargets::Destroy(GpuRenderTarget &target)
    {
        if (target.tonemapSet != VK_NULL_HANDLE)
            vkFreeDescriptorSets(mDevice->Handle(), mPool, 1, &target.tonemapSet);
        target.tonemapSet = VK_NULL_HANDLE;
        DestroyImage2D(*mDevice, target.output);
        DestroyImage2D(*mDevice, target.depth);
        DestroyImage2D(*mDevice, target.color);
    }
}
