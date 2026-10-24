/**
 * @file VulkanUtil.cpp
 * @author Sumin Park
 * @brief Vulkan helpers shared across the renderer's source files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "VulkanUtil.h"
#include "Image2D.h"
#include "Device.h"

#include <core/fs/FileIO.h>
#include <core/log/Log.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace mir::vk
{
    namespace
    {
        std::optional<std::vector<uint32_t>> ReadSpirv(const std::filesystem::path &path)
        {
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
            if (!bytes)
                // ReadBytes takes care of logging onfailure
                return std::nullopt;

            if (bytes->empty() || bytes->size() % sizeof(uint32_t) != 0)
            {
                MIR_LOG_CRITICAL("ReadSpirv: Bad SPIR-V size ({} bytes): {}", bytes->size(), path.string());
                return std::nullopt;
            }

            // Copied, not reinterpreted: vkCreateShaderModule needs pCode 4-byte
            // aligned and a vector<byte> is only aligned to 1.
            std::vector<uint32_t> words(bytes->size() / sizeof(uint32_t));
            std::memcpy(words.data(), bytes->data(), bytes->size());
            return words;
        }
    }

    bool CreateBuffer(VmaAllocator allocator,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        VmaAllocationCreateFlags allocFlags,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation,
        VmaAllocationInfo *outInfo)
    {
        const VkBufferCreateInfo bufferInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };

        const VmaAllocationCreateInfo allocInfo{
            .flags = allocFlags,
            .usage = VMA_MEMORY_USAGE_AUTO };

        return vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
            &outBuffer, &outAllocation, outInfo) == VK_SUCCESS;
    }

    bool CreateUploadBuffer(VmaAllocator allocator,
        std::span<const std::byte> bytes,
        VkBufferUsageFlags usage,
        VkBuffer &outBuffer,
        VmaAllocation &outAllocation)
    {
        VmaAllocationInfo info{};
        if (!CreateBuffer(allocator, bytes.size(), usage,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            outBuffer, outAllocation, &info))
            return false;

        std::memcpy(info.pMappedData, bytes.data(), bytes.size());
        vmaFlushAllocation(allocator, outAllocation, 0, VK_WHOLE_SIZE);
        return true;
    }

    VkRect2D ResolveViewRect(const VkRect2D &requested, VkExtent2D swapchainExtent)
    {
        VkRect2D rect = (requested.extent.width > 0 && requested.extent.height > 0)
            ? requested
            : VkRect2D{ .offset{ 0, 0 }, .extent = swapchainExtent };

        rect.offset.x = std::clamp(rect.offset.x, 0, static_cast<int32_t>(swapchainExtent.width));
        rect.offset.y = std::clamp(rect.offset.y, 0, static_cast<int32_t>(swapchainExtent.height));
        rect.extent.width = std::min(rect.extent.width,
            swapchainExtent.width - static_cast<uint32_t>(rect.offset.x));
        rect.extent.height = std::min(rect.extent.height,
            swapchainExtent.height - static_cast<uint32_t>(rect.offset.y));
        return rect;
    }

    glm::vec2 InvExtent(VkExtent2D extent)
    {
        return { 1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height) };
    }

    bool CreateImage2D(const Device &device, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage,
        const char *debugName, Image2D &out)
    {
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent{ extent.width, extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

        if (vmaCreateImage(device.Allocator(), &imageInfo, &allocInfo, &out.image, &out.allocation, nullptr) !=
            VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateImage2D: vmaCreateImage failed for {}", debugName);
            return false;
        }

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = out.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1 } };

        if (vkCreateImageView(device.Handle(), &viewInfo, nullptr, &out.view) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateImage2D: vkCreateImageView failed for {}", debugName);
            return false;
        }

        device.NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(out.image), debugName);
        return true;
    }

    void DestroyImage2D(const Device &device, Image2D &image)
    {
        if (image.view != VK_NULL_HANDLE)
            vkDestroyImageView(device.Handle(), image.view, nullptr);
        if (image.image != VK_NULL_HANDLE)
            vmaDestroyImage(device.Allocator(), image.image, image.allocation);
        image = Image2D{};
    }

    void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask,
        VkImageLayout oldLayout, VkImageLayout newLayout,
        VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
        uint32_t levelCount, uint32_t layerCount)
    {
        const VkImageMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = srcStage,
            .srcAccessMask = srcAccess,
            .dstStageMask = dstStage,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspectMask,
                .levelCount = levelCount,
                .layerCount = layerCount } };

        const VkDependencyInfo dep{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &barrier };

        vkCmdPipelineBarrier2(cmd, &dep);
    }

    void ColorAttachmentBarrier(VkCommandBuffer cmd, VkImage image, bool firstUse)
    {
        ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
            firstUse ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            firstUse ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            firstUse ? 0 : VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    }

    void ShaderReadOnlyBarrier(VkCommandBuffer cmd, VkImage image)
    {
        ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }

    void BeginFullscreenPass(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent,
        VkAttachmentLoadOp loadOp, VkClearColorValue clear)
    {
        const VkRenderingAttachmentInfo attachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = loadOp,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .color = clear } };

        const VkRect2D area{ .offset{ 0, 0 }, .extent = extent };
        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea = area,
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &attachment };
        vkCmdBeginRendering(cmd, &renderingInfo);

        const VkViewport viewport{
            .x = 0.0f, .y = 0.0f,
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .minDepth = 0.0f, .maxDepth = 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &area);
    }

    VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path &path)
    {
        const std::optional<std::vector<uint32_t>> words = ReadSpirv(path);
        if (!words.has_value())
            return VK_NULL_HANDLE;

        const VkShaderModuleCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words->size() * sizeof(uint32_t),
            .pCode = words->data() };

        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateShaderModule: vkCreateShaderModule failed: {}", path.string());
            return VK_NULL_HANDLE;
        }
        return module;
    }

    bool SubmitAndWait(const Device &device, const std::function<void(VkCommandBuffer)> &record)
    {
        VkCommandPool pool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = device.QueueFamily() };

        if (vkCreateCommandPool(device.Handle(), &poolInfo, nullptr, &pool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("SubmitAndWait: command pool creation failed");
            return false;
        }

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        vkAllocateCommandBuffers(device.Handle(), &allocInfo, &cmd);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &beginInfo);
        record(cmd);
        vkEndCommandBuffer(cmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cmd };

        const bool submitted = vkQueueSubmit(device.Queue(), 1, &submitInfo, VK_NULL_HANDLE) == VK_SUCCESS;
        if (submitted)
            device.WaitQueueIdle();
        else
            MIR_LOG_CRITICAL("SubmitAndWait: vkQueueSubmit failed");

        // Destroying the pool frees its command buffer too.
        vkDestroyCommandPool(device.Handle(), pool, nullptr);
        return submitted;
    }

    bool AllocateDescriptorSet(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout,
        VkDescriptorSet &outSet)
    {
        const VkDescriptorSetAllocateInfo allocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout };
        return vkAllocateDescriptorSets(device, &allocInfo, &outSet) == VK_SUCCESS;
    }

    bool CreateFullscreenPipeline(const Device &device, const FullscreenPipelineDesc &desc,
        const char *debugName, VkPipeline &outPipeline)
    {
        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = desc.vertModule,
             .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = desc.fragModule,
             .pName = desc.fragEntry } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1 };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };

        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = desc.blendAttachment };

        const VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &desc.colorFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = desc.layout };

        if (vkCreateGraphicsPipelines(device.Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline) !=
            VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateFullscreenPipeline: vkCreateGraphicsPipelines failed for {}", debugName);
            return false;
        }
        device.NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(outPipeline), debugName);
        return true;
    }
}
