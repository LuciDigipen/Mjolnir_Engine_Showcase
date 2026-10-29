/**
 * @file DebugDraw.cpp
 * @author Rahul Nair
 * @brief The editor's debug views drawn over a finished frame - each view's
 *        meshes as wireframe lines and its debug line segments such as
 *        colliders, for both the raster and ray-traced paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DebugDraw.h"
#include "Device.h"
#include "ResourceStore.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <renderer/VulkanRenderer.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mir::vk
{
    namespace
    {
        // Wireframe alone: light lines on the clear color
        constexpr glm::vec4 kWireframeColor{ 0.85f, 0.85f, 0.85f, 1.0f };
        // over the shaded scene: dark and see-through, so the shading still reads
        constexpr glm::vec4 kOverlayColor{ 0.0f, 0.0f, 0.0f, 0.6f };

        // 64 MB of segments; a scene past it is drawn without its debug lines
        constexpr size_t kMaxLinePoints = size_t{ 1 } << 22;

        bool DrawsWireframe(const RenderView &view)
        {
            return view.debugView.shading != ViewportShading::Render;
        }

        size_t LinePointCount(std::span<const RenderView> views)
        {
            size_t count = 0;
            for (const RenderView &view : views)
                count += view.debugLines.size();
            return count;
        }

        void SetViewRect(VkCommandBuffer cmd, const VkRect2D &rect)
        {
            const VkViewport viewport{
                .x = static_cast<float>(rect.offset.x),
                .y = static_cast<float>(rect.offset.y),
                .width = static_cast<float>(rect.extent.width),
                .height = static_cast<float>(rect.extent.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);
        }
    }

    bool DebugDraw::Initialize(Device &device, VkFormat colorFormat)
    {
        mDevice = &device;
        const bool created = CreateLines(colorFormat) && CreateWireframe(colorFormat);
        if (!created)
            Shutdown();
        return created;
    }

    bool DebugDraw::CreateWireframe(VkFormat colorFormat)
    {
        if (!mDevice->FillModeNonSolidSupported())
        {
            MIR_LOG_WARN("DebugDraw: the GPU has no fillModeNonSolid; the Wireframe view draws the shaded scene");
            return true;
        }

        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("debugWireframe.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath("debugWireframe.fragmentMain.spv"));

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(DebugWireframePushConstants) };
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        bool created = vertModule != VK_NULL_HANDLE && fragModule != VK_NULL_HANDLE;
        if (created && vkCreatePipelineLayout(mDevice->Handle(), &layoutInfo, nullptr, &mWireframeLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DebugDraw: vkCreatePipelineLayout (wireframe) failed");
            created = false;
        }
        created = created &&
            CreatePipeline(Kind::Wireframe, vertModule, fragModule, colorFormat, true, mWireframeDepth) &&
            CreatePipeline(Kind::Wireframe, vertModule, fragModule, colorFormat, false, mWireframeColor);

        if (vertModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
        return created;
    }

    bool DebugDraw::CreateLines(VkFormat colorFormat)
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice->Handle(), ShaderPath("debugLines.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice->Handle(), ShaderPath("debugLines.fragmentMain.spv"));

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(glm::mat4) };
        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        bool created = vertModule != VK_NULL_HANDLE && fragModule != VK_NULL_HANDLE;
        if (created && vkCreatePipelineLayout(mDevice->Handle(), &layoutInfo, nullptr, &mLinesLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DebugDraw: vkCreatePipelineLayout (lines) failed");
            created = false;
        }
        created = created &&
            CreatePipeline(Kind::Lines, vertModule, fragModule, colorFormat, true, mLinesDepth) &&
            CreatePipeline(Kind::Lines, vertModule, fragModule, colorFormat, false, mLinesColor);

        if (vertModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(mDevice->Handle(), vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE)
            vkDestroyShaderModule(mDevice->Handle(), fragModule, nullptr);
        return created;
    }

    bool DebugDraw::CreatePipeline(Kind kind, VkShaderModule vertModule, VkShaderModule fragModule,
        VkFormat colorFormat, bool withDepth, VkPipeline &outPipeline)
    {
        const bool wireframe = kind == Kind::Wireframe;

        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertModule, .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragModule, .pName = "fragmentMain" } };

        // a wireframe reads the meshes' own vertex buffers; lines read DebugLinePoint
        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = wireframe ? static_cast<uint32_t>(sizeof(Vertex)) : static_cast<uint32_t>(sizeof(DebugLinePoint)),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const VkVertexInputAttributeDescription attributes[]{
            { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,
              .offset = wireframe ? static_cast<uint32_t>(offsetof(Vertex, pos))
                                  : static_cast<uint32_t>(offsetof(DebugLinePoint, position)) },
            { .location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,
              .offset = static_cast<uint32_t>(offsetof(DebugLinePoint, color)) } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            .vertexAttributeDescriptionCount = wireframe ? 1u : 2u,
            .pVertexAttributeDescriptions = attributes };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = wireframe ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1 };

        // Reverse-Z: a positive bias pulls a line toward the camera, off the surface it outlines.
        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = wireframe && withDepth ? VK_TRUE : VK_FALSE,
            .depthBiasConstantFactor = 1.0f,
            .depthBiasSlopeFactor = 1.0f,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        // reads the scene's depth, never writes it; debug lines skip the test altogether
        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = wireframe ? VK_TRUE : VK_FALSE,
            .depthWriteEnable = VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE };

        const VkPipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                VK_COLOR_COMPONENT_A_BIT };

        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment };

        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE };
        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = wireframe && withDepth ? 3u : 2u,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &colorFormat,
            .depthAttachmentFormat = withDepth ? kDepthFormat : VK_FORMAT_UNDEFINED };

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
            .pDepthStencilState = withDepth ? &depthStencil : nullptr,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = wireframe ? mWireframeLayout : mLinesLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        const VkResult result = vkCreateGraphicsPipelines(
            mDevice->Handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &outPipeline);
        if (result != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("DebugDraw: vkCreateGraphicsPipelines failed: {}", static_cast<int>(result));
            outPipeline = VK_NULL_HANDLE;
            return false;
        }
        return true;
    }

    void DebugDraw::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        const VkDevice device = mDevice->Handle();
        for (VkPipeline *pipeline :{ &mWireframeDepth, &mWireframeColor, &mLinesDepth, &mLinesColor })
        {
            if (*pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
        for (VkPipelineLayout *layout :{ &mWireframeLayout, &mLinesLayout })
        {
            if (*layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
        for (LineBuffer &lines : mLineBuffers)
        {
            if (lines.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), lines.buffer, lines.allocation);
            lines = {};
        }
    }

    bool DebugDraw::Wants(std::span<const RenderView> views) const
    {
        const bool linesAvailable = mLinesDepth != VK_NULL_HANDLE;
        return std::any_of(views.begin(), views.end(), [&](const RenderView &view)
            {
                return (WireframeAvailable() && DrawsWireframe(view)) || (linesAvailable && !view.debugLines.empty());
            });
    }

    bool DebugDraw::UploadLines(uint32_t frame, std::span<const RenderView> views)
    {
        const size_t points = LinePointCount(views);
        if (points == 0)
            return false;
        if (points > kMaxLinePoints)
        {
            if (!mWarnedTooManyLines)
            {
                MIR_LOG_WARN("DebugDraw: {} debug line points is past the {} limit; they are not drawn",
                    points, kMaxLinePoints);
                mWarnedTooManyLines = true;
            }
            return false;
        }

        LineBuffer &lines = mLineBuffers[frame];
        const VkDeviceSize bytes = points * sizeof(DebugLinePoint);
        if (bytes > lines.capacity)
        {
            // grows by doubling, so a scene that keeps adding colliders reallocates rarely
            const VkDeviceSize capacity = std::max<VkDeviceSize>(bytes, 2 * lines.capacity);
            if (lines.buffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mDevice->Allocator(), lines.buffer, lines.allocation);
            lines = {};

            VmaAllocationInfo info{};
            if (!CreateBuffer(mDevice->Allocator(), capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                lines.buffer, lines.allocation, &info))
            {
                MIR_LOG_ERROR("DebugDraw: allocating {} bytes of debug lines failed", capacity);
                lines = {};
                return false;
            }
            lines.mapped = info.pMappedData;
            lines.capacity = capacity;
        }

        std::byte *write = static_cast<std::byte *>(lines.mapped);
        for (const RenderView &view : views)
        {
            std::memcpy(write, view.debugLines.data(), view.debugLines.size_bytes());
            write += view.debugLines.size_bytes();
        }
        vmaFlushAllocation(mDevice->Allocator(), lines.allocation, 0, bytes);
        return true;
    }

    void DebugDraw::Record(VkCommandBuffer cmd, uint32_t frame, std::span<const RenderView> views,
        VkImageView color, VkExtent2D extent, VkImageView depth, const ResourceStore &resources)
    {
        if (mDevice == nullptr)
            return;

        const bool withDepth = depth != VK_NULL_HANDLE;
        const bool drawLines = mLinesDepth != VK_NULL_HANDLE && UploadLines(frame, views);

        const VkRenderingAttachmentInfo colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = color,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE };

        const VkRenderingAttachmentInfo depthAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = depth,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_NONE };

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{ 0, 0 }, .extent = extent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
            .pDepthAttachment = withDepth ? &depthAttachment : nullptr };

        vkCmdBeginRendering(cmd, &renderingInfo);

        VkDeviceSize lineOffset = 0;
        for (const RenderView &view : views)
        {
            const VkDeviceSize viewLines = view.debugLines.size_bytes();
            const VkRect2D rect = ResolveViewRect(view.rect, extent);
            if (rect.extent.width == 0 || rect.extent.height == 0)
            {
                lineOffset += viewLines;
                continue;
            }
            SetViewRect(cmd, rect);

            if (WireframeAvailable() && DrawsWireframe(view))
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, withDepth ? mWireframeDepth : mWireframeColor);

                // Wireframe has no shaded scene to hide behind: every edge shows
                const bool overlay = view.debugView.shading == ViewportShading::RenderWireframe;
                if (withDepth)
                    vkCmdSetDepthTestEnable(cmd, overlay ? VK_TRUE : VK_FALSE);
                const glm::vec4 lineColor = overlay ? kOverlayColor : kWireframeColor;

                for (const DrawItem &item : view.items)
                {
                    const GpuMesh *const mesh = resources.FindMesh(item.mesh);
                    if (mesh == nullptr)
                        continue;

                    const VkDeviceSize vertexOffset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);
                    vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

                    const DebugWireframePushConstants pushData{
                        .viewProjModel = view.viewProj * item.model,
                        .color = lineColor };
                    vkCmdPushConstants(cmd, mWireframeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(DebugWireframePushConstants), &pushData);

                    vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
                }
            }

            if (drawLines && !view.debugLines.empty())
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, withDepth ? mLinesDepth : mLinesColor);
                vkCmdPushConstants(cmd, mLinesLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &view.viewProj);
                vkCmdBindVertexBuffers(cmd, 0, 1, &mLineBuffers[frame].buffer, &lineOffset);
                vkCmdDraw(cmd, static_cast<uint32_t>(view.debugLines.size()), 1, 0, 0);
            }
            lineOffset += viewLines;
        }

        vkCmdEndRendering(cmd);
    }
}
