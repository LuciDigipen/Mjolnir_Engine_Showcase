/**
 * @file DescriptorLayoutLimits.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/DescriptorLayoutLimits.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/DescriptorLayoutLimits.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

namespace
{
    DescriptorLimits LimitsFor(const DescriptorUsage &usage)
    {
        return DescriptorLimits{
            .maxBoundDescriptorSets = usage.mBoundSets,
            .maxPerStageSamplers = usage.mPerStageSamplers,
            .maxPerStageSampledImages = usage.mPerStageSampledImages,
            .maxPerStageUniformBuffers = usage.mPerStageUniformBuffers,
            .maxPerStageStorageBuffers = usage.mPerStageStorageBuffers,
            .maxPerStageStorageImages = usage.mPerStageStorageImages,
            .maxPerStageResources = usage.mPerStageResources,
            .maxSetSamplers = usage.mLayoutSamplers,
            .maxSetSampledImages = usage.mLayoutSampledImages,
            .maxSetUniformBuffers = usage.mLayoutUniformBuffers,
            .maxSetDynamicUniformBuffers = usage.mLayoutDynamicUniformBuffers,
            .maxSetStorageBuffers = usage.mLayoutStorageBuffers,
            .maxSetDynamicStorageBuffers = usage.mLayoutDynamicStorageBuffers,
            .maxSetStorageImages = usage.mLayoutStorageImages };
    }
}

TEST_CASE("The shared raster budget counts legacy and table descriptors", "[renderer][material]")
{
    const DescriptorUsage usage = SharedRasterDescriptorUsage();
    CHECK(usage.mBoundSets == 5);
    CHECK(usage.mPerStageSamplers == 261);
    CHECK(usage.mPerStageSampledImages == 1029);
    CHECK(usage.mPerStageUniformBuffers == 2);
    CHECK(usage.mPerStageStorageBuffers == 4);
    CHECK(usage.mPerStageResources == 1036);
    CHECK(usage.mLayoutSamplers == 261);
    CHECK(usage.mLayoutSampledImages == 1029);
    CHECK(usage.mLayoutUniformBuffers == 2);
    CHECK(usage.mLayoutDynamicUniformBuffers == 2);
    CHECK(usage.mLayoutStorageBuffers == 4);

    const DescriptorLimits exact = LimitsFor(usage);
    CHECK(DescriptorLayoutFits(exact, usage));

    DescriptorLimits below = exact;
    below.maxBoundDescriptorSets -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageSamplers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageSampledImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageResources -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetSamplers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetSampledImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetDynamicUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
}

TEST_CASE("The shared RT budget counts its storage images and buffers", "[renderer][material]")
{
    const DescriptorUsage usage = SharedRtDescriptorUsage();
    CHECK(usage.mBoundSets == 2);
    CHECK(usage.mPerStageSamplers == 256);
    CHECK(usage.mPerStageSampledImages == 1024);
    CHECK(usage.mPerStageStorageBuffers == 3);
    CHECK(usage.mPerStageStorageImages == 4);
    CHECK(usage.mPerStageResources == 1031);
    CHECK(usage.mLayoutSamplers == 256);
    CHECK(usage.mLayoutSampledImages == 1024);
    CHECK(usage.mLayoutUniformBuffers == 1);
    CHECK(usage.mLayoutDynamicUniformBuffers == 1);
    CHECK(usage.mLayoutStorageBuffers == 3);
    CHECK(usage.mLayoutStorageImages == 7);

    const DescriptorLimits exact = LimitsFor(usage);
    CHECK(DescriptorLayoutFits(exact, usage));

    DescriptorLimits below = exact;
    below.maxBoundDescriptorSets -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageSamplers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageSampledImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageStorageImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageResources -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetSamplers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetSampledImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetDynamicUniformBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetStorageImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
}
