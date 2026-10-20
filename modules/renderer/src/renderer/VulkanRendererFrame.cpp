/**
 * @file VulkanRendererFrame.cpp
 * @author Sumin Park
 * @brief The frame loop for raster and ray traced frames, and the raster pass recording.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <renderer/VulkanRenderer.h>
#include "vulkan/Bloom.h"
#include "vulkan/CameraBuffer.h"
#include "vulkan/DepthOfField.h"
#include "vulkan/Device.h"
#include "vulkan/FrameCapture.h"
#include "vulkan/FrameSync.h"
#include "vulkan/Fxaa.h"
#include "vulkan/ImGuiOverlay.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/MeshUploader.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/MotionBlur.h"
#include "vulkan/PostProcess.h"
#include "vulkan/RayTracer.h"
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/Swapchain.h"
#include "vulkan/VelocityPass.h"
#include "vulkan/VulkanConstants.h"
#include "vulkan/VulkanUtil.h"
#include "vulkan/VulkanPushConstants.h"

#include <core/log/Log.h>

#include <glm/glm.hpp>

#include <algorithm>

namespace mir
{
    void VulkanRenderer::RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex)
    {
        const VkRect2D rect = vk::ResolveViewRect(view.rect, mSwapchain->Extent());

        const VkViewport viewport{
            .x = static_cast<float>(rect.offset.x),
            .y = static_cast<float>(rect.offset.y),
            .width = static_cast<float>(std::max(rect.extent.width, 1u)),
            .height = static_cast<float>(std::max(rect.extent.height, 1u)),
            .minDepth = 0.0f,
            .maxDepth = 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);

        const uint32_t cameraOffset = WriteCameraData(view, viewIndex);
        const VkDescriptorSet cameraSet = mCameraBuffer->Set(mFrameSync->Index());
        const VkDescriptorSet shadowSet = mShadowMaps->Set(mFrameSync->Index());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            1, 1, &cameraSet, 1, &cameraOffset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            3, 1, &shadowSet, 0, nullptr);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundDescriptorSet = VK_NULL_HANDLE;

        // no mesh/material batching yet, just draw per item
        for (const DrawItem &item : view.items)
        {
            const vk::GpuMesh *const mesh = mResources->FindMesh(item.mesh);
            if (mesh == nullptr)
            {
                if (!item.mesh.IsNull())
                    ++mSkippedDraws;
                continue;
            }

            const vk::GpuMaterial *const material = mMaterialStore->FindOrDefault(item.material);
            if (material == nullptr)
                continue;

            if (material->mModelShader &&
                !mModelDescriptors->Bind(cmd, mMaterialStore->PipelineLayout(), mFrameSync->Index(),
                    item.deformation, material->mParameters))
                continue;

            if (material->mPipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->mPipeline);
                boundPipeline = material->mPipeline;
            }

            if (material->mDescriptorSet != boundDescriptorSet)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
                    0, 1, &material->mDescriptorSet, 0, nullptr);
                boundDescriptorSet = material->mDescriptorSet;
            }

            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);

            vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

            const vk::RasterPushConstants pushData{
                .model = item.model,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .tint = item.tint };

            vkCmdPushConstants(cmd, mMaterialStore->PipelineLayout(), VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(vk::RasterPushConstants), &pushData);

            vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
        }

        // Sky last, at the far depth 0: the test (GEQUAL, no write) passes only
        // where nothing above wrote depth, i.e. still at the clear value - a
        // fullscreen ray-marched shader paying its cost only on actual
        // background pixels, with early-z doing the rejection.
        if (mSkyVisible)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->SkyPipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->SkyPipelineLayout(),
                0, 1, &cameraSet, 1, &cameraOffset);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }

    void VulkanRenderer::RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        const uint32_t frame = mFrameSync->Index();
        const VkExtent2D extent = mSwapchain->Extent();

        vk::ImageBarrier(cmd, mSwapchain->HdrColorImage(frame), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        vk::ImageBarrier(cmd, mSwapchain->DepthImage(frame), VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo hdrColorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->HdrColorView(frame),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .color{ { mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } } } };

        const VkRenderingAttachmentInfo depthAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->DepthView(frame),
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .depthStencil{ .depth = 0.0f } } }; // reverse-Z: 0 is the far end

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            // The stored extent, never the window: re-deriving it invites drift.
            .renderArea{ .offset{ 0, 0 }, .extent = extent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &hdrColorAttachment,
            .pDepthAttachment = &depthAttachment };

        vkCmdBeginRendering(cmd, &renderingInfo);
        mDevice->BeginLabel(cmd, "Scene Pass", { 1.0f, 0.0f, 1.0f, 1.0f });

        if (views.size() > vk::kMaxViewsPerFrame)
        {
            MIR_LOG_ERROR("RecordCommands: {} views this frame, only the first {} (kMaxViewsPerFrame) are drawn",
                views.size(), vk::kMaxViewsPerFrame);
        }

        const size_t viewCount = std::min<size_t>(views.size(), vk::kMaxViewsPerFrame);
        for (size_t i = 0; i < viewCount; ++i)
            RecordView(cmd, views[i], static_cast<uint32_t>(i));

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        vk::ImageBarrier(cmd, mSwapchain->HdrColorImage(frame), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        if (mMotionBlurEnabled)
        {
            vk::ImageBarrier(cmd, mSwapchain->DepthImage(frame), VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

            mVelocityCameraOffsets.clear();
            const size_t motionViewCount = std::min<size_t>(views.size(), vk::kMaxViewsPerFrame);
            for (size_t i = 0; i < motionViewCount; ++i)
                mVelocityCameraOffsets.push_back(WriteCameraData(views[i], static_cast<uint32_t>(i)));

            mDevice->BeginLabel(cmd, "Velocity Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mVelocityPass->Record(cmd, frame, views.subspan(0, motionViewCount), mCameraBuffer->Set(frame),
                mVelocityCameraOffsets, mSwapchain->DepthView(frame), extent, *mResources);
            mDevice->EndLabel(cmd);
        }

        vk::ImageBarrier(cmd, mSwapchain->DepthImage(frame), VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        VkImageView colorSource = mSwapchain->HdrColorView(frame);
        if (mDepthOfFieldEnabled)
        {
            mDevice->BeginLabel(cmd, "Depth Of Field Pass", { 0.5f, 0.0f, 1.0f, 1.0f });
            const float nearZ = views.empty() ? 0.1f : views[0].nearZ;
            const float farZ = views.empty() ? 0.0f : views[0].farZ;
            mDepthOfField->Record(cmd, frame, mSwapchain->HdrColorView(frame), mSwapchain->DepthView(frame),
                /*isLinearDepth*/ false, nearZ, farZ, extent);
            mDevice->EndLabel(cmd);
            colorSource = mDepthOfField->OutputView(frame);
        }

        if (mMotionBlurEnabled)
        {
            mDevice->BeginLabel(cmd, "Motion Blur Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mMotionBlur->Record(cmd, frame, colorSource, mVelocityPass->OutputView(frame), extent);
            mDevice->EndLabel(cmd);
            colorSource = mMotionBlur->OutputView(frame);
        }

        if (mBloomEnabled)
        {
            mDevice->BeginLabel(cmd, "Bloom Pass", { 1.0f, 0.5f, 0.0f, 1.0f });
            mBloom->Record(cmd, frame, colorSource, extent);
            mDevice->EndLabel(cmd);
        }

        const bool fxaaActive = mAntiAliasingMode == AntiAliasingMode::Fxaa;
        if (fxaaActive)
        {
            vk::ColorAttachmentBarrier(cmd, mPostProcess->IntermediateImage(frame));

            const VkRenderingAttachmentInfo intermediateAttachment{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView = mPostProcess->IntermediateView(frame),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
            const VkRenderingInfo intermediateRenderingInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea{ .offset{ 0, 0 }, .extent = extent },
                .layerCount = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments = &intermediateAttachment };

            vkCmdBeginRendering(cmd, &intermediateRenderingInfo);
            mDevice->BeginLabel(cmd, "Tonemap Pass", { 1.0f, 1.0f, 0.0f, 1.0f });

            const VkViewport intermediateViewport{
                .x = 0.0f, .y = 0.0f,
                .width = static_cast<float>(extent.width),
                .height = static_cast<float>(extent.height),
                .minDepth = 0.0f, .maxDepth = 1.0f };
            const VkRect2D intermediateScissor{ .offset{ 0, 0 }, .extent = extent };
            vkCmdSetViewport(cmd, 0, 1, &intermediateViewport);
            vkCmdSetScissor(cmd, 0, 1, &intermediateScissor);

            mPostProcess->Record(cmd, frame, colorSource, mBloom->OutputView(frame),
                mBloom->Intensity(), extent);

            mDevice->EndLabel(cmd);
            vkCmdEndRendering(cmd);

            vk::ShaderReadOnlyBarrier(cmd, mPostProcess->IntermediateImage(frame));
        }

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo swapchainAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->View(imageIndex),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            // Every pixel is about to be overwritten by the fullscreen tonemap/FXAA triangle.
            .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE };

        const VkRenderingInfo tonemapRenderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{ 0, 0 }, .extent = extent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &swapchainAttachment };

        vkCmdBeginRendering(cmd, &tonemapRenderingInfo);
        mDevice->BeginLabel(cmd, fxaaActive ? "FXAA Pass" : "Tonemap Pass", { 1.0f, 1.0f, 0.0f, 1.0f });

        const VkViewport viewport{
            .x = 0.0f, .y = 0.0f,
            .width = static_cast<float>(extent.width),
            .height = static_cast<float>(extent.height),
            .minDepth = 0.0f, .maxDepth = 1.0f };
        const VkRect2D scissor{ .offset{ 0, 0 }, .extent = extent };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (fxaaActive)
            mFxaa->Record(cmd, frame, mPostProcess->IntermediateView(frame), extent);
        else
            mPostProcess->Record(cmd, frame, colorSource, mBloom->OutputView(frame),
                mBloom->Intensity(), extent);

        mImGuiOverlay->RecordInPass(cmd);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        const bool captured = mCapture->Record(cmd, *mSwapchain, imageIndex);
        mSwapchain->RecordPresentTransition(cmd, imageIndex,
            captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    std::optional<vk::FrameContext> VulkanRenderer::BeginFrame()
    {
        if (mFrameSync->Stopped() || mWindow->Width() == 0 || mWindow->Height() == 0)
            return std::nullopt;

        if (mSwapchain->NeedsRecreate(*mWindow) && !RecreateSwapchain())
            return std::nullopt;

        const std::optional<vk::FrameContext> frame = mFrameSync->Begin(*mSwapchain);
        // After Begin's wait: this slot's last frame is done with its set 2 buffers.
        if (frame)
        {
            mModelDescriptors->ResetFrame(mFrameSync->Index());
            mMeshUploads->Collect(*mResources, *mRayTracer, mFrameSync->CompletedValue());
            mMeshUploads->Record(frame->cmd, frame->signalValue, *mResources, *mRayTracer);
        }
        return frame;
    }

    void VulkanRenderer::DrawFrame(std::span<const RenderView> views)
    {
        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        mShadowMaps->RecordPasses(frame->cmd, mFrameSync->Index(), *mResources);
        RecordCommands(frame->cmd, frame->imageIndex, views);
        const bool submitted = mFrameSync->End(*mSwapchain, *frame);

        mCapture->Finish(submitted, *mSwapchain);
    }

    void VulkanRenderer::RecordRtFrame(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        const RenderView &view = views[0];

        const VkRect2D rect = vk::ResolveViewRect(view.rect, mSwapchain->Extent());
        const uint32_t frame = mFrameSync->Index();

        const bool applyJitter = mAntiAliasingMode == AntiAliasingMode::Dlss;
        const uint32_t cameraOffset = WriteCameraData(view, 0, applyJitter);

        mDevice->BeginLabel(cmd, "Ray Trace Pass", { 0.0f, 1.0f, 1.0f, 1.0f });
        mRayTracer->Record(cmd, frame, view.items, rect, mCameraBuffer->Set(frame),
            cameraOffset, *mResources, *mMaterialStore, mAntiAliasingMode, mAntiAliasingRtSampleCount);
        mDevice->EndLabel(cmd);

        const bool hasRect = rect.extent.width > 0 && rect.extent.height > 0;

        VkImageView colorSource = mRayTracer->OutputView(frame);
        if (hasRect && mDepthOfFieldEnabled)
        {
            mDevice->BeginLabel(cmd, "Depth Of Field Pass", { 0.5f, 0.0f, 1.0f, 1.0f });
            mDepthOfField->Record(cmd, frame, mRayTracer->OutputView(frame), mRayTracer->DistanceView(frame),
                /*isLinearDepth*/ true, view.nearZ, view.farZ, mSwapchain->Extent());
            mDevice->EndLabel(cmd);
            colorSource = mDepthOfField->OutputView(frame);
        }

        if (hasRect && mMotionBlurEnabled)
        {
            mDevice->BeginLabel(cmd, "Motion Blur Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mMotionBlur->Record(cmd, frame, colorSource, mRayTracer->VelocityView(frame), mSwapchain->Extent());
            mDevice->EndLabel(cmd);
            colorSource = mMotionBlur->OutputView(frame);
        }

        if (hasRect && mBloomEnabled)
        {
            mDevice->BeginLabel(cmd, "Bloom Pass", { 1.0f, 0.5f, 0.0f, 1.0f });
            mBloom->Record(cmd, frame, colorSource, mSwapchain->Extent());
            mDevice->EndLabel(cmd);
        }

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchain->View(imageIndex),
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .color{ { mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } } } };

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea{ .offset{ 0, 0 }, .extent = mSwapchain->Extent() },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment };

        vkCmdBeginRendering(cmd, &renderingInfo);
        mDevice->BeginLabel(cmd, "Tonemap Pass", { 1.0f, 1.0f, 0.0f, 1.0f });

        if (hasRect)
        {
            const VkViewport viewport{
                .x = static_cast<float>(rect.offset.x),
                .y = static_cast<float>(rect.offset.y),
                .width = static_cast<float>(rect.extent.width),
                .height = static_cast<float>(rect.extent.height),
                .minDepth = 0.0f, .maxDepth = 1.0f };
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &rect);

            mPostProcess->Record(cmd, frame, colorSource, mBloom->OutputView(frame),
                mBloom->Intensity(), mSwapchain->Extent());
        }

        mImGuiOverlay->RecordInPass(cmd);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        mSwapchain->RecordPresentTransition(cmd, imageIndex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    void VulkanRenderer::TraceFrame(std::span<const RenderView> views)
    {
        if (!mDevice->RayTracingSupported())
        {
            MIR_LOG_ERROR("TraceFrame: called without ray tracing support; ignored");
            return;
        }

        if (views.empty())
            return;

        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        RecordRtFrame(frame->cmd, frame->imageIndex, views);
        mFrameSync->End(*mSwapchain, *frame);
    }

    uint32_t VulkanRenderer::WriteCameraData(const RenderView &view, uint32_t viewIndex, bool applyJitter)
    {
        const uint32_t frame = mFrameSync->Index();
        const vk::ViewShadowInfo shadow = mShadowMaps->WriteView(frame, view, viewIndex);
        return mCameraBuffer->WriteView(frame, view, viewIndex, shadow, applyJitter);
    }
}
