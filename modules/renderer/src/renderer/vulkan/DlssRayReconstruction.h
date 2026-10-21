/**
 * @file DlssRayReconstruction.h
 * @author Rahul Nair
 * @brief NVIDIA NGX DLSS Ray Reconstruction: denoise, upscale (native-res here -
 *        see renderer.md's Scope note) and anti-alias the ray-traced image in
 *        one pass.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <glm/vec2.hpp>

#include <volk.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

VK_DEFINE_HANDLE(VmaAllocation)

namespace mir::vk
{
    class Device;

    class DlssRayReconstruction
    {
    public:
        bool Initialize(Device &device, VkExtent2D extent);
        void Shutdown();

        bool Resize(VkExtent2D extent);

        struct Inputs
        {
            VkImage colorImage;
            VkImageView colorView;
            VkImage depthImage; // RayTracer's gDistance (linear distance) - see .cpp
            VkImageView depthView;
            VkImage velocityImage;
            VkImageView velocityView;
            VkImage albedoImage;
            VkImageView albedoView;
            VkImage normalImage;
            VkImageView normalView;
            glm::vec2 jitterPixels;
        };

        void Record(VkCommandBuffer cmd, uint32_t frame, const Inputs &inputs, VkExtent2D extent);

        VkImageView OutputView(uint32_t frame) const { return mOutputViews[frame]; }

    private:
        bool CreateOutputImages(VkExtent2D extent);
        void DestroyOutputImages();
        bool CreateFeature(VkExtent2D extent);
        void DestroyFeature();

        Device *mDevice = nullptr;

        NVSDK_NGX_Parameter *mParams = nullptr;
        NVSDK_NGX_Handle *mHandle = nullptr;

        VkImage mOutputImages[kFramesInFlight]{};
        VmaAllocation mOutputAllocations[kFramesInFlight]{};
        VkImageView mOutputViews[kFramesInFlight]{};
    };
}
