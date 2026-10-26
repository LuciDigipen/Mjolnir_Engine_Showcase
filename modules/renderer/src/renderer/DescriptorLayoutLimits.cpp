/**
 * @file DescriptorLayoutLimits.cpp
 * @author Sumin Park
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
        // Raster: set 0 has two combined images and the record UBO; set 1 a camera
        // UBO and light storage buffer; set 2 a model storage buffer; set 3 three
        // combined shadow images and one storage buffer; set 4 the shared table.
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
                    fragmentStorageBuffers,
                .mSetSamplers = vk::kMaxSharedSamplers,
                .mSetSampledImages = vk::kMaxSharedImages,
                .mSetUniformBuffers = 1,
                .mSetDynamicUniformBuffers = 1,
                .mSetStorageBuffers = 1,
                .mSetDynamicStorageBuffers = 0,
                .mSetStorageImages = 0 };
        }

        // RT: set 0 has the TLAS, two storage buffers (instance info and records),
        // the table rows and its storage images; set 1 the camera UBO and lights.
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
                    vk::kCameraSetStorageBuffers + vk::kCameraSetUniformBuffers + 3 +
                    vk::kRtSetAccelerationStructures,
                .mSetSamplers = vk::kMaxSharedSamplers,
                .mSetSampledImages = vk::kMaxSharedImages,
                .mSetUniformBuffers = vk::kCameraSetUniformBuffers,
                .mSetDynamicUniformBuffers = vk::kCameraSetUniformBuffers,
                .mSetStorageBuffers = vk::kRtSetStorageBuffers,
                .mSetDynamicStorageBuffers = 0,
                .mSetStorageImages = vk::kRtSetStorageImages };
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

    bool DescriptorLayoutFits(const DescriptorLimits &limits, const DescriptorUsage &usage)
    {
        return limits.maxBoundDescriptorSets >= usage.mBoundSets &&
            limits.maxPerStageSamplers >= usage.mPerStageSamplers &&
            limits.maxPerStageSampledImages >= usage.mPerStageSampledImages &&
            limits.maxPerStageUniformBuffers >= usage.mPerStageUniformBuffers &&
            limits.maxPerStageStorageBuffers >= usage.mPerStageStorageBuffers &&
            limits.maxPerStageStorageImages >= usage.mPerStageStorageImages &&
            limits.maxPerStageResources >= usage.mPerStageResources &&
            limits.maxSetSamplers >= usage.mSetSamplers &&
            limits.maxSetSampledImages >= usage.mSetSampledImages &&
            limits.maxSetUniformBuffers >= usage.mSetUniformBuffers &&
            limits.maxSetDynamicUniformBuffers >= usage.mSetDynamicUniformBuffers &&
            limits.maxSetStorageBuffers >= usage.mSetStorageBuffers &&
            limits.maxSetDynamicStorageBuffers >= usage.mSetDynamicStorageBuffers &&
            limits.maxSetStorageImages >= usage.mSetStorageImages;
    }
}
