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
#include <vector>

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
        uint64_t BindTexture(uint64_t key, VkImageView view);
        void RetireTexture(uint64_t key, uint64_t lastUse);
        void Collect(uint64_t completed);
        // The caller has waited for every frame that could use these bindings.
        void ReleaseTextures();

        struct TextureBinding
        {
            uint64_t mKey = 0;
            VkDescriptorSet mSet = VK_NULL_HANDLE;
            uint64_t mRetireAfter = UINT64_MAX;
        };

    private:
        std::vector<TextureBinding> mTextures;
        Device *mDevice = nullptr;
        bool mBackendInitialized = false;
        ImDrawData *mDrawData = nullptr;
    };
}
