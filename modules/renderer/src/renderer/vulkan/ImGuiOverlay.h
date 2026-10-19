/**
 * @file ImGuiOverlay.h
 * @author Sumin Park
 * @brief ImGui's Vulkan backend and the overlay it draws on top of each frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>

#include <cstdint>

struct ImDrawData;

namespace mir::vk
{
    class Device;
    class Swapchain;

    class ImGuiOverlay
    {
    public:
        bool Initialize(Device &device, const Swapchain &swapchain);
        void Shutdown();

        // nullptr draws nothing this frame.
        void SetDrawData(ImDrawData *drawData) { mDrawData = drawData; }

        void RecordInPass(VkCommandBuffer cmd) const;

    private:
        Device *mDevice = nullptr;
        bool mBackendInitialized = false;
        ImDrawData *mDrawData = nullptr;
    };
}
