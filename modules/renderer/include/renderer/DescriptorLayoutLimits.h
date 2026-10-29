/**
 * @file DescriptorLayoutLimits.h
 * @author Jackey Cen
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
        uint32_t mLayoutSamplers = 0;
        uint32_t mLayoutSampledImages = 0;
        uint32_t mLayoutUniformBuffers = 0;
        uint32_t mLayoutDynamicUniformBuffers = 0;
        uint32_t mLayoutStorageBuffers = 0;
        uint32_t mLayoutDynamicStorageBuffers = 0;
        uint32_t mLayoutStorageImages = 0;
    };

    // The maxDescriptorSet limits and the layout totals below are pipeline-wide,
    // while the per-stage values are the worst single stage.
    DescriptorUsage SharedRasterDescriptorUsage();
    DescriptorUsage SharedRtDescriptorUsage();
    bool DescriptorLayoutFits(const DescriptorLimits &limits, const DescriptorUsage &usage);
}
