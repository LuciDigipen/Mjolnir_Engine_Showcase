/**
 * @file ParticleRibbon.tests.cpp
 * @author sumin.park
 * @brief Analytical ribbon motion, activation discontinuities and bounded retirement.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles/ParticleRuntime.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>

namespace
{
    const std::vector<mir::ParticleBehavior> kDefaultUpdate{{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidth", {3}}, {"RibbonFade", {}}};
    mir::ParticleEffect Ribbon(float lifetime = 4, uint32_t capacity = 256, std::vector<mir::ParticleBehavior> update = kDefaultUpdate)
    {
        mir::ParticleEmitterAsset emitter;
        emitter.simulationTarget = mir::ParticleSimulationTarget::CPU;
        emitter.spawnRate = 0;
        emitter.spawnPerMeter = 2;
        emitter.lifetime = lifetime;
        emitter.capacity = capacity;
        emitter.texture = {1};
        emitter.spawnBehaviors = {{"RibbonSpawn", {0.25f}}};
        emitter.updateBehaviors = std::move(update);
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

TEST_CASE("CPU ribbon appearance for every accepted stack shape is unchanged", "[particles][ribbon]")
{
    struct Expected { float width; glm::vec4 color; };
    struct Shape { std::vector<mir::ParticleBehavior> update; Expected oldest, middle, newest; };
    const std::vector<float> gradient{1, 0.5f, 0.25f, 1, 0.5f, 1, 1, 0.5f};
    const std::vector<Shape> shapes{
        { {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidth", {3}}, {"RibbonFade", {}}},
            {0.225f, {1, 0.4f, 0.1f, 0.375f}}, {0.1875f, {1, 0.4f, 0.1f, 0.4375f}}, {0.15f, {1, 0.4f, 0.1f, 0.5f}} },
        { {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidthCurve", {0.5f, 2, 1}}, {"RibbonFade", {}}},
            {0.1875f, {1, 0.4f, 0.1f, 0.375f}}, {0.13125f, {1, 0.4f, 0.1f, 0.4375f}}, {0.075f, {1, 0.4f, 0.1f, 0.5f}} },
        { {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidth", {3}}, {"RibbonColorGradient", gradient}, {"RibbonFade", {}}},
            {0.225f, {0.875f, 0.25f, 0.04375f, 0.328125f}}, {0.1875f, {0.9375f, 0.225f, 0.034375f, 0.41015625f}},
            {0.15f, {1, 0.2f, 0.025f, 0.5f}} },
        { {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidthCurve", {0.5f, 2, 1}}, {"RibbonColorGradient", gradient}, {"RibbonFade", {}}},
            {0.1875f, {0.875f, 0.25f, 0.04375f, 0.328125f}}, {0.13125f, {0.9375f, 0.225f, 0.034375f, 0.41015625f}},
            {0.075f, {1, 0.2f, 0.025f, 0.5f}} } };
    for (const auto &shape : shapes)
    {
        mir::ParticleRuntime runtime;
        const auto h = runtime.Start({2}, Ribbon(4, 256, shape.update));
        for (int i = 1; i <= 60; ++i)
        {
            REQUIRE(runtime.SetPose(h, Pose(i / 6.0)));
            runtime.Advance(1.0f / 60);
        }
        const auto &points = Emitter(runtime, h).ribbon.points;
        REQUIRE(points.size() == 21);
        const Expected *expected[]{ &shape.oldest, &shape.middle, &shape.newest };
        const size_t indices[]{ 0, 10, 20 };
        for (int k = 0; k < 3; ++k)
        {
            const auto &point = points[indices[k]];
            CHECK(point.width == Catch::Approx(expected[k]->width).margin(1e-6));
            for (int c = 0; c < 4; ++c) CHECK(point.color[c] == Catch::Approx(expected[k]->color[c]).margin(1e-6));
        }
    }
}

namespace
{
    std::vector<mir::ParticleRibbonPoint> Fly(std::vector<mir::ParticleBehavior> update)
    {
        mir::ParticleRuntime runtime;
        const auto h = runtime.Start({2}, Ribbon(4, 256, std::move(update)));
        REQUIRE_FALSE(h.IsNull());
        for (int i = 1; i <= 60; ++i)
        {
            REQUIRE(runtime.SetPose(h, Pose(i / 6.0)));
            runtime.Advance(1.0f / 60);
        }
        return Emitter(runtime, h).ribbon.points;
    }
    const mir::ParticleBehavior kAge{"RibbonAge", {}}, kWind{"RibbonWind", {}}, kFade{"RibbonFade", {}};
    const std::vector<float> kGradient{1, 0.5f, 0.25f, 1, 0.5f, 1, 1, 0.5f};
}
TEST_CASE("Optional CPU ribbon behaviors fall back to the emitter values when omitted", "[particles][ribbon]")
{
    const auto noWind = Fly({kAge, {"RibbonWidth", {3}}, kFade});
    const auto noFade = Fly({kAge, kWind, {"RibbonWidth", {3}}});
    const auto noWidth = Fly({kAge, kWind, kFade});
    REQUIRE(noWind.size() == 21);
    REQUIRE(noFade.size() == 21);
    REQUIRE(noWidth.size() == 21);
    for (const auto &point : noWind) CHECK(std::abs(point.position.Meters().y) < 0.0001);
    for (const auto &point : noFade) CHECK(point.color.a == Catch::Approx(0.5f));
    for (const auto &point : noWidth) CHECK(point.width == Catch::Approx(0.15f));
}
TEST_CASE("CPU ribbon behaviors that own different attributes can be reordered", "[particles][ribbon]")
{
    const auto canonical = Fly({kAge, kWind, {"RibbonWidth", {3}}, kFade});
    const auto shuffled = Fly({kFade, {"RibbonWidth", {3}}, kWind, kAge});
    REQUIRE(canonical.size() == shuffled.size());
    for (size_t i = 0; i < canonical.size(); ++i)
    {
        CHECK(shuffled[i].width == Catch::Approx(canonical[i].width));
        CHECK(shuffled[i].color.a == Catch::Approx(canonical[i].color.a));
        CHECK(shuffled[i].position.Meters().y == Catch::Approx(canonical[i].position.Meters().y));
    }
}
TEST_CASE("CPU ribbon stacks require spawn and age and reject duplicates", "[particles][ribbon]")
{
    std::string error;
    auto emitter = Ribbon().emitterDefinitions[0];
    emitter.spawnBehaviors.clear();
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
    CHECK(error.find("RibbonSpawn") != std::string::npos);
    emitter = Ribbon().emitterDefinitions[0];
    emitter.updateBehaviors = {kWind, kFade};
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
    CHECK(error.find("RibbonAge") != std::string::npos);
    emitter.updateBehaviors = {kAge};
    CHECK(mir::ValidateParticleEmitterAsset(emitter, error));
    emitter.updateBehaviors = {kAge, kAge};
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
    CHECK(error.find("duplicate") != std::string::npos);
    emitter.updateBehaviors = {kAge, {"Integrate", {}}};
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter, error));
}

TEST_CASE("New ribbon authoring combines gradient alpha with fade regardless of stored order", "[particles][ribbon][authoring]")
{
    auto effect = Ribbon(4, 256, {kFade, {"RibbonColorGradient", kGradient}, kAge});
    mir::ParticleRuntime runtime;
    const auto handle = runtime.Start({2}, effect);
    REQUIRE_FALSE(handle.IsNull());
    for (int i = 1; i <= 60; ++i)
    {
        REQUIRE(runtime.SetPose(handle, Pose(i / 6.0)));
        runtime.Advance(1.0f / 60);
    }
    const auto &points = Emitter(runtime, handle).ribbon.points;
    REQUIRE(points.size() == 21);
    CHECK(points.front().color.a == Catch::Approx(0.328125f).margin(1e-6));
}
