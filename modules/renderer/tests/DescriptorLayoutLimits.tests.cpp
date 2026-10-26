/**
 * @file DescriptorLayoutLimits.tests.cpp
 * @author Sumin Park
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
            .maxSetSamplers = usage.mSetSamplers,
            .maxSetSampledImages = usage.mSetSampledImages,
            .maxSetUniformBuffers = usage.mSetUniformBuffers,
            .maxSetDynamicUniformBuffers = usage.mSetDynamicUniformBuffers,
            .maxSetStorageBuffers = usage.mSetStorageBuffers,
            .maxSetDynamicStorageBuffers = usage.mSetDynamicStorageBuffers,
            .maxSetStorageImages = usage.mSetStorageImages };
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
    CHECK(usage.mPerStageResources == 1035);
    CHECK(usage.mSetSamplers == 256);
    CHECK(usage.mSetSampledImages == 1024);
    CHECK(usage.mSetStorageBuffers == 1);

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
    CHECK(usage.mPerStageResources == 1032);
    CHECK(usage.mSetStorageBuffers == 2);
    CHECK(usage.mSetStorageImages == 7);

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
    below.maxPerStageStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageStorageImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxPerStageResources -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetStorageBuffers -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
    below = exact;
    below.maxSetStorageImages -= 1;
    CHECK_FALSE(DescriptorLayoutFits(below, usage));
}
