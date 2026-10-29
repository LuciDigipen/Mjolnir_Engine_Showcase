/**
 * @file ParticleRibbon.tests.cpp
 * @author sumin.park
 * @brief Analytical ribbon motion, activation discontinuities and bounded retirement.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles/ParticleRuntime.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>

namespace
{
    mir::ParticleEffect Ribbon(float lifetime = 4, uint32_t capacity = 256)
    {
        mir::ParticleEmitterAsset emitter;
        emitter.simulationTarget = mir::ParticleSimulationTarget::CPU;
        emitter.spawnRate = 0;
        emitter.spawnPerMeter = 2;
        emitter.lifetime = lifetime;
        emitter.capacity = capacity;
        emitter.texture = {1};
        emitter.spawnBehaviors = {{"RibbonSpawn", {0.25f}}};
        emitter.updateBehaviors = {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidth", {3}}, {"RibbonFade", {}}};
        mir::ParticleEffect effect;
        effect.emitters = {{1}};
        effect.emitterDefinitions = {emitter};
        effect.parameters.wind = {0,2,0};
        return effect;
    }
    mir::WorldPose Pose(double x, double y = 0)
    {
        mir::WorldPose pose;
        pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(x,y,0));
        return pose;
    }
    const mir::ParticleEmitterInstance &Emitter(const mir::ParticleRuntime &runtime, mir::ParticleHandle handle)
    {
        return *runtime.FindEmitter(runtime.Find(handle)->emitters[0]);
    }
}
TEST_CASE("CPU ribbon straight flight and constant wind match analytical positions", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    const auto h = runtime.Start({2}, Ribbon());
    REQUIRE_FALSE(h.IsNull());
    for (int i = 1; i <= 60; ++i)
    {
        REQUIRE(runtime.SetPose(h, Pose(i / 6.0)));
        runtime.Advance(1.0f / 60);
    }
    const auto &state = Emitter(runtime,h).ribbon;
    REQUIRE(state.points.size() == 21);
    for (const auto &point : state.points)
    {
        CHECK(std::abs(double(point.position.Meters().x) - 10 * point.birthTime) < 0.0001);
        CHECK(std::abs(double(point.position.Meters().y) - 2 * (state.time - point.birthTime)) < 0.0001);
        CHECK(point.width >= 0.15f);
        CHECK(point.color.a <= 0.5f);
        CHECK(std::abs(point.textureDistance - 10 * point.birthTime) < 0.0001);
    }
}
TEST_CASE("CPU ribbons use maximum interval at rest and bounded oldest-first eviction", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    const auto h = runtime.Start({2}, Ribbon(4,3));
    for (int i = 0; i < 60; ++i) runtime.Advance(1.0f / 60);
    const auto &state = Emitter(runtime,h).ribbon;
    REQUIRE(state.points.size() == 3);
    CHECK(runtime.Stats().evictedRibbonPoints == 2);
    CHECK(state.points.front().link == 2);
    CHECK(state.points.back().birthTime - state.points.front().birthTime < 0.501);
}
TEST_CASE("CPU ribbon teleports, activation gaps and pause resumes break connections", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    const auto h = runtime.Start({2}, Ribbon());
    runtime.Advance(1.0f / 60);
    const auto first = Emitter(runtime,h).ribbon.points.back().ribbon;
    REQUIRE(runtime.SetPose(h, Pose(1000)));
    runtime.Advance(1.0f / 60);
    CHECK(Emitter(runtime,h).ribbon.points.back().ribbon != first);
    auto parameters = runtime.Find(h)->parameters;
    parameters.spawnRateScale = 0;
    REQUIRE(runtime.SetParameters(h, parameters));
    runtime.Advance(1.0f / 60);
    const auto before = Emitter(runtime,h).ribbon.points.back().ribbon;
    parameters.spawnRateScale = 1;
    REQUIRE(runtime.SetParameters(h, parameters));
    runtime.Advance(1.0f / 60);
    CHECK(Emitter(runtime,h).ribbon.points.back().ribbon != before);
    const auto active = Emitter(runtime,h).ribbon.points.back().ribbon;
    runtime.Pause(h,true);
    runtime.SetPose(h,Pose(2000));
    runtime.Advance(1);
    runtime.Pause(h,false);
    runtime.Advance(1.0f / 60);
    CHECK(Emitter(runtime,h).ribbon.points.back().ribbon != active);
    CHECK(Emitter(runtime,h).spawns == 1);
}
TEST_CASE("Removed ribbon owners drain while points continue to drift and fade", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    const mir::Entity owner{10,1};
    const auto h = runtime.Start({2}, Ribbon(0.5f), {}, owner);
    runtime.Advance(1.0f / 60);
    const auto old = Emitter(runtime,h).ribbon.points.front();
    runtime.Detach(owner);
    for (int i = 0; i < 6; ++i) runtime.Advance(1.0f / 60);
    const auto &now = Emitter(runtime,h).ribbon.points.front();
    CHECK(double(now.position.Meters().y - old.position.Meters().y) > 0.1999);
    CHECK(now.color.a < old.color.a);
    CHECK(now.width > old.width);
    for (int i = 0; i < 30; ++i) runtime.Advance(1.0f / 60);
    CHECK_FALSE(runtime.Find(h));
}
TEST_CASE("Remote CPU ribbon histories preserve local precision across system tile movement", "[particles][ribbon]")
{
    if constexpr (sizeof(mir::WorldReal) == sizeof(float))
    {
        mir::ParticleRuntime runtime;
        CHECK(runtime.Start({2},Ribbon(),Pose(1000000)).IsNull());
        return;
    }
    mir::ParticleRuntime nearRuntime, farRuntime;
    auto effect = Ribbon(120);
    effect.emitterDefinitions[0].teleportDistance = 65536;
    effect.emitterDefinitions[0].spawnPerMeter = 0;
    const auto near = nearRuntime.Start({2},effect);
    const auto far = farRuntime.Start({2},effect,Pose(1000000));
    for (int i = 1; i <= 120; ++i)
    {
        nearRuntime.SetPose(near,Pose(i));
        farRuntime.SetPose(far,Pose(1000000+i));
        nearRuntime.Advance(1.0f/60);
        farRuntime.Advance(1.0f/60);
    }
    const auto &a = Emitter(nearRuntime,near).ribbon.points;
    const auto &b = Emitter(farRuntime,far).ribbon.points;
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        CHECK(glm::length(b[i].position.Meters() - a[i].position.Meters() - mir::WorldVec3(1000000,0,0)) < 0.01);
    // Move the aircraft a million metres onward, old history remains in its original coordinates.
    for (int i = 1; i <= 20; ++i)
    {
        farRuntime.SetPose(far,Pose(1000120 + i*50000));
        farRuntime.Advance(1.0f/60);
    }
    CHECK(std::abs(double(Emitter(farRuntime,far).ribbon.points.front().position.Meters().x) - 1000000) < 0.01);
}
TEST_CASE("CPU ribbon schemas reject incomplete stacks and invalid interval parameters", "[particles][ribbon]")
{
    auto emitter = Ribbon().emitterDefinitions[0];
    std::string error;
    REQUIRE(mir::ValidateParticleEmitterAsset(emitter,error));
    const auto bytes = mir::BuildParticleEmitterAsset(emitter);
    REQUIRE(mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(bytes)).mEmitter);
    emitter.spawnBehaviors[0].parameters[0] = 0;
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter,error));
    emitter = Ribbon().emitterDefinitions[0];
    emitter.updateBehaviors.erase(emitter.updateBehaviors.begin());
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter,error));
}

TEST_CASE("CPU ribbons share the bounded birth budget and never replay overflow", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    auto effect = Ribbon();
    effect.emitterDefinitions[0].spawnPerMeter = 65536;
    effect.emitterDefinitions[0].teleportDistance = 100;
    const auto h = runtime.Start({2},effect);
    REQUIRE(runtime.SetPose(h,Pose(10)));
    runtime.Advance(1.0f/60);
    CHECK(Emitter(runtime,h).spawns == mir::kParticleSpawnBudget);
    CHECK(Emitter(runtime,h).ribbon.points.size() == 256);
    CHECK(runtime.Stats().droppedBudgetSpawns > 600000);
    runtime.Advance(1.0f/60);
    CHECK(Emitter(runtime,h).spawns == 1);
}
