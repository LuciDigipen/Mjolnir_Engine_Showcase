/**
 * @file ParticleVulkan.h
 * @author sumin.park
 * @brief Particle backend Vulkan allocation, shaders and explicit synchronization helpers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/FxSystem.h>

#include <vk_mem_alloc.h>

namespace mir::gpu
{
    bool CreateParticleBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
        VmaAllocationCreateFlags flags, VkBuffer &buffer, VmaAllocation &allocation, VmaAllocationInfo &info);
    VkShaderModule LoadShader(VkDevice device, const char *name);
    void NameBuffer(const FxDeviceContext &device, VkBuffer buffer, const char *name);
    void MemoryBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess);
    VkRect2D ResolveRect(VkRect2D requested, VkExtent2D extent);
    bool CreateSpritePipeline(VkDevice device, VkPipelineLayout layout, VkFormat format, VkPipeline &pipeline);
}
