/**
 * @file DebugDraw.h
 * @author Rahul Nair
 * @brief The editor's debug views drawn over a finished frame - each view's
 *        meshes as wireframe lines and its debug line segments such as
 *        colliders, for both the raster and ray-traced paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "VulkanConstants.h"

#include <volk.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
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
        // false only on a hard failure; a device without fillModeNonSolid leaves the wireframe unavailable instead
        bool Initialize(Device &device, VkFormat colorFormat);
        void Shutdown();

        // false: Wireframe falls back to the shaded render, since nothing could draw its lines
        bool WireframeAvailable() const { return mWireframeDepth != VK_NULL_HANDLE; }

        // whether any view draws anything this frame; false records nothing at all
        bool Wants(std::span<const RenderView> views) const;

        // Begins and ends its own rendering into color (COLOR_ATTACHMENT_OPTIMAL). With depth
        // (DEPTH_STENCIL_READ_ONLY_OPTIMAL, the scene's), Render + Wireframe lines hide behind
        // nearer geometry; without it, and always in Wireframe, every line shows. Debug line
        // segments always show, so a collider inside its mesh stays visible.
        void Record(VkCommandBuffer cmd, uint32_t frame, std::span<const RenderView> views, VkImageView color,
            VkExtent2D extent, VkImageView depth, const ResourceStore &resources);

    private:
        enum class Kind { Wireframe, Lines };
        bool CreatePipeline(Kind kind, VkShaderModule vertModule, VkShaderModule fragModule, VkFormat colorFormat,
            bool withDepth, VkPipeline &outPipeline);
        bool CreateWireframe(VkFormat colorFormat);
        bool CreateLines(VkFormat colorFormat);
        // Copies every view's segments into this frame's buffer, growing it first; false leaves them undrawn.
        bool UploadLines(uint32_t frame, std::span<const RenderView> views);

        Device *mDevice = nullptr;

        VkPipelineLayout mWireframeLayout = VK_NULL_HANDLE;
        // drawn into a pass with the scene's depth attached; the depth test is dynamic state
        VkPipeline mWireframeDepth = VK_NULL_HANDLE;
        // drawn into a color-only pass
        VkPipeline mWireframeColor = VK_NULL_HANDLE;

        VkPipelineLayout mLinesLayout = VK_NULL_HANDLE;
        VkPipeline mLinesDepth = VK_NULL_HANDLE;
        VkPipeline mLinesColor = VK_NULL_HANDLE;

        // one per frame in flight: BeginFrame waited for the slot, so its buffer is free to rewrite
        struct LineBuffer
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            void *mapped = nullptr;
            VkDeviceSize capacity = 0;
        };
        std::array<LineBuffer, kFramesInFlight> mLineBuffers{};
        bool mWarnedTooManyLines = false;
    };
}
