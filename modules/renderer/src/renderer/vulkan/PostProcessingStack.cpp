/**
 * @file PostProcessingStack.cpp
 * @author Rahul Nair
 * @brief Every screen-space post effect behind one lifecycle - see
 *        PostProcessingStack.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "PostProcessingStack.h"
#include "Device.h"
#include "RayTracer.h"
#include "VulkanUtil.h"

#include <core/log/Log.h>

namespace mir::vk
{
    bool PostProcessingStack::Initialize(Device &device, VkFormat swapchainFormat, VkExtent2D extent,
        VkDescriptorSetLayout cameraSetLayout)
    {
        mDevice = &device;

    #if MIR_WITH_DLSS
        if (device.NgxSupported())
        {
            mDlssReady = mDlss.Initialize(device);
            if (!mDlssReady)
                MIR_LOG_ERROR("Initialize: DLSS Ray Reconstruction failed to initialize despite "
                    "NgxSupported() - Dlss is no longer offered and a Dlss request clamps to Off");
        }
    #endif

        return mPostProcess.Initialize(device, swapchainFormat) &&
            mFxaa.Initialize(device, swapchainFormat) &&
            mBloom.Initialize(device, extent) &&
            mDepthOfField.Initialize(device, extent) &&
            mVelocityPass.Initialize(device, cameraSetLayout, extent) &&
            mMotionBlur.Initialize(device, extent);
    }

    void PostProcessingStack::Shutdown()
    {
        mMotionBlur.Shutdown();
        mVelocityPass.Shutdown();
        mDepthOfField.Shutdown();
        mBloom.Shutdown();
        mFxaa.Shutdown();
        mPostProcess.Shutdown();
    #if MIR_WITH_DLSS
        mDlss.Shutdown();
        mDlssReady = false;
    #endif
    }

    bool PostProcessingStack::Resize(VkExtent2D extent)
    {
        if (!mFxaa.Resize(extent) ||
            !mBloom.Resize(extent) ||
            !mDepthOfField.Resize(extent) ||
            !mVelocityPass.Resize(extent) ||
            !mMotionBlur.Resize(extent))
            return false;

    #if MIR_WITH_DLSS
        if (mDlssReady && !mDlss.Resize(extent))
        {
            mDlssReady = false;
            MIR_LOG_WARN("DLSS Ray Reconstruction resize failed; Dlss is no longer offered and a Dlss "
                "request clamps to Off");
        }
    #endif
        return true;
    }

    void PostProcessingStack::Apply(const PostProcessingState &state, RayTracer &rayTracer, VkExtent2D extent)
    {
        mPostProcess.SetParams(state.hdr.exposure, static_cast<int>(state.hdr.op));
        mPostProcess.SetFilmParams(state.film.grain, state.film.contrast, state.film.saturation,
            state.film.vignetteStrength, state.film.vignetteRadius);

        mBloomEnabled = state.bloomEnabled;
        mBloom.SetParams(state.bloom.threshold, state.bloom.intensity, state.bloom.radius);

        mDepthOfFieldEnabled = state.depthOfFieldEnabled;
        mDepthOfField.SetParams(state.depthOfField.focusDistance, state.depthOfField.aperture,
            state.depthOfField.maxBlur);

        mMotionBlurEnabled = state.motionBlurEnabled;
        mMotionBlur.SetParams(state.motionBlur.shutterStrength, static_cast<uint32_t>(state.motionBlur.sampleCount));

        mAntiAliasingRtSampleCount = static_cast<uint32_t>(state.antiAliasing.rtSampleCount);
        const AntiAliasingMode mode = state.antiAliasing.mode;
        if (mode == mAntiAliasingMode)
            return;
        mAntiAliasingMode = mode;

        if (mDevice == nullptr || mDevice->Handle() == VK_NULL_HANDLE)
            return;

        // Each mode's images exist only while it is active; in-flight frames may still use them.
        mDevice->WaitIdle();

        if (mode == AntiAliasingMode::Fxaa)
        {
            if (!mFxaa.CreateIntermediate(extent))
                MIR_LOG_ERROR("Apply: FXAA intermediate allocation failed; rendering without FXAA");
        }
        else
        {
            mFxaa.DestroyIntermediate();
        }

    #if MIR_WITH_DLSS
        mDlss.ResetHistory();
        if (mode == AntiAliasingMode::Dlss && mDlssReady)
        {
            if (!rayTracer.CreateDlssGuides() || !mDlss.Activate(extent))
            {
                mDlssReady = false;
                MIR_LOG_ERROR("Apply: DLSS Ray Reconstruction resources failed to allocate; Dlss is "
                    "no longer offered and a Dlss request clamps to Off");
            }
        }
        if (mode != AntiAliasingMode::Dlss || !mDlssReady)
        {
            mDlss.Deactivate();
            rayTracer.DestroyDlssGuides();
        }
    #else
        (void)rayTracer;
    #endif
    }

    bool PostProcessingStack::DlssAvailable() const
    {
    #if MIR_WITH_DLSS
        return mDlssReady;
    #else
        return false;
    #endif
    }

    void PostProcessingStack::ResetHistory()
    {
    #if MIR_WITH_DLSS
        mDlss.ResetHistory();
    #endif
    }

    VkImageView PostProcessingStack::RecordRaster(VkCommandBuffer cmd, uint32_t frame, const RasterInputs &inputs)
    {
        if (mMotionBlurEnabled)
        {
            ImageBarrier(cmd, inputs.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

            mDevice->BeginLabel(cmd, "Velocity Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mVelocityPass.Record(cmd, frame, inputs.views, inputs.cameraSet, inputs.cameraOffsets,
                inputs.depthView, inputs.extent, *inputs.resources);
            mDevice->EndLabel(cmd);
        }

        ImageBarrier(cmd, inputs.depthImage, VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        VkImageView colorSource = inputs.sceneColor;
        if (mDepthOfFieldEnabled)
        {
            mDevice->BeginLabel(cmd, "Depth Of Field Pass", { 0.5f, 0.0f, 1.0f, 1.0f });
            const float nearZ = inputs.views.empty() ? 0.1f : inputs.views[0].nearZ;
            const float farZ = inputs.views.empty() ? 0.0f : inputs.views[0].farZ;
            mDepthOfField.Record(cmd, frame, inputs.sceneColor, inputs.depthView,
                /*isLinearDepth*/ false, nearZ, farZ, inputs.extent);
            mDevice->EndLabel(cmd);
            colorSource = mDepthOfField.OutputView(frame);
        }

        if (mMotionBlurEnabled)
        {
            mDevice->BeginLabel(cmd, "Motion Blur Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mMotionBlur.Record(cmd, frame, colorSource, mVelocityPass.OutputView(frame), inputs.extent);
            mDevice->EndLabel(cmd);
            colorSource = mMotionBlur.OutputView(frame);
        }

        return colorSource;
    }

    AntiAliasingMode PostProcessingStack::RtAntiAliasingMode() const
    {
        if (mAntiAliasingMode != AntiAliasingMode::Dlss)
            return mAntiAliasingMode;

        bool canReconstruct = false;
    #if MIR_WITH_DLSS
        canReconstruct = mDlssReady && mDlss.IsActive();
    #endif
        return canReconstruct ? AntiAliasingMode::Dlss : AntiAliasingMode::Msaa;
    }

    VkImageView PostProcessingStack::RecordRayTraced(VkCommandBuffer cmd, uint32_t frame, const RayTracer &rayTracer,
        const RenderView &view, glm::vec2 jitterPixels, VkExtent2D extent)
    {
        const VkRect2D rect = ResolveViewRect(view.rect, extent);
        VkImageView colorSource = rayTracer.View(RtImage::Output, frame);

    #if MIR_WITH_DLSS
        // The ray-tracing path has no denoiser today - Ray Reconstruction becomes its denoiser and upscaler
        if (RtAntiAliasingMode() == AntiAliasingMode::Dlss)
        {
            mDevice->BeginLabel(cmd, "Dlss Ray Reconstruction Pass", { 0.0f, 0.5f, 1.0f, 1.0f });
            const DlssRayReconstruction::Inputs dlssInputs{
                .colorImage = rayTracer.Image(RtImage::Output, frame),
                .colorView = rayTracer.View(RtImage::Output, frame),
                .depthImage = rayTracer.Image(RtImage::DlssDepth, frame),
                .depthView = rayTracer.View(RtImage::DlssDepth, frame),
                .velocityImage = rayTracer.Image(RtImage::Velocity, frame),
                .velocityView = rayTracer.View(RtImage::Velocity, frame),
                .albedoImage = rayTracer.Image(RtImage::Albedo, frame),
                .albedoView = rayTracer.View(RtImage::Albedo, frame),
                .normalImage = rayTracer.Image(RtImage::Normal, frame),
                .normalView = rayTracer.View(RtImage::Normal, frame),
                .specularAlbedoImage = rayTracer.Image(RtImage::SpecularAlbedo, frame),
                .specularAlbedoView = rayTracer.View(RtImage::SpecularAlbedo, frame),
                .jitterPixels = jitterPixels };
            const bool reconstructed = mDlss.Record(cmd, frame, dlssInputs, extent, rect);
            mDevice->EndLabel(cmd);
            if (reconstructed)
            {
                colorSource = mDlss.OutputView(frame);
            }
            else
            {
                // DlssAvailable() now reports false, so the next frame's clamp and the panel drop Dlss.
                mDlssReady = false;
                MIR_LOG_WARN("DLSS Ray Reconstruction failed; Dlss is no longer offered and a Dlss request "
                    "clamps to Off");
            }
        }
    #else
        (void)rect;
        (void)jitterPixels;
    #endif

        if (mDepthOfFieldEnabled)
        {
            mDevice->BeginLabel(cmd, "Depth Of Field Pass", { 0.5f, 0.0f, 1.0f, 1.0f });
            mDepthOfField.Record(cmd, frame, colorSource, rayTracer.View(RtImage::Distance, frame),
                /*isLinearDepth*/ true, view.nearZ, view.farZ, extent);
            mDevice->EndLabel(cmd);
            colorSource = mDepthOfField.OutputView(frame);
        }

        if (mMotionBlurEnabled)
        {
            mDevice->BeginLabel(cmd, "Motion Blur Pass", { 0.0f, 1.0f, 0.5f, 1.0f });
            mMotionBlur.Record(cmd, frame, colorSource, rayTracer.View(RtImage::Velocity, frame), extent);
            mDevice->EndLabel(cmd);
            colorSource = mMotionBlur.OutputView(frame);
        }

        return colorSource;
    }

    void PostProcessingStack::RecordBloom(VkCommandBuffer cmd, uint32_t frame, VkImageView color, VkExtent2D extent)
    {
        if (!mBloomEnabled) return;
        mDevice->BeginLabel(cmd, "Bloom Pass", {1, 0.5f, 0, 1});
        mBloom.Record(cmd, frame, color, extent);
        mDevice->EndLabel(cmd);
    }

    bool PostProcessingStack::PrepareComposite(VkCommandBuffer cmd, uint32_t frame, VkImageView color,
        VkExtent2D extent)
    {
        const bool fxaa = mAntiAliasingMode == AntiAliasingMode::Fxaa && mFxaa.HasIntermediate();
        if (!fxaa)
            return false;

        ColorAttachmentBarrier(cmd, mFxaa.IntermediateImage(frame));
        BeginFullscreenPass(cmd, mFxaa.IntermediateView(frame), extent);
        mDevice->BeginLabel(cmd, "Tonemap Pass", { 1.0f, 1.0f, 0.0f, 1.0f });

        // Grain waits for the FXAA pass, so the edge blend can't smear it.
        mPostProcess.Record(cmd, frame, color, mBloom.OutputView(frame), mBloomEnabled ? mBloom.Intensity() : 0.0f, FilmGrain{}, extent);

        mDevice->EndLabel(cmd);
        vkCmdEndRendering(cmd);

        ShaderReadOnlyBarrier(cmd, mFxaa.IntermediateImage(frame));
        return true;
    }

    void PostProcessingStack::RecordComposite(VkCommandBuffer cmd, uint32_t frame, VkImageView color, bool fxaa,
        VkExtent2D extent)
    {
        const FilmGrain grain = mPostProcess.NextFilmGrain();
        if (fxaa)
            mFxaa.Record(cmd, frame, mFxaa.IntermediateView(frame), grain, extent);
        else
            mPostProcess.Record(cmd, frame, color, mBloom.OutputView(frame), mBloomEnabled ? mBloom.Intensity() : 0.0f, grain, extent);
    }
}
