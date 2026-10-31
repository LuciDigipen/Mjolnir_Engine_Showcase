/**
 * @file TerrainJobExecutor.tests.cpp
 * @author Rahul Nair
 * @brief Verifies retained streamers keep their executor after runtime teardown.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainStreamingTestSupport.h"

#include <terrain_runtime/TerrainJobExecutor.h>

#include <jobs/JobExecutor.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <memory>

using namespace mir;
using namespace mir::test;

TEST_CASE("A retained terrain streamer finishes reads and accepts new releases after its runtime releases the lane",
          "[terrain_runtime][jobs]")
{
    const size_t workers = GENERATE(size_t(0), size_t(1), size_t(2));
    JobExecutor jobs(workers);
    auto runtimeLane = std::make_shared<TerrainJobExecutor>(jobs, 2);
    std::weak_ptr<TerrainJobExecutor> lifetime = runtimeLane;
    auto streamer = std::make_unique<TerrainStreamer>(
        StreamingSettings(uint64_t(1) << 30, TerrainResidencyMode::Eager), runtimeLane);
    const AssetId asset = TerrainAsset("terrain/retained.terrain.json");
    const auto first = OpenControlled(EncodeEdge(asset, 1));
    streamer->Add(first.release);
    streamer->Update();

    // Same teardown contract as TerrainRuntime: finish submitted work, then
    // release ownership without closing a lane another consumer still owns.
    runtimeLane->WaitIdle();
    runtimeLane.reset();
    CHECK_FALSE(lifetime.expired());
    UpdateUntil(*streamer, [&] { return streamer->Stats().loading == 0 && streamer->Stats().waiting == 0; });
    CHECK(streamer->Stats().readsSucceeded > 0);
    CHECK(streamer->Stats().failed == 0);

    const auto second = OpenControlled(EncodeEdge(asset, 2, 1.0f));
    const auto previousReads = streamer->Stats().readsSucceeded;
    streamer->Add(second.release);
    UpdateUntil(*streamer, [&] { return streamer->Stats().loading == 0 && streamer->Stats().waiting == 0; });
    CHECK(streamer->Stats().readsSucceeded > previousReads);
    CHECK(streamer->Stats().failed == 0);
    streamer.reset();
    CHECK(lifetime.expired());
}
