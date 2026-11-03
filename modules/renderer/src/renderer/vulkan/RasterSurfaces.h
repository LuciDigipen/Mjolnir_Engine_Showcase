/**
 * @file RasterSurfaces.h
 * @author Rahul Nair
 * @brief Optional raster inputs for screen-space indirect lighting.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"

#include <array>

namespace mir::vk
{
    enum class RasterSurface : uint32_t { NormalKind, Albedo, DiffuseRadiance, DirectLighting, Count };
    constexpr uint32_t kRasterSurfaceCount = uint32_t(RasterSurface::Count);
    constexpr VkFormat kRasterSurfaceFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    class RasterSurfaces
    {
    public:
        void Initialize(Device &device) { mDevice = &device; }
        // Before BeginFrame; waits only when replacing or freeing resources.
        void Configure(bool requested, VkExtent2D extent);
        void Resize(VkExtent2D extent); // Caller has idled the device.
        void Shutdown();
        static bool FitsBudget(VkExtent2D extent, const VkPhysicalDeviceLimits &limits);
        bool Active() const { return mImages[0][0].image != VK_NULL_HANDLE; }
        const Image2D &Image(RasterSurface surface, uint32_t frame) const { return mImages[frame][uint32_t(surface)]; }
        // Cleared to the background value; the depth image stays the pass's own attachment.
        std::array<VkRenderingAttachmentInfo, kRasterSurfaceCount> Attachments(uint32_t frame) const;
        void BeginOpaque(VkCommandBuffer cmd, uint32_t frame);
        void EndOpaque(VkCommandBuffer cmd, uint32_t frame);

    private:
        bool Create();
        void Destroy();
        Device *mDevice = nullptr;
        VkExtent2D mExtent{};
        bool mRequested = false, mFailed = false;
        std::array<std::array<Image2D, kRasterSurfaceCount>, kFramesInFlight> mImages{};
    };
}
