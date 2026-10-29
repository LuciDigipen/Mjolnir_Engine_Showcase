/**
 * @file DebugDraw.cpp
 * @author Rahul Nair
 * @brief The editor's debug views drawn over a finished frame - each view's
 *        meshes as wireframe lines, for both the raster and ray-traced paths.
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
#include "VulkanConstants.h"
#include "VulkanPushConstants.h"
#include "VulkanUtil.h"

#include <renderer/VulkanRenderer.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>

namespace mir::vk
{
    namespace
    {
        // Wireframe alone: light lines on the clear color
        constexpr glm::vec4 kWireframeColor{ 0.85f, 0.85f, 0.85f, 1.0f };
        // over the shaded scene: dark and see-through, so the shading still reads
        constexpr glm::vec4 kOverlayColor{ 0.0f, 0.0f, 0.0f, 0.6f };

        bool DrawsLines(const RenderView &view)
        {
            return view.debugView.shading != ViewportShading::Render;
        }
    }

    bool DebugDraw::Initialize(Device &device, VkFormat colorFormat)
    {
        mDevice = &device;
        if (!device.FillModeNonSolidSupported())
        {
            MIR_LOG_WARN("DebugDraw: the GPU has no fillModeNonSolid; the Wireframe view draws the shaded scene");
            return true;
        }

        VkShaderModule vertModule = CreateShaderModule(device.Handle(), ShaderPath("debugWireframe.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(device.Handle(), ShaderPath("debugWireframe.fragmentMain.spv"));
        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(device.Handle(), vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(device.Handle(), fragModule, nullptr);
            return false;
        }

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(DebugWireframePushConstants) };

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        bool created = vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &mLayout) == VK_SUCCESS;
        if (!created)
            MIR_LOG_CRITICAL("DebugDraw: vkCreatePipelineLayout failed");

        created = created && CreatePipeline(vertModule, fragModule, colorFormat, true, mDepthPipeline) &&
            CreatePipeline(vertModule, fragModule, colorFormat, false, mColorPipeline);

        vkDestroyShaderModule(device.Handle(), vertModule, nullptr);
        vkDestroyShaderModule(device.Handle(), fragModule, nullptr);

        if (!created)
            Shutdown();
        return created;
    }

    bool DebugDraw::CreatePipeline(VkShaderModule vertModule, VkShaderModule fragModule, VkFormat colorFormat,
        bool withDepth, VkPipeline &outPipeline)
    {
        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertModule, .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragModule, .pName = "fragmentMain" } };

        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const VkVertexInputAttributeDescription positionAttribute{
            .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &positionAttribute };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
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
            .polygonMode = VK_POLYGON_MODE_LINE,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = withDepth ? VK_TRUE : VK_FALSE,
            .depthBiasConstantFactor = 1.0f,
            .depthBiasSlopeFactor = 1.0f,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        // reads the scene's depth, never writes it
        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
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
            .dynamicStateCount = withDepth ? 3u : 2u,
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
            .layout = mLayout,
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

        if (mDepthPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), mDepthPipeline, nullptr);
        if (mColorPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(mDevice->Handle(), mColorPipeline, nullptr);
        if (mLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(mDevice->Handle(), mLayout, nullptr);
        mDepthPipeline = VK_NULL_HANDLE;
        mColorPipeline = VK_NULL_HANDLE;
        mLayout = VK_NULL_HANDLE;
    }

    bool DebugDraw::Wants(std::span<const RenderView> views) const
    {
        return Available() && std::any_of(views.begin(), views.end(), DrawsLines);
    }

    void DebugDraw::Record(VkCommandBuffer cmd, std::span<const RenderView> views, VkImageView color,
        VkExtent2D extent, VkImageView depth, const ResourceStore &resources) const
    {
        if (!Available())
            return;

        const bool withDepth = depth != VK_NULL_HANDLE;

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
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, withDepth ? mDepthPipeline : mColorPipeline);

        for (const RenderView &view : views)
        {
            if (!DrawsLines(view))
                continue;

            const VkRect2D rect = ResolveViewRect(view.rect, extent);
            if (rect.extent.width == 0 || rect.extent.height == 0)
                continue;
            const VkViewport viewport{
                .x = static_cast<float>(rect.offset.x),
                .y = static_cast<float>(rect.offset.y),
                .width = static_cast<float>(rect.extent.width),
                .height = static_cast<float>(rect.extent.height),
                .minDepth = 0.0f,
                .maxDepth = 1.0f };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);

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
                vkCmdPushConstants(cmd, mLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(DebugWireframePushConstants), &pushData);

                vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
            }
        }

        vkCmdEndRendering(cmd);
    }
}
