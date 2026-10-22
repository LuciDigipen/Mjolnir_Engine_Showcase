/**
 * @file VulkanUtil.h
 * @author Sumin Park
 * @brief Vulkan helpers shared across the renderer's source files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <volk.h>
#include <vk_mem_alloc.h>

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace mir::vk
{
    class Device;

    // 1 / extent, as fragment shaders want it for texel-to-UV math.
    glm::vec2 InvExtent(VkExtent2D extent);

    bool CreateBuffer(VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaAllocationCreateFlags allocFlags,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation,
        VmaAllocationInfo *outInfo);

    // buffer for cpu writing
    bool CreateUploadBuffer(VmaAllocator allocator,
        std::span<const std::byte> bytes,
        VkBufferUsageFlags usage,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation);

    // An empty requested rect means the whole swapchain.
    VkRect2D ResolveViewRect(const VkRect2D &requested, VkExtent2D swapchainExtent);

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        uint32_t levelCount = 1, uint32_t layerCount = 1);

    // Transitions a 2D color image into a color-attachment render target.
    // firstUse: the image has been VK_IMAGE_LAYOUT_UNDEFINED since creation
    // (or its last external use); otherwise it is assumed to be coming from
    // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
    void ColorAttachmentBarrier(VkCommandBuffer cmd, VkImage image, bool firstUse = true);

    // Transitions a 2D color image from color-attachment back to shader-read-only.
    void ShaderReadOnlyBarrier(VkCommandBuffer cmd, VkImage image);

    // Begins dynamic rendering into one color attachment (already in COLOR_ATTACHMENT_OPTIMAL)
    // covering all of `extent`, and sets a matching viewport and scissor. The caller ends it.
    void BeginFullscreenPass(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent,
        VkAttachmentLoadOp loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, VkClearColorValue clear = {});

    // VK_NULL_HANDLE on failure, already logged.
    VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path &path);

    // Records into a throwaway command buffer, submits it and waits for the queue to go idle.
    bool SubmitAndWait(const Device &device, const std::function<void(VkCommandBuffer)> &record);

    bool AllocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
        VkDescriptorSet &outSet);

    // Fixed-function state shared by every single-triangle fullscreen pass
    struct FullscreenPipelineDesc
    {
        VkShaderModule vertModule;
        VkShaderModule fragModule;
        const char *fragEntry;
        VkPipelineLayout layout;
        const VkPipelineColorBlendAttachmentState *blendAttachment;
        VkFormat colorFormat;
    };

    bool CreateFullscreenPipeline(const Device &device, const FullscreenPipelineDesc &desc,
        const char *debugName, VkPipeline &outPipeline);
}
