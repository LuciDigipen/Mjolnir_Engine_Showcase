/**
 * @file ParticleRuntime.tests.cpp
 * @author sumin.park
 * @brief Handle generation, capacity, drain, pause and bounded clock behavior.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles/ParticleRuntime.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

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

TEST_CASE("Detached particle instances retain their originating scene", "[particles][scene]")
{
    mir::ParticleRuntime runtime(1);
    const auto scene = mir::AllocateSceneInstanceId();
    const mir::Entity owner{ 7, 1 };
    const auto handle = runtime.Start({ 2 }, Definition(), {}, owner, nullptr, false, scene);
    REQUIRE(runtime.Find(handle));
    runtime.Detach(owner);
    REQUIRE(runtime.Find(handle));
    CHECK(runtime.Find(handle)->owner.IsNull());
    CHECK(runtime.Find(handle)->scene == scene);
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

TEST_CASE("Moving particle schedules agree at 30 60 90 120 and 144 Hz", "[particles][motion]")
{
    for (const uint32_t hz : {30u, 60u, 90u, 120u, 144u})
    {
        CAPTURE(hz);
        mir::ParticleRuntime runtime;
        auto definition = Definition(4, 37);
        definition.emitterDefinitions[0].spawnPerMeter = 2.5f;
        definition.emitterDefinitions[0].burst = 7;
        const auto handle = runtime.Start({2}, definition);
        REQUIRE_FALSE(handle.IsNull());
        const auto emitterHandle = runtime.Find(handle)->emitters[0];
        uint32_t count = 0;
        double lastX = 0;
        for (uint32_t frame = 0; frame < hz * 2; ++frame)
        {
            mir::WorldPose pose;
            pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(10.0 * (frame + 1) / hz, 0, 0));
            REQUIRE(runtime.SetPose(handle, pose));
            runtime.Advance(1.0f / hz);
            const auto &emitter = *runtime.FindEmitter(emitterHandle);
            REQUIRE(emitter.births.size() == emitter.spawns);
            for (const auto &birth : emitter.births)
            {
                CHECK(birth.sequence == count++);
                CHECK(birth.fraction >= 0);
                CHECK(birth.fraction <= 1);
                CHECK(birth.age >= 0);
                CHECK(birth.age <= emitter.seconds);
                const auto position = emitter.spawnStart.Meters() +
                    (emitter.attachmentPosition.Meters() - emitter.spawnStart.Meters()) * mir::WorldReal(birth.fraction);
                CHECK(double(position.x) + 1e-5 >= lastX);
                lastX = double(position.x);
            }
        }
        // Two seconds at 37/s, twenty metres at 2.5/m, and one burst of seven.
        CHECK(count == 131);
        CHECK(runtime.FindEmitter(emitterHandle)->birthSequence == 131);
        CHECK(runtime.Stats().droppedBudgetSpawns == 0);
        REQUIRE(runtime.Stop(handle));
        REQUIRE(runtime.ResumeEmission(handle));
        REQUIRE(runtime.SetParameters(handle, {0, 1}));
        runtime.Advance(1.0f / 60);
        CHECK(runtime.FindEmitter(emitterHandle)->spawns == 0);
    }
}

TEST_CASE("Swept particle emission remains local within one centimetre at one thousand kilometres", "[particles][motion]")
{
    if constexpr (!mir::kLargeWorld)
        SKIP("Float world mode explicitly rejects the translated fixture");
    mir::ParticleRuntime nearRuntime, farRuntime;
    auto definition = Definition(4, 83);
    definition.emitterDefinitions[0].spawnPerMeter = 3;
    definition.emitterDefinitions[0].localOffset = {0.125f, -0.25f, 0.5f};
    mir::WorldPose translated;
    const mir::WorldVec3 origin(1000000, 1000000, -1000000);
    translated.position = *mir::WorldPosition::FromMeters(origin);
    const auto nearHandle = nearRuntime.Start({2}, definition);
    const auto farHandle = farRuntime.Start({2}, definition, translated);
    REQUIRE_FALSE(nearHandle.IsNull());
    REQUIRE_FALSE(farHandle.IsNull());
    const auto nearEmitter = nearRuntime.Find(nearHandle)->emitters[0];
    const auto farEmitter = farRuntime.Find(farHandle)->emitters[0];
    uint32_t checked = 0;
    for (uint32_t frame = 0; frame < 240; ++frame)
    {
        mir::WorldPose pose;
        const mir::WorldVec3 path(0.137 * (frame + 1), 0.019 * (frame + 1), 0);
        pose.position = *mir::WorldPosition::FromMeters(path);
        translated.position = *mir::WorldPosition::FromMeters(origin + path);
        REQUIRE(nearRuntime.SetPose(nearHandle, pose));
        REQUIRE(farRuntime.SetPose(farHandle, translated));
        nearRuntime.Advance(1.0f / 120);
        farRuntime.Advance(1.0f / 120);
        const auto &near = *nearRuntime.FindEmitter(nearEmitter);
        const auto &far = *farRuntime.FindEmitter(farEmitter);
        REQUIRE(near.births.size() == far.births.size());
        for (size_t i = 0; i < near.births.size(); ++i)
        {
            const auto nearPosition = near.spawnStart.Meters() +
                (near.attachmentPosition.Meters() - near.spawnStart.Meters()) * mir::WorldReal(near.births[i].fraction);
            const auto farPosition = far.spawnStart.Meters() - origin +
                (far.attachmentPosition.Meters() - far.spawnStart.Meters()) * mir::WorldReal(far.births[i].fraction);
            CHECK(glm::length(nearPosition - farPosition) <= 0.01);
            CHECK(near.births[i].sequence == far.births[i].sequence);
            ++checked;
        }
    }
    CHECK(checked > 250);
}

TEST_CASE("Rotating particle attachments inherit point motion and rotate local velocity", "[particles][motion]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(1, 60);
    definition.emitterDefinitions[0].localOffset = {2, 0, 0};
    definition.emitterDefinitions[0].velocity = {1, 0, 0};
    definition.emitterDefinitions[0].inheritVelocity = 1;
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto emitterHandle = runtime.Find(handle)->emitters[0];
    mir::WorldPose pose;
    pose.linear = glm::mat3(glm::vec3(0, 1, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1));
    REQUIRE(runtime.SetPose(handle, pose));
    runtime.Advance(1.0f / 60);
    const auto &emitter = *runtime.FindEmitter(emitterHandle);
    CHECK(glm::length(emitter.spawnStart.Meters() - mir::WorldVec3(2, 0, 0)) < 1e-5);
    CHECK(glm::length(emitter.attachmentPosition.Meters() - mir::WorldVec3(0, 2, 0)) < 1e-5);
    CHECK(glm::length(emitter.attachmentVelocity - glm::vec3(-120, 120, 0)) < 0.001f);
    CHECK(glm::length(emitter.emissionVelocity - glm::vec3(0, 1, 0)) < 1e-5f);
    CHECK_FALSE(emitter.teleported);
}

TEST_CASE("Particle hitches cap swept work and teleports do not emit connecting distance particles", "[particles][motion]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(4, 60);
    definition.emitterDefinitions[0].spawnPerMeter = 10;
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto emitterHandle = runtime.Find(handle)->emitters[0];
    mir::WorldPose pose;
    pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(10, 0, 0));
    REQUIRE(runtime.SetPose(handle, pose));
    runtime.Advance(1);
    const auto &hitch = *runtime.FindEmitter(emitterHandle);
    CHECK(hitch.spawns == 10);
    CHECK(hitch.seconds <= 4.0f / 60);
    CHECK(std::abs(hitch.spawnStart.Meters().x - mir::WorldReal(10 - 2.0 / 3)) < 1e-5);
    CHECK(std::abs(hitch.attachmentVelocity.x - 10) < 1e-5);
    CHECK(runtime.Stats().droppedSeconds > 0.9);
    runtime.Advance(0);
    CHECK(runtime.FindEmitter(emitterHandle)->births.empty());
    pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(1000, 0, 0));
    REQUIRE(runtime.SetPose(handle, pose));
    runtime.Advance(1.0f / 60);
    const auto &teleport = *runtime.FindEmitter(emitterHandle);
    CHECK(teleport.teleported);
    CHECK(teleport.spawns == 1);
    CHECK(glm::length(teleport.spawnStart.Meters() - teleport.attachmentPosition.Meters()) == 0);
    CHECK(glm::length(teleport.attachmentVelocity) == 0);
    CHECK(teleport.distanceRemainder == 0);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitterHandle)->spawns == 1);
    CHECK_FALSE(runtime.FindEmitter(emitterHandle)->teleported);
}

TEST_CASE("Particle system tile shifts are shared by emitter instances at the radius boundary", "[particles][motion]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(4, 60, 2);
    definition.emitterDefinitions[0].localOffset = {-3, 0, 0};
    definition.emitterDefinitions[1].localOffset = {3, 0, 0};
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto first = runtime.Find(handle)->emitters[0];
    const auto second = runtime.Find(handle)->emitters[1];
    mir::WorldPose pose;
    pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(mir::kParticleTileRadius, 0, 0));
    REQUIRE(runtime.SetPose(handle, pose));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.Find(handle)->tileOrigin.Meters().x == 0);
    pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(mir::kParticleTileRadius + 1, 0, 0));
    REQUIRE(runtime.SetPose(handle, pose));
    runtime.Advance(1.0f / 60);
    CHECK(runtime.Find(handle)->tileOrigin.Meters().x == mir::kParticleTileRadius + 1);
    CHECK(runtime.FindEmitter(first)->system == runtime.FindEmitter(second)->system);
    const auto origin = runtime.Find(handle)->tileOrigin.Meters();
    CHECK(runtime.FindEmitter(first)->attachmentPosition.Meters().x - origin.x == -3);
    CHECK(runtime.FindEmitter(second)->attachmentPosition.Meters().x - origin.x == 3);
    CHECK(runtime.Stats().activeEmitters == 2);
    CHECK(runtime.FindEmitter(first)->birthSequence == 2);
    CHECK(runtime.FindEmitter(second)->birthSequence == 2);
}

TEST_CASE("Float particle worlds explicitly reject positions outside their precision budget", "[particles][motion]")
{
    mir::ParticleRuntime runtime;
    const auto definition = Definition(1, 60);
    mir::WorldPose remote;
    remote.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(1000000, 0, 0));
    if constexpr (mir::kLargeWorld)
    {
        CHECK_FALSE(runtime.Start({2}, definition, remote).IsNull());
    }
    else
    {
        CHECK(runtime.Start({2}, definition, remote).IsNull());
        CHECK(runtime.Stats().rejectedStarts == 1);
        const auto handle = runtime.Start({2}, definition);
        REQUIRE_FALSE(handle.IsNull());
        CHECK_FALSE(runtime.SetPose(handle, remote));
        CHECK(runtime.Find(handle)->pose.position.Meters().x == 0);
        auto offsetDefinition = definition;
        offsetDefinition.emitterDefinitions[0].localOffset = {2, 0, 0};
        mir::WorldPose edge;
        edge.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(mir::kParticleFloatWorldLimitMeters - 1, 0, 0));
        CHECK(runtime.Start({2}, offsetDefinition, edge).IsNull());
    }
}

TEST_CASE("Resuming particle emission never sweeps stopped or paused attachment motion", "[particles][motion]")
{
    mir::ParticleRuntime runtime;
    auto definition = Definition(4, 0);
    definition.emitterDefinitions[0].spawnPerMeter = 10;
    const auto handle = runtime.Start({2}, definition);
    REQUIRE_FALSE(handle.IsNull());
    const auto emitter = runtime.Find(handle)->emitters[0];
    auto move = [&](float x)
    {
        mir::WorldPose pose;
        pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(x, 0, 0));
        REQUIRE(runtime.SetPose(handle, pose));
    };
    move(1);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 10);
    REQUIRE(runtime.Stop(handle));
    runtime.Advance(0.05f);
    REQUIRE(runtime.ResumeEmission(handle));
    move(5);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    CHECK(glm::length(runtime.FindEmitter(emitter)->attachmentVelocity) == 0);
    move(6);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 10);
    CHECK(std::abs(runtime.FindEmitter(emitter)->attachmentVelocity.x - 60) < 0.001f);

    // A pose can move while paused even when the simulation receives no intervening tick.
    REQUIRE(runtime.Pause(handle, true));
    move(8);
    REQUIRE(runtime.Pause(handle, false));
    move(9);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 0);
    CHECK(glm::length(runtime.FindEmitter(emitter)->attachmentVelocity) == 0);
    move(10);
    runtime.Advance(1.0f / 60);
    CHECK(runtime.FindEmitter(emitter)->spawns == 10);
}

TEST_CASE("Start delay holds back births, the burst and the sweep until it ends", "[particles][delay]")
{
    mir::ParticleEmitterAsset emitter;
    emitter.texture = {1};
    emitter.startDelay = 0.5f;
    emitter.spawnRate = 60;
    emitter.burst = 5;
    emitter.capacity = 256;
    mir::ParticleEffect effect;
    effect.emitters = {{1}};
    effect.emitterDefinitions = {emitter};
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, effect);
    REQUIRE_FALSE(handle.IsNull());
    const auto *state = runtime.FindEmitter(runtime.Find(handle)->emitters[0]);
    uint32_t born = 0;
    int firstBirthTick = -1;
    for (int tick = 1; tick <= 60; ++tick)
    {
        mir::WorldPose pose;
        pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(tick, 0, 0));
        REQUIRE(runtime.SetPose(handle, pose));
        runtime.Advance(1.0f / 60);
        if (state->spawns && firstBirthTick < 0) firstBirthTick = tick;
        if (firstBirthTick < 0) CHECK(state->births.empty());
        born += state->spawns;
        // The delayed emitter never sweeps the motion made while it waited.
        if (tick == firstBirthTick) CHECK(state->attachmentVelocity.x == Catch::Approx(60.0f).margin(0.5f));
    }
    CHECK(firstBirthTick >= 30);
    CHECK(firstBirthTick <= 32);
    CHECK(born >= 5 + 25);
}

TEST_CASE("A follow emitter reports its movement while a plain one and a teleport do not", "[particles][follow]")
{
    mir::ParticleEmitterAsset follow;
    follow.texture = {1};
    follow.spawnRate = 0;
    follow.followEmitter = true;
    auto plain = follow;
    plain.followEmitter = false;
    mir::ParticleEffect effect;
    effect.emitters = {{1}, {1}};
    effect.emitterDefinitions = {follow, plain};
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, effect);
    REQUIRE_FALSE(handle.IsNull());
    const auto *moving = runtime.FindEmitter(runtime.Find(handle)->emitters[0]);
    const auto *still = runtime.FindEmitter(runtime.Find(handle)->emitters[1]);
    double x = 0;
    const auto move = [&](double distance, float dt)
    {
        x += distance;
        mir::WorldPose pose;
        pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(x, 0, 0));
        REQUIRE(runtime.SetPose(handle, pose));
        runtime.Advance(dt);
    };
    for (int tick = 0; tick < 3; ++tick)
    {
        move(0.5, 1.0f / 60);
        CHECK(moving->followDelta.x == Catch::Approx(0.5f));
        CHECK(still->followDelta == glm::vec3(0));
    }
    // A frame shorter than one step simulates nothing, so the movement carries into the next step.
    move(0.5, 0.004f);
    CHECK(moving->followDelta == glm::vec3(0));
    runtime.Advance(1.0f / 60);
    CHECK(moving->followDelta.x == Catch::Approx(0.5f));
    move(1000, 1.0f / 60);
    CHECK(moving->followDelta == glm::vec3(0));
}

TEST_CASE("Live edits move placements and copy data fields but leave recook fields alone", "[particles][placement]")
{
    mir::ParticleEmitterAsset gpu;
    gpu.texture = {1};
    mir::ParticleEmitterAsset ribbon;
    ribbon.texture = {1};
    ribbon.simulationTarget = mir::ParticleSimulationTarget::CPU;
    ribbon.spawnRate = 0;
    ribbon.spawnPerMeter = 2;
    ribbon.spawnBehaviors = {{"RibbonSpawn", {0.25f}}};
    ribbon.updateBehaviors = {{"RibbonAge", {}}, {"RibbonFade", {}}};
    mir::ParticleEffect effect;
    effect.emitters = {{1}, {2}};
    effect.placementIds = {1, 2};
    effect.placementOffsets = {{1, 0, 0}, {0, 0, 0}};
    effect.emitterDefinitions = {gpu, ribbon};
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, effect);
    REQUIRE_FALSE(handle.IsNull());
    const auto *first = runtime.FindEmitter(runtime.Find(handle)->emitters[0]);
    const auto *second = runtime.FindEmitter(runtime.Find(handle)->emitters[1]);
    CHECK(first->attachmentPosition.Meters().x == 1);

    SECTION("Placement and instance offsets add together")
    {
        const std::vector<glm::vec3> instance{{0, 0, 2}, {0, 0, 0}};
        REQUIRE(runtime.SetEmitterOffsets(handle, instance));
        const std::vector<glm::vec3> placement{{3, 0, 0}, {0, 5, 0}};
        REQUIRE(runtime.SetPlacementOffsets(handle, placement));
        CHECK(first->attachmentPosition.Meters() == mir::WorldVec3(3, 0, 2));
        CHECK(second->attachmentPosition.Meters() == mir::WorldVec3(0, 5, 0));
        CHECK(first->previousPosition.Meters() == first->attachmentPosition.Meters());
        CHECK_FALSE(runtime.SetPlacementOffsets(handle, std::vector<glm::vec3>{{0, 4097, 0}}));
        CHECK(first->placementOffset == glm::vec3(3, 0, 0));
    }
    SECTION("A GPU draft copies its data fields but not capacity or behaviors")
    {
        auto draft = gpu;
        draft.lifetime = 5;
        draft.size = 0.5f;
        draft.localOffset = {0, 2, 0};
        draft.renderer.stretch = 0.2f;
        draft.capacity = 999;
        draft.updateBehaviors = {{"Age", {}}, {"Integrate", {}}, {"Drag", {1}}};
        REQUIRE(runtime.SetEmitterData(handle, 0, draft));
        CHECK(first->definition.lifetime == 5);
        CHECK(first->definition.size == 0.5f);
        CHECK(first->definition.renderer.stretch == 0.2f);
        CHECK(first->attachmentPosition.Meters() == mir::WorldVec3(1, 2, 0));
        CHECK(first->definition.capacity == gpu.capacity);
        CHECK(first->definition.updateBehaviors.size() == gpu.updateBehaviors.size());
    }
    SECTION("A ribbon draft replaces its behaviors and has them resolved again")
    {
        auto draft = ribbon;
        draft.updateBehaviors = {{"RibbonAge", {}}};
        runtime.Advance(1.0f / 60);
        REQUIRE(runtime.SetEmitterData(handle, 1, draft));
        CHECK(second->definition.updateBehaviors.size() == 1);
        CHECK(second->ribbon.resolvers.empty());
    }
    SECTION("Wrong targets, invalid drafts and bad indices are refused")
    {
        CHECK_FALSE(runtime.SetEmitterData(handle, 0, ribbon));
        auto invalid = gpu;
        invalid.lifetime = -1;
        CHECK_FALSE(runtime.SetEmitterData(handle, 0, invalid));
        CHECK_FALSE(runtime.SetEmitterData(handle, 5, gpu));
        CHECK(first->definition.lifetime == gpu.lifetime);
    }
}

TEST_CASE("A start delay set on a playing emitter holds back its births", "[particles][delay]")
{
    mir::ParticleEmitterAsset emitter;
    emitter.texture = {1};
    emitter.spawnRate = 60;
    emitter.capacity = 256;
    mir::ParticleEffect effect;
    effect.emitters = {{1}};
    effect.emitterDefinitions = {emitter};
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, effect);
    REQUIRE_FALSE(handle.IsNull());
    const auto *state = runtime.FindEmitter(runtime.Find(handle)->emitters[0]);
    auto draft = emitter;
    draft.startDelay = 1.0f;
    REQUIRE(runtime.SetEmitterData(handle, 0, draft));
    int firstBirthTick = -1;
    for (int tick = 1; tick <= 90 && firstBirthTick < 0; ++tick)
    {
        runtime.Advance(1.0f / 60);
        if (state->spawns) firstBirthTick = tick;
    }
    CHECK(firstBirthTick >= 60);
    CHECK(firstBirthTick <= 62);
}
