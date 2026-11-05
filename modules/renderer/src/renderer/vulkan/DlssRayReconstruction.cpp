/**
 * @file DlssRayReconstruction.cpp
 * @author Rahul Nair
 * @brief NVIDIA NGX DLSS Ray Reconstruction - see DlssRayReconstruction.h.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DlssRayReconstruction.h"
#include "Device.h"
#include "VulkanUtil.h"

#include <renderer/DlssConfig.h>

#include <core/log/Log.h>

#include <vk_mem_alloc.h>

// Order matters
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>

#include "NgxIdentity.h"

#include <format>
#include <algorithm>

namespace mir::vk
{
    namespace
    {
        NVSDK_NGX_Resource_VK WrapInput(VkImageView view, VkImage image, VkFormat format, VkExtent2D extent)
        {
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            return NVSDK_NGX_Create_ImageView_Resource_VK(
                view, image, range, format, extent.width, extent.height, /*readWrite*/ false);
        }

        void NVSDK_CONV NgxLogCallback(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature) noexcept
        {
            if (level == NVSDK_NGX_LOGGING_LEVEL_OFF)
                return;
            MIR_LOG_INFO("[NGX] {}", message);
        }
    }

    bool DlssRayReconstruction::Initialize(Device &device)
    {
        mDevice = &device;

        const NVSDK_NGX_LoggingInfo loggingInfo{
            .LoggingCallback = &NgxLogCallback,
            .MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE,
            .DisableOtherLoggingSinks = false };
        const NVSDK_NGX_FeatureCommonInfo featureInfo{ .LoggingInfo = loggingInfo };

        const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kNgxProjectId, kNgxEngineType, kNgxEngineVersion,
            NgxApplicationDataPath(), mDevice->Instance(), mDevice->Physical(), mDevice->Handle(),
            vkGetInstanceProcAddr, vkGetDeviceProcAddr, &featureInfo);
        if (!NVSDK_NGX_SUCCEED(initResult))
        {
            MIR_LOG_ERROR("Initialize: NVSDK_NGX_VULKAN_Init_with_ProjectID failed: {:#x}",
                static_cast<unsigned>(initResult));
            return false;
        }

        const NVSDK_NGX_Result paramsResult = NVSDK_NGX_VULKAN_GetCapabilityParameters(&mParams);
        if (!NVSDK_NGX_SUCCEED(paramsResult) || mParams == nullptr)
        {
            MIR_LOG_ERROR("Initialize: NVSDK_NGX_VULKAN_GetCapabilityParameters failed: {:#x}",
                static_cast<unsigned>(paramsResult));
            return false;
        }

        return true;
    }

    bool DlssRayReconstruction::Activate(VkExtent2D extent)
    {
        if (IsActive())
            return true;
        if (!CreateOutputImages(extent))
        {
            DestroyOutputImages();
            return false;
        }
        mResetHistory = true;
        return true;
    }

    void DlssRayReconstruction::Deactivate()
    {
        if (mDevice == nullptr)
            return;
        DestroyFeature();
        DestroyOutputImages();
    }

    void DlssRayReconstruction::SetOptions(bool rayReconstruction, DlssRrPreset preset)
    {
        preset = preset == DlssRrPreset::D ? DlssRrPreset::D : DlssRrPreset::E;
        const bool recreate = rayReconstruction != mRayReconstruction ||
            (rayReconstruction && preset != mPreset);
        mPreset = preset;
        if (!recreate) return;
        if (mHandle != nullptr) mDevice->WaitIdle();
        DestroyFeature();
        mRayReconstruction = rayReconstruction;
        mResetHistory = true;
        MIR_LOG_INFO("DLSS: Ray Reconstruction {}, RR preset {}",
            mRayReconstruction ? "on" : "off (DLAA)", ToString(mPreset));
    }

    bool DlssRayReconstruction::CreateOutputImages(VkExtent2D extent)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
            if (!CreateImage2D(*mDevice, kHdrColorFormat, extent,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    std::format("Dlss Ray Reconstruction output ({})", frame).c_str(), mOutput[frame]))
                return false;

        return true;
    }

    void DlssRayReconstruction::DestroyOutputImages()
    {
        for (Image2D &image : mOutput)
            DestroyImage2D(*mDevice, image);
    }

    bool DlssRayReconstruction::CreateFeature(VkExtent2D extent)
    {
        NVSDK_NGX_DLSSD_Create_Params createParams{};
        createParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        // Packed: our gNormal carries roughness in .w rather than a separate buffer.
        createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        // The RT shader projects the primary hit into the same reversed-Z convention as raster.
        createParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        createParams.InWidth = extent.width;
        createParams.InHeight = extent.height;
        // Native resolution (DLAA-style denoise+AA, no upscale ratio) - the acceptance criteria
        // never ask for a render-scale control.
        createParams.InTargetWidth = extent.width;
        createParams.InTargetHeight = extent.height;
        createParams.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
            NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        createParams.InEnableOutputSubrects = true;

        bool ok = false;
        if (mRayReconstruction)
            NVSDK_NGX_Parameter_SetUI(mParams, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
            mPreset == DlssRrPreset::D ? NVSDK_NGX_RayReconstruction_Hint_Render_Preset_D :
                NVSDK_NGX_RayReconstruction_Hint_Render_Preset_E);
        const bool submitted = SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                NVSDK_NGX_Result result;
                if (mRayReconstruction)
                    result = NGX_VULKAN_CREATE_DLSSD_EXT1(
                        mDevice->Handle(), cmd, 1, 1, &mHandle, mParams, &createParams);
                else
                {
                    NVSDK_NGX_DLSS_Create_Params dlaa{};
                    dlaa.Feature.InWidth = extent.width;
                    dlaa.Feature.InHeight = extent.height;
                    dlaa.Feature.InTargetWidth = extent.width;
                    dlaa.Feature.InTargetHeight = extent.height;
                    dlaa.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
                    dlaa.InFeatureCreateFlags = createParams.InFeatureCreateFlags;
                    dlaa.InEnableOutputSubrects = true;
                    result = NGX_VULKAN_CREATE_DLSS_EXT1(
                        mDevice->Handle(), cmd, 1, 1, &mHandle, mParams, &dlaa);
                }
                ok = NVSDK_NGX_SUCCEED(result);
                if (!ok)
                    MIR_LOG_ERROR("CreateFeature: NGX {} failed: {:#x}",
                        mRayReconstruction ? "Ray Reconstruction" : "DLAA", static_cast<unsigned>(result));
            });
        mResetHistory = true;
        mFeatureExtent = extent;
        if (submitted && ok)
            MIR_LOG_INFO("DLSS: created {}x{} feature, Ray Reconstruction {}, RR preset {}",
                extent.width, extent.height, mRayReconstruction ? "on" : "off (DLAA)", ToString(mPreset));
        return submitted && ok;
    }

    void DlssRayReconstruction::DestroyFeature()
    {
        if (mHandle != nullptr)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(mHandle);
            mHandle = nullptr;
        }
        mFeatureExtent = {};
        mLastEvaluationTime = {};
    }

    bool DlssRayReconstruction::Resize(VkExtent2D extent)
    {
        if (!IsActive())
            return true;
        DestroyOutputImages();
        mResetHistory = true;
        return CreateOutputImages(extent);
    }

    void DlssRayReconstruction::Shutdown()
    {
        if (mDevice == nullptr)
            return;

        DestroyFeature();
        DestroyOutputImages();

        if (mParams != nullptr)
        {
            NVSDK_NGX_VULKAN_DestroyParameters(mParams);
            mParams = nullptr;
        }

        NVSDK_NGX_VULKAN_Shutdown1(mDevice->Handle());
    }

    bool DlssRayReconstruction::Record(VkCommandBuffer cmd, uint32_t frame, const Inputs &inputs,
        VkExtent2D extent, VkRect2D rect)
    {
        // Editor viewports occupy a subrect of the swapchain. The network runs
        // at that viewport's size; resource wrappers still describe the full images.
        if (rect.extent.width != mFeatureExtent.width || rect.extent.height != mFeatureExtent.height)
        {
            // Only a live feature can still be in use by an in-flight frame.
            if (mHandle != nullptr)
                mDevice->WaitIdle();
            DestroyFeature();
            if (!CreateFeature(rect.extent))
                return false;
        }
        if (rect.offset.x != mLastRect.offset.x || rect.offset.y != mLastRect.offset.y)
            mResetHistory = true;
        mLastRect = rect;
        // NGX inputs are sampled images, output is storage. Establish visibility
        // directly from the RT/transfer producers, including cleared sky pixels.
        const VkImage sampledImages[]{ inputs.colorImage, inputs.velocityImage };
        for (VkImage image : sampledImages)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        const VkImage guideImages[]{ inputs.depthImage, inputs.albedoImage,
            inputs.normalImage, inputs.specularAlbedoImage };
        for (VkImage image : guideImages)
            ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);

        ImageBarrier(cmd, mOutput[frame].image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);

        // The feature owns temporal resources shared between frame slots. Order
        // all preceding NGX work, including transfer operations, before reusing it.
        const VkMemoryBarrier2 historyBarrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT };
        const VkDependencyInfo historyDependency{
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount = 1, .pMemoryBarriers = &historyBarrier };
        vkCmdPipelineBarrier2(cmd, &historyDependency);

        auto colorRes = WrapInput(inputs.colorView, inputs.colorImage, kHdrColorFormat, extent);
        auto depthRes = WrapInput(inputs.depthView, inputs.depthImage, kDistanceFormat, extent);
        auto velocityRes = WrapInput(inputs.velocityView, inputs.velocityImage, kRtVelocityFormat, extent);
        auto albedoRes = WrapInput(inputs.albedoView, inputs.albedoImage, kAlbedoFormat, extent);
        auto normalRes = WrapInput(inputs.normalView, inputs.normalImage, kHdrColorFormat, extent);
        auto specularRes = WrapInput(inputs.specularAlbedoView, inputs.specularAlbedoImage, kHdrColorFormat, extent);
        const VkImageSubresourceRange outputRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        auto outputRes = NVSDK_NGX_Create_ImageView_Resource_VK(
            mOutput[frame].view, mOutput[frame].image, outputRange, kHdrColorFormat,
            extent.width, extent.height, /*readWrite*/ true);

        NVSDK_NGX_VK_DLSSD_Eval_Params evalParams{};
        evalParams.pInColor = &colorRes;
        evalParams.pInOutput = &outputRes;
        evalParams.pInDepth = &depthRes;
        evalParams.pInMotionVectors = &velocityRes;
        evalParams.pInDiffuseAlbedo = &albedoRes;
        evalParams.pInSpecularAlbedo = &specularRes;
        evalParams.pInNormals = &normalRes;
        evalParams.pInRoughness = &normalRes;
        // Ray generation adds the sample offset. NGX receives the offset that
        // removes that jitter, and motion in render pixels toward the previous frame.
        evalParams.InJitterOffsetX = -inputs.jitterPixels.x;
        evalParams.InJitterOffsetY = -inputs.jitterPixels.y;
        evalParams.InMVScaleX = -0.5f * rect.extent.width;
        evalParams.InMVScaleY = -0.5f * rect.extent.height;
        evalParams.InReset = mResetHistory;
        evalParams.InRenderSubrectDimensions = { rect.extent.width, rect.extent.height };
        const NVSDK_NGX_Coordinates base{ static_cast<unsigned>(rect.offset.x), static_cast<unsigned>(rect.offset.y) };
        evalParams.InColorSubrectBase = base;
        evalParams.InDepthSubrectBase = base;
        evalParams.InMVSubrectBase = base;
        evalParams.InDiffuseAlbedoSubrectBase = base;
        evalParams.InSpecularAlbedoSubrectBase = base;
        evalParams.InNormalsSubrectBase = base;
        evalParams.InRoughnessSubrectBase = base;
        evalParams.InOutputSubrectBase = base;

        // NGX consumes a rendered-frame interval, including time spent waiting
        // for the previous frame. Restarts/cuts use a neutral first-frame interval.
        const auto evaluationTime = std::chrono::steady_clock::now();
        const double elapsedMilliseconds = mResetHistory || mLastEvaluationTime == std::chrono::steady_clock::time_point{}
            ? 1000.0 / 60.0
            : std::chrono::duration<double, std::milli>(evaluationTime - mLastEvaluationTime).count();
        evalParams.InFrameTimeDeltaInMsec = static_cast<float>(std::clamp(elapsedMilliseconds, 0.1, 1000.0));

        NVSDK_NGX_Result result;
        if (mRayReconstruction)
            result = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, mHandle, mParams, &evalParams);
        else
        {
            // Standard DLSS at native resolution provides DLAA without the RR denoiser.
            // Preserve editor subrect addressing and the same motion/jitter convention.
            NVSDK_NGX_VK_DLSS_Eval_Params dlaa{};
            dlaa.Feature.pInColor = &colorRes;
            dlaa.Feature.pInOutput = &outputRes;
            dlaa.pInDepth = &depthRes;
            dlaa.pInMotionVectors = &velocityRes;
            dlaa.InJitterOffsetX = evalParams.InJitterOffsetX;
            dlaa.InJitterOffsetY = evalParams.InJitterOffsetY;
            dlaa.InMVScaleX = evalParams.InMVScaleX;
            dlaa.InMVScaleY = evalParams.InMVScaleY;
            dlaa.InReset = evalParams.InReset;
            dlaa.InRenderSubrectDimensions = evalParams.InRenderSubrectDimensions;
            dlaa.InColorSubrectBase = base;
            dlaa.InDepthSubrectBase = base;
            dlaa.InMVSubrectBase = base;
            dlaa.InOutputSubrectBase = base;
            dlaa.InFrameTimeDeltaInMsec = evalParams.InFrameTimeDeltaInMsec;
            result = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, mHandle, mParams, &dlaa);
        }
        const bool succeeded = NVSDK_NGX_SUCCEED(result);
        if (!succeeded)
            MIR_LOG_ERROR("Record: NGX {} evaluation failed: {:#x}",
                mRayReconstruction ? "Ray Reconstruction" : "DLAA", static_cast<unsigned>(result));
        else
        {
            if (mResetHistory)
                MIR_LOG_INFO("DLSS: {} evaluation succeeded, history reset",
                    mRayReconstruction ? "Ray Reconstruction" : "DLAA");
            mResetHistory = false;
            mLastEvaluationTime = evaluationTime;
        }

        ImageBarrier(cmd, mOutput[frame].image, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        return succeeded;
    }
}
