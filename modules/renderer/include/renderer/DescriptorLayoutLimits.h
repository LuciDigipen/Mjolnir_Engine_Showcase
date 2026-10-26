/**
 * @file DescriptorLayoutLimits.h
 * @author Sumin Park
 * @brief Whether the shared material layouts fit a device's descriptor limits.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    struct DescriptorLimits
    {
        uint32_t maxBoundDescriptorSets = 0;
        uint32_t maxPerStageSamplers = 0;
        uint32_t maxPerStageSampledImages = 0;
        uint32_t maxPerStageUniformBuffers = 0;
        uint32_t maxPerStageStorageBuffers = 0;
        uint32_t maxPerStageStorageImages = 0;
        uint32_t maxPerStageResources = 0;
        uint32_t maxSetSamplers = 0;
        uint32_t maxSetSampledImages = 0;
        uint32_t maxSetUniformBuffers = 0;
        uint32_t maxSetDynamicUniformBuffers = 0;
        uint32_t maxSetStorageBuffers = 0;
        uint32_t maxSetDynamicStorageBuffers = 0;
        uint32_t maxSetStorageImages = 0;
    };

    struct DescriptorUsage
    {
        uint32_t mBoundSets = 0;
        uint32_t mPerStageSamplers = 0;
        uint32_t mPerStageSampledImages = 0;
        uint32_t mPerStageUniformBuffers = 0;
        uint32_t mPerStageStorageBuffers = 0;
        uint32_t mPerStageStorageImages = 0;
        uint32_t mPerStageResources = 0;
        uint32_t mSetSamplers = 0;
        uint32_t mSetSampledImages = 0;
        uint32_t mSetUniformBuffers = 0;
        uint32_t mSetDynamicUniformBuffers = 0;
        uint32_t mSetStorageBuffers = 0;
        uint32_t mSetDynamicStorageBuffers = 0;
        uint32_t mSetStorageImages = 0;
    };

    // Worst stage and worst set of the raster layout, legacy bindings included.
    DescriptorUsage SharedRasterDescriptorUsage();
    // Worst stage and worst set of the RT layout, its camera set included.
    DescriptorUsage SharedRtDescriptorUsage();
    bool DescriptorLayoutFits(const DescriptorLimits &limits, const DescriptorUsage &usage);
}
