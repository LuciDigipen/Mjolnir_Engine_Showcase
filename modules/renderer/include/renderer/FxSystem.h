/**
 * @file FxSystem.h
 * @author sumin.park
 * @brief Generic FX scheduling and Vulkan recording boundary, without particle ownership.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <volk.h>

#include <cstdint>
#include <span>

VK_DEFINE_HANDLE(VmaAllocator)

namespace mir
{
    struct RenderView;
    inline constexpr uint32_t kFxFramesInFlight = 2;

    inline constexpr uint32_t kFxMaxViews = 16;
    struct FxEnvironment
    {
        glm::vec4 sunDirection{}, sunColor{}, ambient{}, fogColor{}, fogRange{};
    };

    // Vulkan access is intentionally exposed only at the FX implementation boundary.
    struct FxDeviceContext
    {
        VkDevice device = VK_NULL_HANDLE;
        VmaAllocator allocator = nullptr;
        bool debugNames = false;
        float timestampPeriod = 0;
        uint32_t timestampBits = 0;
    };
    struct FxFrameContext
    {
        FxDeviceContext device;
        VkCommandBuffer commands;
        uint32_t frame;
        uint64_t signalValue;
    };
    struct FxRenderContext
    {
        FxFrameContext frame;
        VkImageView sceneColor;
        VkImageView depth;
        std::span<const RenderView> views;
        VkExtent2D extent;
        bool rayDistance;
        glm::vec2 jitter;
        VkFormat colorFormat;
        FxEnvironment environment;
    };

    class IFxSystem
    {
    public:
        virtual ~IFxSystem() = default;
        // Outside rendering, after the frame slot's existing timeline wait.
        virtual void RecordSimulation(const FxFrameContext &context) = 0;
        virtual bool HasRenderData() const = 0;
        // Outside rendering, view-local GPU work must not advance simulation.
        virtual void PrepareDraw(const FxRenderContext &) {}
        // Inside renderer-owned HDR dynamic rendering, depth is sampled read-only.
        virtual void RecordDraw(const FxRenderContext &context) = 0;
        // Outside rendering, after the HDR output is transitioned for sampling.
        virtual void FinishDraw(const FxRenderContext &) {}
        virtual void ClearScene() = 0;
        // Called only after all recorded uses complete, including unregistration.
        virtual void Shutdown() = 0;
    };
}
