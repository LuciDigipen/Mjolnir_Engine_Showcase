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
#include "vulkan/CameraBuffer.h"
#include "vulkan/DebugDraw.h"
#include "vulkan/Device.h"
#include "vulkan/FrameCapture.h"
#include "vulkan/FrameSync.h"
#include "vulkan/ImGuiOverlay.h"
#include "vulkan/MaterialStore.h"
#include "vulkan/MeshUploader.h"
#include "vulkan/ModelDescriptors.h"
#include "vulkan/PostProcessingStack.h"
#include "vulkan/RayTracer.h"
#include "vulkan/RasterSurfaces.h"
#include "vulkan/SsgiTrace.h"
#if MIR_WITH_RTXGI
#include "vulkan/Ddgi.h"
#endif
#include "vulkan/ResourceStore.h"
#include "vulkan/ShadowMaps.h"
#include "vulkan/Swapchain.h"
#include "vulkan/VulkanConstants.h"
#include "vulkan/VulkanUtil.h"
#include "vulkan/VulkanPushConstants.h"

#include <core/log/Log.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>

namespace mir
{
    uint32_t VulkanRenderer::RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex,
        RasterPass pass, uint32_t cameraOffset)
    {
        const bool surfaces = pass == RasterPass::Surface;
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

        auto &modelSets = mRasterSurfaces->ModelSets(viewIndex);
        if (pass == RasterPass::Scene || pass == RasterPass::Opaque)
        {
            cameraOffset = WriteCameraData(view, viewIndex);
            if (mRasterSurfaces->Active()) modelSets.assign(view.items.size(), VK_NULL_HANDLE);
        }
        const VkDescriptorSet cameraSet = mCameraBuffer->Set(mFrameSync->Index());
        const VkDescriptorSet shadowSet = mShadowMaps->Set(mFrameSync->Index());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            1, 1, &cameraSet, 1, &cameraOffset);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
            3, 1, &shadowSet, 0, nullptr);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundDescriptorSet = VK_NULL_HANDLE;

        // Wireframe draws only lines, over the clear color; without line support it stays shaded
        const bool shaded = view.debugView.shading != ViewportShading::Wireframe || !mDebugDraw->WireframeAvailable();
        const size_t drawCount = shaded ? view.items.size() : 0;

        // no mesh/material batching yet, just draw per item
        // EQUAL replay runs in reverse so the original GREATER winner owns depth ties.
        for (size_t iteration = 0; iteration < drawCount; ++iteration)
        {
            const size_t draw = surfaces ? view.items.size() - 1 - iteration : iteration;
            const DrawItem &item = view.items[draw];
            const vk::GpuMaterial *const material = mMaterialStore->FindOrDefault(item.material);
            if (material == nullptr) continue;
            const bool transparent = item.transparent || material->mDescription.alphaMode == MaterialAlphaMode::Blend;
            if ((pass == RasterPass::Opaque || surfaces) && transparent) continue;
            if (pass == RasterPass::Transparent && !transparent) continue;
            const vk::GpuMesh *const mesh = mResources->FindMesh(item.mesh);
            if (mesh == nullptr)
            {
                if (!surfaces && !item.mesh.IsNull())
                    ++mSkippedDraws;
                continue;
            }

            const VkPipeline pipeline = surfaces ? mMaterialStore->SurfacePipeline(item.material) : material->mPipeline;
            if (!pipeline) continue;
            if (material->mModelShader)
            {
                if (surfaces)
                {
                    if (!modelSets[draw]) continue;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->PipelineLayout(),
                        2, 1, &modelSets[draw], 0, nullptr);
                }
                else
                {
                    const VkDescriptorSet set = mModelDescriptors->Bind(cmd, mMaterialStore->PipelineLayout(),
                        mFrameSync->Index(), item.deformation, material->mParameters);
                    if (!set) continue;
                    if (mRasterSurfaces->Active()) modelSets[draw] = set;
                }
            }

            if (pipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundPipeline = pipeline;
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
        if (mSkyVisible && shaded && (pass == RasterPass::Scene || pass == RasterPass::Opaque))
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->SkyPipeline());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mMaterialStore->SkyPipelineLayout(),
                0, 1, &cameraSet, 1, &cameraOffset);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        return cameraOffset;
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
        const bool ssgi = mSsgiTrace->Active();
        std::array<uint32_t, vk::kMaxViewsPerFrame> cameraOffsets{};
        for (size_t i = 0; i < viewCount; ++i)
            cameraOffsets[i] = RecordView(cmd, views[i], static_cast<uint32_t>(i), ssgi ? RasterPass::Opaque : RasterPass::Scene);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        if (mRasterSurfaces->Active())
        {
            mDevice->BeginLabel(cmd, "Raster Surface Pass", { 0.5f, 0.8f, 0.3f, 1.0f });
            mRasterSurfaces->Begin(cmd, frame, mSwapchain->DepthImage(frame), mSwapchain->DepthView(frame));
            for (size_t i = viewCount; i-- > 0;)
                RecordView(cmd, views[i], uint32_t(i), RasterPass::Surface, cameraOffsets[i]);
            mRasterSurfaces->End(cmd, frame, mSwapchain->DepthImage(frame));
            mDevice->EndLabel(cmd);
            mDevice->BeginLabel(cmd, "SSGI Trace", { 0.3f, 0.8f, 0.5f, 1.0f });
            mSsgiTrace->Record(cmd, frame, mSwapchain->DepthImage(frame), mSwapchain->DepthView(frame),
                *mRasterSurfaces, mCameraBuffer->Set(frame), std::span(cameraOffsets).first(viewCount), mSsgiSettings);
            mDevice->EndLabel(cmd);
        }

        if (ssgi)
        {
            mDevice->BeginLabel(cmd, "SSGI Composition", { 0.3f, 0.8f, 0.5f, 1.0f });
            mSsgiTrace->Compose(cmd, frame, mSwapchain->HdrColorImage(frame), mSwapchain->HdrColorView(frame), extent,
                mSwapchain->DepthImage(frame), mSwapchain->DepthView(frame), *mRasterSurfaces,
                mCameraBuffer->Set(frame), std::span(cameraOffsets).first(viewCount), mSsgiSettings);
            mDevice->EndLabel(cmd);
            vk::ImageBarrier(cmd, mSwapchain->HdrColorImage(frame), VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            auto colorLoad = hdrColorAttachment; colorLoad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            auto depthLoad = depthAttachment; depthLoad.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            auto transparentInfo = renderingInfo;
            transparentInfo.pColorAttachments = &colorLoad; transparentInfo.pDepthAttachment = &depthLoad;
            vkCmdBeginRendering(cmd, &transparentInfo);
            mDevice->BeginLabel(cmd, "Transparent Pass", { 0.5f, 0.5f, 0.8f, 1.0f });
            for (size_t i = 0; i < viewCount; ++i)
                RecordView(cmd, views[i], uint32_t(i), RasterPass::Transparent, cameraOffsets[i]);
            mDevice->EndLabel(cmd);
            vkCmdEndRendering(cmd);
        }

        vk::ImageBarrier(cmd, mSwapchain->HdrColorImage(frame), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        const std::span<const RenderView> drawnViews = views.subspan(0, viewCount);

        const VkImageView colorSource = mPostProcessing->RecordRaster(cmd, frame, {
            .sceneColor = mSwapchain->HdrColorView(frame),
            .depthImage = mSwapchain->DepthImage(frame),
            .depthView = mSwapchain->DepthView(frame),
            .views = drawnViews,
            .cameraSet = mCameraBuffer->Set(frame),
            .cameraOffsets = std::span(cameraOffsets).first(viewCount),
            .resources = mResources.get(),
            .extent = extent });

        const bool fxaaActive = mPostProcessing->PrepareComposite(cmd, frame, colorSource, extent);

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Every pixel is about to be overwritten by the fullscreen tonemap/FXAA triangle.
        vk::BeginFullscreenPass(cmd, mSwapchain->View(imageIndex), extent);
        mDevice->BeginLabel(cmd, fxaaActive ? "FXAA Pass" : "Tonemap Pass", { 1.0f, 1.0f, 0.0f, 1.0f });

        mPostProcessing->RecordComposite(cmd, frame, colorSource, fxaaActive, extent);

        RecordDebugViews(cmd, imageIndex, drawnViews, mSwapchain->DepthImage(frame), mSwapchain->DepthView(frame));

        mImGuiOverlay->RecordInPass(cmd);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        const bool captured = mCapture->Record(cmd, *mSwapchain, imageIndex);
        mSwapchain->RecordPresentTransition(cmd, imageIndex,
            captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    void VulkanRenderer::RecordDebugViews(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views,
        VkImage sceneDepth, VkImageView sceneDepthView)
    {
        // nothing on: no extra pass, no barriers
        if (!mDebugDraw->Wants(views))
            return;

        const VkImage swapchainImage = mSwapchain->Image(imageIndex);
        const VkExtent2D extent = mSwapchain->Extent();

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        vk::ImageBarrier(cmd, swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        // post-processing left the scene depth for sampling; the lines only test against it
        if (sceneDepth != VK_NULL_HANDLE)
        {
            vk::ImageBarrier(cmd, sceneDepth, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        }

        mDevice->BeginLabel(cmd, "Debug View Pass", { 0.9f, 0.9f, 0.9f, 1.0f });
        mDebugDraw->Record(cmd, mFrameSync->Index(), views, mSwapchain->View(imageIndex), extent, sceneDepthView,
            *mResources);
        mDevice->EndLabel(cmd);

        vk::ImageBarrier(cmd, swapchainImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // ImGui draws last, over the lines
        vk::BeginFullscreenPass(cmd, mSwapchain->View(imageIndex), extent, VK_ATTACHMENT_LOAD_OP_LOAD);
        mDevice->BeginLabel(cmd, "ImGui Pass", { 1.0f, 1.0f, 0.0f, 1.0f });
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
        const bool hadSurfaces = mRasterSurfaces->Active();
        bool captureSurfaces = mSsgiSettings.enabled && mSsgiSettings.ValidationError() == nullptr && mSsgiSettings.intensity > 0;
        if (captureSurfaces)
            for (const auto &view : views)
                for (const auto &item : view.items)
                    if (!item.transparent && !mMaterialStore->SupportsSurfaceInputs(item.material)) captureSurfaces = false;
        mRasterSurfaces->Configure(captureSurfaces, mSwapchain->Extent());
        if (hadSurfaces && !mRasterSurfaces->Active()) mMaterialStore->ReleaseSurfacePipelines();
#if MIR_WITH_RTXGI
        mDdgi->Configure(mDdgiSettings, false);
#endif
        mGiWasShading = false;
        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        mSsgiTrace->Configure(mRasterSurfaces->Active(), mSwapchain->Extent(), views, mSsgiSettings.halfResolution);
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

        const AntiAliasingMode effectiveAaMode = mPostProcessing->RtAntiAliasingMode();
        const bool applyJitter = effectiveAaMode == AntiAliasingMode::Dlss;
        glm::vec2 jitterPixels{ 0.0f };
        const uint32_t cameraOffset = WriteCameraData(view, 0, applyJitter, &jitterPixels);

        mDevice->BeginLabel(cmd, "RT Scene Preparation", { 0.0f, 0.7f, 1.0f, 1.0f });
        bool extended = false;
#if MIR_WITH_RTXGI
        extended = mDdgi->CanUpdate(view.cameraPosition) && view.rayTracingItems.has_value() && view.rayTracingSceneComplete;
#endif
        [[maybe_unused]] const auto scene = mRayTracer->PrepareScene(cmd, frame,
            extended ? *view.rayTracingItems : view.items, *mResources, *mMaterialStore,
            extended ? std::optional(view.items) : std::nullopt);
        mDevice->EndLabel(cmd);

        std::optional<vk::RayTracer::GiShading> gi;
#if MIR_WITH_RTXGI
        gi = mDdgi->Record(cmd, frame, view.cameraPosition, mCameraBuffer->Set(frame), cameraOffset,
            scene, view.rayTracingSceneComplete);
#endif
        if (gi.has_value() != mGiWasShading)
        {
            mPostProcessing->ResetHistory();
            mGiWasShading = gi.has_value();
        }
        mDevice->BeginLabel(cmd, "Ray Trace Pass", { 0.0f, 1.0f, 1.0f, 1.0f });
        mRayTracer->RecordTrace(cmd, frame, rect, mCameraBuffer->Set(frame),
            cameraOffset, effectiveAaMode, mPostProcessing->RtSampleCount(), gi ? &*gi : nullptr);
        mDevice->EndLabel(cmd);

        const bool hasRect = rect.extent.width > 0 && rect.extent.height > 0;

        const VkImageView colorSource = hasRect
            ? mPostProcessing->RecordRayTraced(cmd, frame, *mRayTracer, rect, jitterPixels, view.nearZ, view.farZ,
                mSwapchain->Extent())
            : mRayTracer->View(vk::RtImage::Output, frame);

        vk::ImageBarrier(cmd, mSwapchain->Image(imageIndex), VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        // Cleared: outside the view rect nothing else draws.
        vk::BeginFullscreenPass(cmd, mSwapchain->View(imageIndex), mSwapchain->Extent(), VK_ATTACHMENT_LOAD_OP_CLEAR,
            VkClearColorValue{ { mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } });
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

            mPostProcessing->RecordComposite(cmd, frame, colorSource, /*fxaa*/ false, mSwapchain->Extent());
        }

        // the trace leaves no raster depth, so its lines all show
        RecordDebugViews(cmd, imageIndex, views.first(1), VK_NULL_HANDLE, VK_NULL_HANDLE);

        mImGuiOverlay->RecordInPass(cmd);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        const bool captured = mCapture->Record(cmd, *mSwapchain, imageIndex);
        mSwapchain->RecordPresentTransition(cmd, imageIndex,
            captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    void VulkanRenderer::TraceFrame(std::span<const RenderView> views)
    {
        const bool hadSurfaces = mRasterSurfaces->Active();
        mRasterSurfaces->Configure(false, mSwapchain->Extent());
        mSsgiTrace->Configure(false, mSwapchain->Extent(), {}, mSsgiSettings.halfResolution);
        if (hadSurfaces) mMaterialStore->ReleaseSurfacePipelines();
        if (!mDevice->RayTracingSupported())
        {
            MIR_LOG_ERROR("TraceFrame: called without ray tracing support; ignored");
            return;
        }

        if (views.empty())
            return;

        // different camera starts it over.
        if (views[0].historyId != mTracedHistoryId)
        {
            mPostProcessing->ResetHistory();
            mTracedHistoryId = views[0].historyId;
        }

#if MIR_WITH_RTXGI
        mDdgi->Configure(mDdgiSettings, views[0].rayTracingItems.has_value());
#endif

        const std::optional<vk::FrameContext> frame = BeginFrame();
        if (!frame)
            return;

        RecordRtFrame(frame->cmd, frame->imageIndex, views);
        const bool submitted = mFrameSync->End(*mSwapchain, *frame);
        mCapture->Finish(submitted, *mSwapchain);
    }

    uint32_t VulkanRenderer::WriteCameraData(const RenderView &view, uint32_t viewIndex, bool applyJitter,
        glm::vec2 *outJitter)
    {
        const uint32_t frame = mFrameSync->Index();
        const vk::ViewShadowInfo shadow = mShadowMaps->WriteView(frame, view, viewIndex);
        return mCameraBuffer->WriteView(frame, view, viewIndex, shadow, applyJitter, outJitter);
    }
}
