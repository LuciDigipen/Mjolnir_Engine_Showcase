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

#include <format>

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

        void NVSDK_CONV NgxLogCallback(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature)
        {
            if (level == NVSDK_NGX_LOGGING_LEVEL_OFF)
                return;
            MIR_LOG_INFO("[NGX] {}", message);
        }
    }

    bool DlssRayReconstruction::Initialize(Device &device, VkExtent2D extent)
    {
        mDevice = &device;

        const NVSDK_NGX_LoggingInfo loggingInfo{
            .LoggingCallback = &NgxLogCallback,
            .MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE,
            .DisableOtherLoggingSinks = false };
        const NVSDK_NGX_FeatureCommonInfo featureInfo{ .LoggingInfo = loggingInfo };

        const NVSDK_NGX_Result initResult = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kNgxProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
            L".", mDevice->Instance(), mDevice->Physical(), mDevice->Handle(),
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

        return CreateOutputImages(extent) && CreateFeature(extent);
    }

    bool DlssRayReconstruction::CreateOutputImages(VkExtent2D extent)
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kHdrColorFormat,
                .extent{ extent.width, extent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mDevice->Allocator(), &imageInfo, &allocInfo,
                &mOutputImages[frame], &mOutputAllocations[frame], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateOutputImages: vmaCreateImage failed for Dlss output {}", frame);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mOutputImages[frame],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kHdrColorFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice->Handle(), &viewInfo, nullptr, &mOutputViews[frame]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateOutputImages: vkCreateImageView failed for Dlss output {}", frame);
                return false;
            }

            mDevice->NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mOutputImages[frame]),
                std::format("Dlss Ray Reconstruction output ({})", frame).c_str());
        }

        return true;
    }

    void DlssRayReconstruction::DestroyOutputImages()
    {
        for (uint32_t frame = 0; frame < kFramesInFlight; ++frame)
        {
            if (mOutputViews[frame] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice->Handle(), mOutputViews[frame], nullptr);
                mOutputViews[frame] = VK_NULL_HANDLE;
            }
            if (mOutputImages[frame] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mDevice->Allocator(), mOutputImages[frame], mOutputAllocations[frame]);
                mOutputImages[frame] = VK_NULL_HANDLE;
                mOutputAllocations[frame] = VK_NULL_HANDLE;
            }
        }
    }

    bool DlssRayReconstruction::CreateFeature(VkExtent2D extent)
    {
        NVSDK_NGX_DLSSD_Create_Params createParams{};
        createParams.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        // Packed: our gNormal carries roughness in .w rather than a separate buffer.
        createParams.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        // Linear: our gDistance is a ray-hit distance, not a projected hardware depth buffer -
        // a documented simplification (no projected depth exists on the RT path at all).
        createParams.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_Linear;
        createParams.InWidth = extent.width;
        createParams.InHeight = extent.height;
        // Native resolution (DLAA-style denoise+AA, no upscale ratio) - the acceptance criteria
        // never ask for a render-scale control.
        createParams.InTargetWidth = extent.width;
        createParams.InTargetHeight = extent.height;
        createParams.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
        createParams.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
        createParams.InEnableOutputSubrects = false;

        bool ok = false;
        SubmitAndWait(*mDevice, [&](VkCommandBuffer cmd)
            {
                const NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSSD_EXT1(
                    mDevice->Handle(), cmd, 0, 0, &mHandle, mParams, &createParams);
                ok = NVSDK_NGX_SUCCEED(result);
                if (!ok)
                    MIR_LOG_ERROR("CreateFeature: NGX_VULKAN_CREATE_DLSSD_EXT1 failed: {:#x}",
                        static_cast<unsigned>(result));
            });
        return ok;
    }

    void DlssRayReconstruction::DestroyFeature()
    {
        if (mHandle != nullptr)
        {
            NVSDK_NGX_VULKAN_ReleaseFeature(mHandle);
            mHandle = nullptr;
        }
    }

    bool DlssRayReconstruction::Resize(VkExtent2D extent)
    {
        DestroyFeature();
        DestroyOutputImages();
        return CreateOutputImages(extent) && CreateFeature(extent);
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

    void DlssRayReconstruction::Record(VkCommandBuffer cmd, uint32_t frame, const Inputs &inputs, VkExtent2D extent)
    {
        // Color/depth/velocity arrive VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL - RayTracer::Record
        // always leaves them there for the fragment-shader samplers (DoF/MotionBlur/Bloom) that
        // read them on every other anti-aliasing mode. NGX's Resource_VK carries no layout of its
        // own and expects VK_IMAGE_LAYOUT_GENERAL (the same convention every compute-driven
        // storage-image consumer in this module already uses), so they round-trip through GENERAL
        // for this call and back. gAlbedo/gNormal need no such round-trip: RayTracer leaves them in
        // GENERAL already, since nothing else ever samples them.
        ImageBarrier(cmd, inputs.colorImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        ImageBarrier(cmd, inputs.depthImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        ImageBarrier(cmd, inputs.velocityImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        ImageBarrier(cmd, mOutputImages[frame], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);

        const NVSDK_NGX_Resource_VK colorRes = WrapInput(inputs.colorView, inputs.colorImage, kHdrColorFormat, extent);
        const NVSDK_NGX_Resource_VK depthRes = WrapInput(inputs.depthView, inputs.depthImage, kDistanceFormat, extent);
        const NVSDK_NGX_Resource_VK velocityRes =
            WrapInput(inputs.velocityView, inputs.velocityImage, kHdrColorFormat, extent);
        const NVSDK_NGX_Resource_VK albedoRes = WrapInput(inputs.albedoView, inputs.albedoImage, kAlbedoFormat, extent);
        const NVSDK_NGX_Resource_VK normalRes = WrapInput(inputs.normalView, inputs.normalImage, kHdrColorFormat, extent);
        const VkImageSubresourceRange outputRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        const NVSDK_NGX_Resource_VK outputRes = NVSDK_NGX_Create_ImageView_Resource_VK(
            mOutputViews[frame], mOutputImages[frame], outputRange, kHdrColorFormat,
            extent.width, extent.height, /*readWrite*/ true);

        NVSDK_NGX_VK_DLSSD_Eval_Params evalParams{};
        evalParams.pInColor = const_cast<NVSDK_NGX_Resource_VK *>(&colorRes);
        evalParams.pInOutput = const_cast<NVSDK_NGX_Resource_VK *>(&outputRes);
        evalParams.pInDepth = const_cast<NVSDK_NGX_Resource_VK *>(&depthRes);
        evalParams.pInMotionVectors = const_cast<NVSDK_NGX_Resource_VK *>(&velocityRes);
        evalParams.pInDiffuseAlbedo = const_cast<NVSDK_NGX_Resource_VK *>(&albedoRes);
        evalParams.pInNormals = const_cast<NVSDK_NGX_Resource_VK *>(&normalRes);
        evalParams.InJitterOffsetX = inputs.jitterPixels.x;
        evalParams.InJitterOffsetY = inputs.jitterPixels.y;
        evalParams.InRenderSubrectDimensions = { extent.width, extent.height };

        const NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSSD_EXT(cmd, mHandle, mParams, &evalParams);
        if (!NVSDK_NGX_SUCCEED(result))
            MIR_LOG_ERROR("Record: NGX_VULKAN_EVALUATE_DLSSD_EXT failed: {:#x}", static_cast<unsigned>(result));

        ImageBarrier(cmd, mOutputImages[frame], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

        // DepthOfField and MotionBlur still read depth/velocity after this - color doesn't go
        // back, this subsystem's own output replaces it as colorSource from here on.
        ImageBarrier(cmd, inputs.depthImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
        ImageBarrier(cmd, inputs.velocityImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    }
}
