/**
 * @file RasterSurfaces.cpp
 * @author Rahul Nair
 * @brief Owns optional raster surface attachments and their pass transitions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "RasterSurfaces.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <vk_mem_alloc.h>

#include <format>

namespace mir::vk
{
    namespace
    {
        constexpr uint64_t kMaxSurfaceBytes = 256ull * 1024 * 1024;
        constexpr uint32_t kFragmentBuffers = 3; // Lights, spot matrices and model data.
    }

    void RasterSurfaces::Destroy()
    {
        if (!mDevice) return;
        for (auto &frame : mImages)
            for (auto &image : frame) DestroyImage2D(*mDevice, image);
        for (auto &sets : mModelSets) sets.clear();
    }

    bool RasterSurfaces::FitsBudget(VkExtent2D extent, const VkPhysicalDeviceLimits &limits)
    {
        if (!extent.width || !extent.height ||
            extent.width > limits.maxFramebufferWidth || extent.height > limits.maxFramebufferHeight ||
            extent.width > limits.maxImageDimension2D || extent.height > limits.maxImageDimension2D ||
            limits.maxColorAttachments < kRasterSurfaceCount || limits.maxFragmentOutputAttachments < kRasterSurfaceCount ||
            limits.maxFragmentCombinedOutputResources < kRasterSurfaceCount + kFragmentBuffers) return false;
        constexpr uint64_t kMaxPixels = kMaxSurfaceBytes / (kRasterSurfaceCount * 8 * kFramesInFlight);
        return uint64_t(extent.width) * extent.height <= kMaxPixels;
    }

    bool RasterSurfaces::Create()
    {
        if (!FitsBudget(mExtent, mDevice->Properties().limits)) return false;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(mDevice->Physical(), kRasterSurfaceFormat, &properties);
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((properties.optimalTilingFeatures & required) != required) return false;
        for (const char *shader : { "default", "model", "unlit", "error" })
            for (const char *entry : { "vertexMain", "fragmentMain" })
            {
                std::error_code error;
                if (!std::filesystem::is_regular_file(ShaderPath(std::format("{}Surface.{}.spv", shader, entry)), error))
                    return false;
            }
        uint64_t allocatedBytes = 0;
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            for (uint32_t surface = 0; surface < kRasterSurfaceCount; ++surface)
            {
                auto &image = mImages[frame][surface];
                const VkImageCreateInfo info{ .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                    .imageType = VK_IMAGE_TYPE_2D, .format = kRasterSurfaceFormat,
                    .extent{ mExtent.width, mExtent.height, 1 }, .mipLevels = 1, .arrayLayers = 1,
                    .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
                const VmaAllocationCreateInfo allocationInfo{ .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
                if (vmaCreateImage(mDevice->Allocator(), &info, &allocationInfo, &image.image, &image.allocation, nullptr) != VK_SUCCESS)
                    return false;
                VmaAllocationInfo allocation{};
                vmaGetAllocationInfo(mDevice->Allocator(), image.allocation, &allocation);
                allocatedBytes += allocation.size;
                if (allocatedBytes > kMaxSurfaceBytes) return false;
                const VkImageViewCreateInfo view{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                    .image = image.image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = kRasterSurfaceFormat,
                    .subresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
                if (vkCreateImageView(mDevice->Handle(), &view, nullptr, &image.view) != VK_SUCCESS) return false;
                mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(image.image),
                    std::format("Raster surface {} frame {}", surface, frame).c_str());
            }
        return true;
    }

    void RasterSurfaces::Configure(bool requested, VkExtent2D extent)
    {
        const bool changed = mExtent.width != extent.width || mExtent.height != extent.height;
        if (Active() && (!requested || changed)) { mDevice->WaitIdle(); Destroy(); }
        if (changed || !requested) mFailed = false;
        mRequested = requested;
        mExtent = extent;
        if (requested && !Active() && !mFailed)
        {
            if (!Create())
            {
                Destroy(); mFailed = true;
                MIR_LOG_WARN("Raster SSGI inputs unavailable; preserving existing raster lighting");
            }
        }
    }

    void RasterSurfaces::Resize(VkExtent2D extent)
    {
        Destroy(); mFailed = false;
        Configure(mRequested, extent);
    }

    void RasterSurfaces::Shutdown() { Destroy(); mRequested = mFailed = false; }

    void RasterSurfaces::Begin(VkCommandBuffer cmd, uint32_t frame, VkImage depth, VkImageView depthView)
    {
        std::array<VkRenderingAttachmentInfo, kRasterSurfaceCount> colors{};
        for (uint32_t i = 0; i < kRasterSurfaceCount; ++i)
        {
            ImageBarrier(cmd, mImages[frame][i].image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            colors[i] = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mImages[frame][i].view, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue{ .color{} } };
        }
        ImageBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        const VkRenderingAttachmentInfo depthAttachment{ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depthView, .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        const VkRenderingInfo info{ .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{}, .extent = mExtent }, .layerCount = 1,
            .colorAttachmentCount = kRasterSurfaceCount, .pColorAttachments = colors.data(),
            .pDepthAttachment = &depthAttachment };
        vkCmdBeginRendering(cmd, &info);
    }

    void RasterSurfaces::End(VkCommandBuffer cmd, uint32_t frame, VkImage depth)
    {
        vkCmdEndRendering(cmd);
        for (const auto &image : mImages[frame])
            ImageBarrier(cmd, image.image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        ImageBarrier(cmd, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
}
