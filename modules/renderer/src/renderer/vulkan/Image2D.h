/**
 * @file Image2D.h
 * @author Rahul Nair
 * @brief A single-mip, single-layer 2D color image and its view, created and
 *        destroyed together - the shape every per-frame render target here has.
 *        Implemented in VulkanUtil.cpp.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    struct Image2D
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // Logs on failure; a partially created `out` is still safe to pass to DestroyImage2D.
    bool CreateImage2D(const Device &device, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
        const char *debugName, Image2D &out, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    // No-op on an empty image; leaves it empty.
    void DestroyImage2D(const Device &device, Image2D &image);
}
