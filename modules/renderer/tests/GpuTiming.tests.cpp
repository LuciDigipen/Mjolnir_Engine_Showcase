/**
 * @file GpuTiming.tests.cpp
 * @author Jackey Cen
 * @brief Tests for the optional GPU timestamp conversion and wrap handling.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../src/renderer/vulkan/GpuTiming.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <string>

using namespace mir::vk;

TEST_CASE("GPU timing names the whole frame and each pass", "[renderer][timing]")
{
    CHECK(GpuTimingRegionName(GpuTimingRegion::Frame) == std::string("frame"));
    CHECK(GpuTimingRegionName(GpuTimingRegion::RasterScene) == std::string("raster_scene"));
    CHECK(GpuTimingRegionName(GpuTimingRegion::RtTraceRays) == std::string("rt_trace_rays"));
    CHECK(static_cast<int>(GpuTimingRegion::Count) == 3);
}

TEST_CASE("GPU timestamps convert with the device period", "[renderer][timing]")
{
    const std::optional<double> milliseconds = GpuTimestampMilliseconds(1000, 2500, 64, 1.0f);
    REQUIRE(milliseconds);
    CHECK(*milliseconds == 0.0015);
}

TEST_CASE("GPU timestamps wrap with the hardware counter", "[renderer][timing]")
{
    const std::optional<double> milliseconds =
        GpuTimestampMilliseconds(UINT64_MAX - 5, 5, 64, 1.0f);
    REQUIRE(milliseconds);
    CHECK(*milliseconds == 0.000011);
}

TEST_CASE("GPU timestamps wrap at the counter width", "[renderer][timing]")
{
    const std::optional<double> narrow = GpuTimestampMilliseconds(0xFFFFFFF0u, 0x00000010u, 32, 1.0f);
    REQUIRE(narrow);
    CHECK(*narrow == 0.000032);

    const uint64_t mask48 = (uint64_t{1} << 48) - 1;
    const std::optional<double> wide = GpuTimestampMilliseconds(mask48 - 7, 4, 48, 1.0f);
    REQUIRE(wide);
    CHECK(*wide == 0.000012);
}

TEST_CASE("GPU timestamps accept a zero interval", "[renderer][timing]")
{
    const std::optional<double> milliseconds = GpuTimestampMilliseconds(123, 123, 32, 1.0f);
    REQUIRE(milliseconds);
    CHECK(*milliseconds == 0.0);
}

TEST_CASE("GPU timestamps reject values outside the valid bits", "[renderer][timing]")
{
    CHECK_FALSE(GpuTimestampMilliseconds(1ull << 40, 1ull << 41, 32, 1.0f).has_value());
    CHECK_FALSE(GpuTimestampMilliseconds(1ull << 48, 1ull << 49, 48, 1.0f).has_value());
    CHECK_FALSE(GpuTimestampMilliseconds(0, 100, 0, 1.0f).has_value());
    CHECK_FALSE(GpuTimestampMilliseconds(0, 100, 64, 0.0f).has_value());
    CHECK_FALSE(GpuTimestampMilliseconds(100, 0, 64, 1.0f).has_value());
    CHECK_FALSE(GpuTimestampMilliseconds(0xFFFFFFF0u, 0x00000010u, 64, 1.0f).has_value());
}
