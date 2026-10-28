/**
 * @file ParticleProbePass.h
 * @author sumin.park
 * @brief Opt-in static quad/trail composition, no particle simulation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "Image2D.h"
#include "VulkanConstants.h"

#include <renderer/VulkanRenderer.h>

#include <optional>

namespace mir::vk
{
    class Device;
    class ParticleProbePass
    {
    public:
        ParticleProbePass() = default;
        ParticleProbePass(const ParticleProbePass &) = delete;
        ParticleProbePass &operator=(const ParticleProbePass &) = delete;
        ParticleProbePass(ParticleProbePass &&) = delete;
        ParticleProbePass &operator=(ParticleProbePass &&) = delete;
        void SetAnchor(std::optional<WorldPosition> anchor) { mAnchor = anchor; }
        // Optional probe allocates lazily, caller idles GPU before Resize/Shutdown.
        bool Resize(VkExtent2D extent);
        void Shutdown();
        VkImageView Record(Device &device, VkCommandBuffer cmd, uint32_t frame,
            VkImageView color, VkImageView depth, std::span<const RenderView> views,
            bool rayDistance, VkExtent2D extent, glm::vec2 depthJitter = {});
    private:
        bool Initialize(Device &device, VkExtent2D extent);
        Device *mDevice = nullptr;
        std::optional<WorldPosition> mAnchor;
        VkSampler mSampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
        VkDescriptorSet mSets[kFramesInFlight]{};
        VkPipelineLayout mLayout = VK_NULL_HANDLE;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        Image2D mOutput[kFramesInFlight]{};
    };
}
