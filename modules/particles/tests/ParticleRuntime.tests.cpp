/**
 * @file ParticleRuntime.tests.cpp
 * @author sumin.park
 * @brief Handle generation, capacity, drain, pause and bounded clock behavior.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles/ParticleRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <limits>

namespace
{
    const mir::ParticleEffect kEffect{0.05f, 60, 256, mir::AssetId{1}};
}

TEST_CASE("Particle handles reject stale slots after destroy and clear", "[particles]")
{
    mir::ParticleRuntime runtime(1);
    const auto first = runtime.Start({2}, kEffect);
    REQUIRE_FALSE(first.IsNull());
    CHECK(runtime.Start({2}, kEffect).IsNull());
    CHECK(runtime.Stats().rejectedStarts == 1);
    REQUIRE(runtime.Destroy(first));
    CHECK_FALSE(runtime.Find(first));
    CHECK_FALSE(runtime.Stop(first));
    CHECK_FALSE(runtime.Pause(first, true));
    const auto second = runtime.Start({2}, kEffect);
    REQUIRE_FALSE(second.IsNull());
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    runtime.Clear();
    CHECK_FALSE(runtime.Find(second));
    CHECK(runtime.Stats().active == 0);
    CHECK(runtime.Capacity() == 1);
}

TEST_CASE("Particle stop drains without extending lifetime and pause freezes it", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, kEffect);
    REQUIRE(runtime.Stop(handle));
    REQUIRE(runtime.Pause(handle, true));
    runtime.Advance(0.1f);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->drainRemaining == kEffect.lifetime);
    REQUIRE(runtime.Pause(handle, false));
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.Stop(handle));
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.Find(handle));
    runtime.Advance(1.0f / 60);
    CHECK_FALSE(runtime.Find(handle));
}

TEST_CASE("Particle emission resumes a draining instance without replacing its identity", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, kEffect);
    runtime.Stop(handle);
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.ResumeEmission(handle));
    runtime.Advance(0.25f);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->phase == mir::ParticlePhase::Emitting);
    CHECK(runtime.Stats().active == 1);
}

TEST_CASE("Particle owner removal drains an unpaused copy and clock work is bounded", "[particles]")
{
    mir::ParticleRuntime runtime;
    const mir::Entity owner{7, 1};
    const auto handle = runtime.Start({2}, kEffect, {}, owner);
    runtime.Pause(handle, true);
    runtime.Detach(owner);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->owner == mir::kNullEntity);
    CHECK_FALSE(runtime.Find(handle)->paused);
    runtime.Advance(1000000.0f);
    CHECK(runtime.Stats().ticks == 4);
    CHECK(runtime.Stats().droppedSeconds > 999999.0);
    CHECK_FALSE(runtime.Find(handle));
    runtime.Advance(std::numeric_limits<float>::infinity());
    CHECK(runtime.Stats().ticks == 4);
    mir::ParticleRuntime zero(0);
    CHECK(zero.Start({2}, kEffect).IsNull());
    CHECK_THROWS(mir::ParticleRuntime(65));
}
