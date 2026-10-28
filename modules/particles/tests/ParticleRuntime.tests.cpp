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

#include <cmath>
#include <limits>

namespace
{
    mir::ParticleEffect Definition(float lifetime = 0.05f, float rate = 60.0f, uint32_t count = 1)
    {
        mir::ParticleEffect system;
        for (uint32_t i = 0; i < count; ++i)
        {
            mir::ParticleEmitterAsset emitter;
            emitter.lifetime = lifetime;
            emitter.spawnRate = rate;
            emitter.texture = mir::AssetId{1};
            system.emitters.push_back(mir::AssetId{100 + i});
            system.emitterDefinitions.push_back(emitter);
        }
        return system;
    }
}

TEST_CASE("Particle system and emitter handles reject stale slots after destroy and clear", "[particles]")
{
    mir::ParticleRuntime runtime(1);
    const auto definition = Definition();
    const auto first = runtime.Start({2}, definition);
    REQUIRE_FALSE(first.IsNull());
    const auto emitter = runtime.Find(first)->emitters[0];
    REQUIRE(runtime.FindEmitter(emitter));
    CHECK(runtime.Start({2}, definition).IsNull());
    CHECK(runtime.Stats().rejectedStarts == 1);
    REQUIRE(runtime.Destroy(first));
    CHECK_FALSE(runtime.Find(first));
    CHECK_FALSE(runtime.FindEmitter(emitter));
    CHECK_FALSE(runtime.Stop(first));
    CHECK_FALSE(runtime.Pause(first, true));
    const auto second = runtime.Start({2}, definition);
    REQUIRE_FALSE(second.IsNull());
    CHECK(second.index == first.index);
    CHECK(second.generation != first.generation);
    const auto replacement = runtime.Find(second)->emitters[0];
    CHECK(replacement.index == emitter.index);
    CHECK(replacement.generation != emitter.generation);
    runtime.Clear();
    CHECK_FALSE(runtime.Find(second));
    CHECK_FALSE(runtime.FindEmitter(replacement));
    CHECK(runtime.Stats().active == 0);
    CHECK(runtime.Stats().activeEmitters == 0);
    CHECK(runtime.Capacity() == 1);
}

TEST_CASE("Particle stop drains without extending lifetime and pause freezes it", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto definition = Definition();
    const auto handle = runtime.Start({2}, definition);
    REQUIRE(runtime.Stop(handle));
    REQUIRE(runtime.Pause(handle, true));
    runtime.Advance(0.1f);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->drainRemaining == definition.emitterDefinitions[0].lifetime);
    REQUIRE(runtime.Pause(handle, false));
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.Stop(handle));
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.Find(handle));
    runtime.Advance(1.0f / 60);
    CHECK_FALSE(runtime.Find(handle));
    CHECK(runtime.Stats().activeEmitters == 0);
}

TEST_CASE("Particle emission resumes a draining system without replacing its identity", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, Definition());
    const auto emitter = runtime.Find(handle)->emitters[0];
    runtime.Stop(handle);
    runtime.Advance(1.0f / 60);
    REQUIRE(runtime.ResumeEmission(handle));
    runtime.Advance(0.25f);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->state == mir::ParticleExecutionState::Active);
    CHECK(runtime.FindEmitter(emitter)->state == mir::ParticleExecutionState::Active);
    CHECK(runtime.FindEmitter(emitter)->spawns == 4);
    CHECK(runtime.Stats().active == 1);
}

TEST_CASE("Particle owner removal drains an unpaused copy and clock work is bounded", "[particles]")
{
    mir::ParticleRuntime runtime;
    const mir::Entity owner{7, 1};
    const auto handle = runtime.Start({2}, Definition(), {}, owner);
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
    CHECK(zero.Start({2}, Definition()).IsNull());
    CHECK_THROWS(mir::ParticleRuntime(65));
}

TEST_CASE("Two-emitter systems have independent schedules and drain lifetimes", "[particles]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(0.05f, 60, 2);
    definition.emitterDefinitions[1].lifetime = 0.2f;
    definition.emitterDefinitions[1].spawnRate = 30;
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto first = runtime.Find(handle)->emitters[0];
    const auto second = runtime.Find(handle)->emitters[1];
    CHECK(first != second);
    CHECK(runtime.Stats().active == 1);
    CHECK(runtime.Stats().activeEmitters == 2);
    CHECK(runtime.FindEmitter(first)->system == handle);
    CHECK(runtime.FindEmitter(second)->emitterId == definition.emitters[1]);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(first)->spawns == 1);
    CHECK(runtime.FindEmitter(second)->spawns == 0);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(first)->spawns == 1);
    CHECK(runtime.FindEmitter(second)->spawns == 1);
    REQUIRE(runtime.Stop(handle));
    for (int i = 0; i < 3; ++i) runtime.Advance(1.0f / 60);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.FindEmitter(first)->state == mir::ParticleExecutionState::Complete);
    CHECK(runtime.FindEmitter(second)->state == mir::ParticleExecutionState::Inactive);
    CHECK(runtime.FindEmitter(first)->seconds == 0);
    CHECK(runtime.FindEmitter(second)->spawns == 0);
    REQUIRE(runtime.ResumeEmission(handle));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(first)->state == mir::ParticleExecutionState::Active);
    CHECK(runtime.FindEmitter(first)->spawns == 1);
    REQUIRE(runtime.Stop(handle));
    for (int i = 0; i < 13; ++i) runtime.Advance(1.0f / 60);
    CHECK_FALSE(runtime.Find(handle));
    CHECK_FALSE(runtime.FindEmitter(first));
    CHECK_FALSE(runtime.FindEmitter(second));
}

TEST_CASE("Emitter pool exhaustion rejects whole systems without partial allocation", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto eight = Definition(1, 1, mir::kMaxSystemEmitters);
    mir::ParticleHandle first;
    for (uint32_t i = 0; i < 8; ++i)
    {
        const auto handle = runtime.Start({2}, eight);
        REQUIRE_FALSE(handle.IsNull());
        if (i == 0) first = handle;
    }
    CHECK(runtime.Stats().active == 8);
    CHECK(runtime.Stats().activeEmitters == mir::kMaxParticleEmitterInstances);
    CHECK_FALSE(runtime.HasCapacity());
    CHECK(runtime.Start({2}, Definition(1, 1, 2)).IsNull());
    CHECK(runtime.Stats().active == 8);
    CHECK(runtime.Stats().activeEmitters == mir::kMaxParticleEmitterInstances);
    const auto oldEmitter = runtime.Find(first)->emitters[0];
    REQUIRE(runtime.Destroy(first));
    CHECK_FALSE(runtime.FindEmitter(oldEmitter));
    REQUIRE_FALSE(runtime.Start({2}, Definition(1, 1, 2)).IsNull());
    CHECK(runtime.Stats().active == 8);
    CHECK(runtime.Stats().activeEmitters == 58);
}

TEST_CASE("Emitter fractions and user overrides belong to CPU instances", "[particles]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(1, 60);
    definition.parameters.spawnRateScale = 0.5f;
    const auto handle = runtime.Start({2}, definition);
    const auto emitter = runtime.Find(handle)->emitters[0];
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 1);
    REQUIRE(runtime.SetParameters(handle, {2, 3}));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 2);
    CHECK(runtime.Find(handle)->parameters.sizeScale == 3);
    REQUIRE(runtime.SetParameters(handle, {0, 1}));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    REQUIRE(runtime.Pause(handle, true));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->seconds == 0);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    CHECK_FALSE(runtime.SetParameters(handle, {-1, 1}));
    CHECK(runtime.Find(handle)->parameters.spawnRateScale == 0);
}

TEST_CASE("Per-frame emitter work clears on zero and invalid ticks", "[particles]")
{
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, Definition(1));
    const auto emitter = runtime.Find(handle)->emitters[0];
    runtime.Advance(1.0f / 120);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    runtime.Advance(1.0f / 120);
    CHECK(runtime.FindEmitter(emitter)->spawns == 1);
    runtime.Advance(0);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    CHECK(runtime.FindEmitter(emitter)->seconds == 0);
    runtime.Advance(std::numeric_limits<float>::quiet_NaN());
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    CHECK(runtime.Stats().ticks == 1);
}

TEST_CASE("CPU scheduling enforces the global spawn budget without queuing dropped births", "[particles]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(1, 65536, 2);
    definition.parameters.spawnRateScale = 16;
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto first = runtime.Find(handle)->emitters[0];
    const auto second = runtime.Find(handle)->emitters[1];
    runtime.Advance(4.0f / 60);
    CHECK(runtime.FindEmitter(first)->spawns == mir::kParticleSpawnBudget);
    CHECK(runtime.FindEmitter(second)->spawns == 0);
    const auto requested = uint64_t(std::floor(4.0 / 60 * 65536 * 16)) * 2;
    CHECK(runtime.Stats().droppedBudgetSpawns == requested - mir::kParticleSpawnBudget);
    REQUIRE(runtime.SetParameters(handle, {0, 1}));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(first)->spawns == 0);
    CHECK(runtime.FindEmitter(second)->spawns == 0);
}

TEST_CASE("InactiveClear and Complete release all system and emitter slots immediately", "[particles]")
{
    mir::ParticleRuntime runtime;
    auto first = runtime.Start({2}, Definition(1, 1, 2));
    REQUIRE(runtime.SetExecutionState(first, mir::ParticleExecutionState::InactiveClear));
    CHECK_FALSE(runtime.Find(first));
    CHECK(runtime.Stats().activeEmitters == 0);
    const auto second = runtime.Start({2}, Definition(1, 1, 2));
    REQUIRE(runtime.SetExecutionState(second, mir::ParticleExecutionState::Complete));
    CHECK_FALSE(runtime.Find(second));
    CHECK(runtime.Stats().active == 0);
    CHECK(runtime.Stats().activeEmitters == 0);
}

TEST_CASE("Invalid unresolved systems and user parameters never consume instance slots", "[particles]")
{
    mir::ParticleRuntime runtime(1);
    auto definition = Definition();
    definition.emitterDefinitions.clear();
    CHECK(runtime.Start({2}, definition).IsNull());
    definition = Definition(1, 1, 9);
    CHECK(runtime.Start({2}, definition).IsNull());
    definition = Definition();
    const mir::ParticleUserParameters invalid{std::numeric_limits<float>::infinity(), 1};
    CHECK(runtime.Start({2}, definition, {}, mir::kNullEntity, &invalid).IsNull());
    CHECK(runtime.Stats().active == 0);
    CHECK(runtime.Stats().activeEmitters == 0);
    REQUIRE_FALSE(runtime.Start({2}, Definition()).IsNull());
}
