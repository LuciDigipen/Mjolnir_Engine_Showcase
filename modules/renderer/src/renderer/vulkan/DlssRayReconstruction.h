/**
 * @file DlssRayReconstruction.h
 * @author Rahul Nair
 * @brief NVIDIA NGX DLSS Ray Reconstruction: denoise and anti-alias the
 *        ray-traced image in one pass, at native resolution (DLAA preset, no
 *        upscale ratio).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"

#include <glm/vec2.hpp>

#include <volk.h>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace mir::vk
{
    class Device;

    class DlssRayReconstruction
    {
    public:
        // NGX itself only - output images and the feature wait for Activate.
        bool Initialize(Device &device);
        void Shutdown();

        // Allocates the output images while Dlss is the active mode; the feature is built by
        // Record at the viewport rect's size. The caller idles the device before either.
        bool Activate(VkExtent2D extent);
        void Deactivate();
        bool IsActive() const { return mOutput[0].image != VK_NULL_HANDLE; }

        // Recreates the output images if active. The feature is sized by the rect, not the
        // swapchain, so Record rebuilds it only if the rect actually changed.
        bool Resize(VkExtent2D extent);

        struct Inputs
        {
            VkImage colorImage;
            VkImageView colorView;
            VkImage depthImage; // Reversed-Z projected depth, separate from DoF's ray distance.
            VkImageView depthView;
            VkImage velocityImage;
            VkImageView velocityView;
            VkImage albedoImage;
            VkImageView albedoView;
            VkImage normalImage;
            VkImageView normalView;
            VkImage specularAlbedoImage;
            VkImageView specularAlbedoView;
            glm::vec2 jitterPixels;
        };

        bool Record(VkCommandBuffer cmd, uint32_t frame, const Inputs &inputs, VkExtent2D extent, VkRect2D rect);
        void ResetHistory() { mResetHistory = true; }

        VkImageView OutputView(uint32_t frame) const { return mOutput[frame].view; }

    private:
        bool CreateOutputImages(VkExtent2D extent);
        void DestroyOutputImages();
        bool CreateFeature(VkExtent2D extent);
        void DestroyFeature();

        Device *mDevice = nullptr;

        NVSDK_NGX_Parameter *mParams = nullptr;
        NVSDK_NGX_Handle *mHandle = nullptr;
        bool mResetHistory = true;
        VkExtent2D mFeatureExtent{};
        VkRect2D mLastRect{};

        Image2D mOutput[kFramesInFlight]{};
    };
}
