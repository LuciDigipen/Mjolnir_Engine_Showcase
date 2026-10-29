/**
 * @file DebugDraw.h
 * @author Rahul Nair
 * @brief The editor's debug views drawn over a finished frame - each view's
 *        meshes as wireframe lines, for both the raster and ray-traced paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <span>

namespace mir
{
    struct RenderView;
}

namespace mir::vk
{
    class Device;
    class ResourceStore;

    class DebugDraw
    {
    public:
        // false only on a hard failure; a device without fillModeNonSolid leaves it unavailable instead
        bool Initialize(Device &device, VkFormat colorFormat);
        void Shutdown();

        // false: Wireframe falls back to the shaded render, since nothing could draw its lines
        bool Available() const { return mDepthPipeline != VK_NULL_HANDLE; }

        // whether any view draws lines this frame; false records nothing at all
        bool Wants(std::span<const RenderView> views) const;

        // Begins and ends its own rendering into color (COLOR_ATTACHMENT_OPTIMAL). With depth
        // (DEPTH_STENCIL_READ_ONLY_OPTIMAL, the scene's), Render + Wireframe lines hide behind
        // nearer geometry; without it, and always in Wireframe, every line shows.
        void Record(VkCommandBuffer cmd, std::span<const RenderView> views, VkImageView color, VkExtent2D extent,
            VkImageView depth, const ResourceStore &resources) const;

    private:
        bool CreatePipeline(VkShaderModule vertModule, VkShaderModule fragModule, VkFormat colorFormat,
            bool withDepth, VkPipeline &outPipeline);

        Device *mDevice = nullptr;
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        // drawn into a pass with the scene's depth attached; the depth test is dynamic state
        VkPipeline mDepthPipeline = VK_NULL_HANDLE;
        // drawn into a color-only pass
        VkPipeline mColorPipeline = VK_NULL_HANDLE;
    };
}
