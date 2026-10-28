/**
 * @file ParticleVulkan.cpp
 * @author sumin.park
 * @brief Vulkan helpers used only by the GPU particle implementation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "ParticleVulkan.h"

#include <core/fs/FileIO.h>
#include <core/fs/Paths.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace mir::gpu
{
    bool CreateParticleBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage,
        VmaAllocationCreateFlags flags, VkBuffer &buffer, VmaAllocation &allocation, VmaAllocationInfo &info)
    {
        const VkBufferCreateInfo description{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        const VmaAllocationCreateInfo memory{.flags = flags, .usage = VMA_MEMORY_USAGE_AUTO};
        return vmaCreateBuffer(allocator, &description, &memory, &buffer, &allocation, &info) == VK_SUCCESS;
    }
    VkShaderModule LoadShader(VkDevice device, const char *name)
    {
        const auto bytes = ReadBytes(ShaderPath(name));
        if (!bytes || bytes->empty() || bytes->size() % 4 != 0) return VK_NULL_HANDLE;
        std::vector<uint32_t> words(bytes->size() / 4);
        std::memcpy(words.data(), bytes->data(), bytes->size());
        const VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = bytes->size(), .pCode = words.data()};
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(device, &info, nullptr, &module);
        return module;
    }
    void NameBuffer(const FxDeviceContext &device, VkBuffer buffer, const char *name)
    {
        if (!device.debugNames || !vkSetDebugUtilsObjectNameEXT) return;
        const VkDebugUtilsObjectNameInfoEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = VK_OBJECT_TYPE_BUFFER, .objectHandle = reinterpret_cast<uint64_t>(buffer), .pObjectName = name};
        vkSetDebugUtilsObjectNameEXT(device.device, &info);
    }
    void MemoryBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess,
        VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess)
    {
        const VkMemoryBarrier2 memory{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = src, .srcAccessMask = srcAccess, .dstStageMask = dst, .dstAccessMask = dstAccess};
        const VkDependencyInfo dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &memory};
        vkCmdPipelineBarrier2(commands, &dependency);
    }
    VkRect2D ResolveRect(VkRect2D requested, VkExtent2D extent)
    {
        VkRect2D rect = requested.extent.width && requested.extent.height ? requested : VkRect2D{{0,0}, extent};
        rect.offset.x = std::clamp(rect.offset.x, 0, int32_t(extent.width));
        rect.offset.y = std::clamp(rect.offset.y, 0, int32_t(extent.height));
        rect.extent.width = std::min(rect.extent.width, extent.width - uint32_t(rect.offset.x));
        rect.extent.height = std::min(rect.extent.height, extent.height - uint32_t(rect.offset.y));
        return rect;
    }
    bool CreateSpritePipeline(VkDevice device, VkPipelineLayout layout, VkFormat format, VkPipeline &pipeline, bool ribbon)
    {
        const auto vert = LoadShader(device, ribbon ? "particlePool.ribbonVertex.spv" : "particlePool.vertexMain.spv");
        const auto frag = LoadShader(device, ribbon ? "particlePool.ribbonFragment.spv" : "particlePool.fragmentMain.spv");
        const VkPipelineColorBlendAttachmentState blend{
            .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 15};
        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vert,
             .pName = ribbon ? "ribbonVertex" : "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = frag,
             .pName = ribbon ? "ribbonFragment" : "fragmentMain" } };

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
            .pAttachments = &blend };

        const VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &format };

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
            .layout = layout };

        const bool ok = vert && frag && vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS;
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return ok;
    }
}
