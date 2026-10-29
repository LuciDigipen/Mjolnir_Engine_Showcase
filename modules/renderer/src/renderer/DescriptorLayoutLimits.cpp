/**
 * @file DescriptorLayoutLimits.cpp
 * @author Jackey Cen
 * @brief Descriptor budgets of the shared raster and RT layouts.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DescriptorLayoutLimits.h>

#include "vulkan/MaterialTableLayout.h"

#include <algorithm>

namespace mir
{
    namespace
    {
        // Raster: the legacy sets plus the shared table; fragment also writes color.
        DescriptorUsage RasterUsage()
        {
            constexpr uint32_t combinedImages =
                vk::kMaterialSetCombinedImages + vk::kShadowSetCombinedImages;
            constexpr uint32_t fragmentStorageBuffers =
                vk::kCameraSetStorageBuffers + vk::kModelSetStorageBuffers +
                vk::kShadowSetStorageBuffers + 1;

            return DescriptorUsage{
                .mBoundSets = vk::kRasterPipelineSetCount,
                .mPerStageSamplers = combinedImages + vk::kMaxSharedSamplers,
                .mPerStageSampledImages = combinedImages + vk::kMaxSharedImages,
                .mPerStageUniformBuffers = vk::kMaterialSetRecordUniformBuffers + vk::kCameraSetUniformBuffers,
                .mPerStageStorageBuffers = std::max(vk::kModelSetStorageBuffers + 1, fragmentStorageBuffers),
                .mPerStageStorageImages = 0,
                .mPerStageResources = combinedImages + vk::kMaxSharedImages +
                    vk::kMaterialSetRecordUniformBuffers + vk::kCameraSetUniformBuffers +
                    fragmentStorageBuffers + 1,
                .mLayoutSamplers = combinedImages + vk::kMaxSharedSamplers,
                .mLayoutSampledImages = combinedImages + vk::kMaxSharedImages,
                .mLayoutUniformBuffers = vk::kMaterialSetRecordUniformBuffers + vk::kCameraSetUniformBuffers,
                .mLayoutDynamicUniformBuffers =
                    vk::kMaterialSetRecordUniformBuffers + vk::kCameraSetUniformBuffers,
                .mLayoutStorageBuffers = fragmentStorageBuffers,
                .mLayoutDynamicStorageBuffers = 0,
                .mLayoutStorageImages = 0 };
        }

        // RT: its own table set plus the camera and light set.
        DescriptorUsage RtUsage()
        {
            return DescriptorUsage{
                .mBoundSets = vk::kRtPipelineSetCount,
                .mPerStageSamplers = vk::kMaxSharedSamplers,
                .mPerStageSampledImages = vk::kMaxSharedImages,
                .mPerStageUniformBuffers = vk::kCameraSetUniformBuffers,
                .mPerStageStorageBuffers = vk::kRtSetStorageBuffers + vk::kCameraSetStorageBuffers,
                .mPerStageStorageImages = 4,
                .mPerStageResources = vk::kMaxSharedImages + vk::kRtSetStorageBuffers +
                    vk::kCameraSetStorageBuffers + vk::kCameraSetUniformBuffers + 3,
                .mLayoutSamplers = vk::kMaxSharedSamplers,
                .mLayoutSampledImages = vk::kMaxSharedImages,
                .mLayoutUniformBuffers = vk::kCameraSetUniformBuffers,
                .mLayoutDynamicUniformBuffers = vk::kCameraSetUniformBuffers,
                .mLayoutStorageBuffers = vk::kRtSetStorageBuffers + vk::kCameraSetStorageBuffers,
                .mLayoutDynamicStorageBuffers = 0,
                .mLayoutStorageImages = vk::kRtSetStorageImages };
        }

        // DDGI's trace pipeline binds its own set beside the RT table and camera sets:
        // two storage buffers, five storage images, three sampled images and one
        // sampler, visible to compute, raygen and closest hit (see Ddgi::CreateDescriptors).
        DescriptorUsage DdgiTraceUsage()
        {
            DescriptorUsage usage = RtUsage();
            usage.mBoundSets += 1;
            usage.mPerStageSamplers += 1;
            usage.mPerStageSampledImages += 3;
            usage.mPerStageStorageBuffers += 2;
            usage.mPerStageStorageImages += 5;
            usage.mPerStageResources += 3 + 5 + 2;
            usage.mLayoutSamplers += 1;
            usage.mLayoutSampledImages += 3;
            usage.mLayoutStorageBuffers += 2;
            usage.mLayoutStorageImages += 5;
            return usage;
        }
    }

    DescriptorUsage SharedRasterDescriptorUsage()
    {
        return RasterUsage();
    }

    DescriptorUsage SharedRtDescriptorUsage()
    {
        return RtUsage();
    }

    DescriptorUsage SharedRtDdgiDescriptorUsage()
    {
        return DdgiTraceUsage();
    }

    bool DescriptorLayoutFits(const DescriptorLimits &limits, const DescriptorUsage &usage)
    {
        return limits.maxBoundDescriptorSets >= usage.mBoundSets &&
            limits.maxPerStageSamplers >= usage.mPerStageSamplers &&
            limits.maxPerStageSampledImages >= usage.mPerStageSampledImages &&
            limits.maxPerStageUniformBuffers >= usage.mPerStageUniformBuffers &&
            limits.maxPerStageStorageBuffers >= usage.mPerStageStorageBuffers &&
            limits.maxPerStageStorageImages >= usage.mPerStageStorageImages &&
            limits.maxPerStageResources >= usage.mPerStageResources &&
            limits.maxSetSamplers >= usage.mLayoutSamplers &&
            limits.maxSetSampledImages >= usage.mLayoutSampledImages &&
            limits.maxSetUniformBuffers >= usage.mLayoutUniformBuffers &&
            limits.maxSetDynamicUniformBuffers >= usage.mLayoutDynamicUniformBuffers &&
            limits.maxSetStorageBuffers >= usage.mLayoutStorageBuffers &&
            limits.maxSetDynamicStorageBuffers >= usage.mLayoutDynamicStorageBuffers &&
            limits.maxSetStorageImages >= usage.mLayoutStorageImages;
    }
}
