/**
 * @file VulkanConstants.h
 * @author Sumin Park
 * @brief Constants shared by the Vulkan subsystems and the facade's sources.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>

namespace mir::vk
{
    constexpr uint32_t kFramesInFlight = 2;

    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

    constexpr VkFormat kHdrColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    constexpr VkFormat kDistanceFormat = VK_FORMAT_R32_SFLOAT;

    constexpr uint32_t kBloomMipLevels = 6;

    // Sizes the per-view slices of the camera, light and spot matrix buffers.
    constexpr uint32_t kMaxViewsPerFrame = 16;
}
