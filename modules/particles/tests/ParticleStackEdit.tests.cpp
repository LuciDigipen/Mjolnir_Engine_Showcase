/**
 * @file ParticleStackEdit.tests.cpp
 * @author sumin.park
 * @brief Editor stack edits on a ribbon emitter, through save, cook, load and playback.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles/ParticleRuntime.h>
#include <particles/ParticleStackEdit.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    // The stack the user had saved before the second width entry appeared.
    mir::ParticleEmitterAsset RibbonEmitter()
    {
        mir::ParticleEmitterAsset emitter;
        emitter.sourceVersion = 2; // preserve the original stack-editing characterization
        emitter.simulationTarget = mir::ParticleSimulationTarget::CPU;
        emitter.spawnRate = 0;
        emitter.spawnPerMeter = 2;
        emitter.lifetime = 4;
        emitter.capacity = 512;
        emitter.texture = {1};
        emitter.spawnBehaviors = {{"RibbonSpawn", {0.25f}}};
        emitter.updateBehaviors = {{"RibbonAge", {}}, {"RibbonWind", {}}, {"RibbonWidthCurve", {23.0f, 5.08f, 15.3f}}, {"RibbonFade", {}}};
        return emitter;
    }
    // What the editor does on "Save emitter + recook" and what the runtime then loads.
    mir::ParticleEmitterAsset SaveAndLoad(const mir::ParticleEmitterAsset &emitter)
    {
        const auto source = mir::WriteParticleEmitterSource(emitter, "textures/x.png");
        const auto read = mir::ReadParticleEmitterSource(source, "assets");
        REQUIRE(read.mEmitter);
        const auto bytes = mir::BuildParticleEmitterAsset(*read.mEmitter);
        const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(bytes));
        REQUIRE(cooked.mEmitter);
        return *cooked.mEmitter;
    }
    // Plays four seconds of straight flight, so the ribbon holds points of every age.
    std::vector<mir::ParticleRibbonPoint> Play(const mir::ParticleEmitterAsset &emitter, double &now)
    {
        mir::ParticleEffect effect;
        effect.emitters = {{1}};
        effect.emitterDefinitions = {emitter};
        mir::ParticleRuntime runtime;
        const auto handle = runtime.Start({2}, effect);
        REQUIRE_FALSE(handle.IsNull());
        for (int i = 1; i <= 240; ++i)
        {
            mir::WorldPose pose;
            pose.position = *mir::WorldPosition::FromMeters(mir::WorldVec3(i / 6.0, 0, 0));
            REQUIRE(runtime.SetPose(handle, pose));
            runtime.Advance(1.0f / 60);
        }
        const auto &state = runtime.FindEmitter(runtime.Find(handle)->emitters[0])->ribbon;
        now = state.time;
        return state.points;
    }
    void CheckCurve(const mir::ParticleEmitterAsset &emitter, float start, float middle, float end)
    {
        double now = 0;
        const auto points = Play(SaveAndLoad(emitter), now);
        REQUIRE(points.size() > 10);
        for (const auto &point : points)
        {
            const float age = static_cast<float>((now - point.birthTime) / emitter.lifetime);
            CHECK(point.width == Catch::Approx(0.15f * mir::EvaluateParticleCurve(start, middle, end, age)).margin(1e-4));
        }
    }
}
TEST_CASE("A saved width curve reaches the playing ribbon", "[particles][stackedit]")
{
    CheckCurve(RibbonEmitter(), 23.0f, 5.08f, 15.3f);
}
TEST_CASE("Adding a second width curve overrides the first, and removing it restores the first", "[particles][stackedit]")
{
    auto emitter = RibbonEmitter();
    REQUIRE(mir::AddParticleBehavior(emitter, false, "RibbonWidthCurve"));
    REQUIRE(emitter.updateBehaviors.back().name == "RibbonWidthCurve");
    CHECK(emitter.updateBehaviors.back().parameters == std::vector<float>{1, 1, 1});
    CheckCurve(emitter, 1, 1, 1);
    REQUIRE(mir::RemoveParticleBehavior(emitter, false, emitter.updateBehaviors.size() - 1));
    CheckCurve(emitter, 23.0f, 5.08f, 15.3f);
}
TEST_CASE("Editing a curve parameter in place changes the cooked width", "[particles][stackedit]")
{
    auto emitter = RibbonEmitter();
    emitter.updateBehaviors[2].parameters = {2, 8, 3};
    CheckCurve(emitter, 2, 8, 3);
}
TEST_CASE("Reordering moves the override: a constant width placed last beats the curve before it", "[particles][stackedit]")
{
    auto emitter = RibbonEmitter();
    REQUIRE(mir::AddParticleBehavior(emitter, false, "RibbonWidth"));
    // RibbonWidth defaults to an end scale of 1, a flat width equal to the emitter size.
    CheckCurve(emitter, 1, 1, 1);
    REQUIRE(mir::MoveParticleBehavior(emitter, false, emitter.updateBehaviors.size() - 1, -1));
    REQUIRE(mir::MoveParticleBehavior(emitter, false, 3, -1));
    CHECK(emitter.updateBehaviors[2].name == "RibbonWidth");
    CHECK(emitter.updateBehaviors[3].name == "RibbonWidthCurve");
    CheckCurve(emitter, 23.0f, 5.08f, 15.3f);
}
TEST_CASE("Required ribbon behaviors keep their last copy and everything else can go", "[particles][stackedit]")
{
    auto emitter = RibbonEmitter();
    CHECK_FALSE(mir::CanRemoveParticleBehavior(emitter, true, 0));
    CHECK_FALSE(mir::RemoveParticleBehavior(emitter, true, 0));
    CHECK_FALSE(mir::CanRemoveParticleBehavior(emitter, false, 0));
    REQUIRE(mir::AddParticleBehavior(emitter, false, "RibbonAge"));
    CHECK(mir::CanRemoveParticleBehavior(emitter, false, 0));
    REQUIRE(mir::RemoveParticleBehavior(emitter, false, 0));
    CHECK_FALSE(mir::CanRemoveParticleBehavior(emitter, false, emitter.updateBehaviors.size() - 1));
    CHECK(mir::CanRemoveParticleBehavior(emitter, false, 1));
    for (size_t i = emitter.updateBehaviors.size(); i-- > 0;)
        if (emitter.updateBehaviors[i].name != "RibbonAge") REQUIRE(mir::RemoveParticleBehavior(emitter, false, i));
    CHECK(emitter.updateBehaviors.size() == 1);
    std::string error;
    CHECK(mir::ValidateParticleEmitterAsset(emitter, error));
}
TEST_CASE("Stack edits reject unknown names, wrong-stage names, a full stack and bad indices", "[particles][stackedit]")
{
    auto emitter = RibbonEmitter();
    CHECK_FALSE(mir::AddParticleBehavior(emitter, false, "Integrate"));
    CHECK_FALSE(mir::AddParticleBehavior(emitter, true, "RibbonAge"));
    CHECK_FALSE(mir::AddParticleBehavior(emitter, false, "Nope"));
    while (emitter.updateBehaviors.size() < 32) REQUIRE(mir::AddParticleBehavior(emitter, false, "RibbonWind"));
    CHECK_FALSE(mir::AddParticleBehavior(emitter, false, "RibbonWind"));
    CHECK_FALSE(mir::RemoveParticleBehavior(emitter, false, 99));
    CHECK_FALSE(mir::MoveParticleBehavior(emitter, false, 0, -1));
    CHECK_FALSE(mir::MoveParticleBehavior(emitter, false, 31, 1));
    CHECK_FALSE(mir::MoveParticleBehavior(emitter, false, 99, 1));
}

TEST_CASE("New authoring keeps one behavior per kind and remembers inactive size mode", "[particles][stackedit][authoring]")
{
    auto emitter = RibbonEmitter();
    emitter.sourceVersion = 3;
    CHECK_FALSE(mir::AddParticleBehavior(emitter, false, "RibbonWidthCurve"));
    REQUIRE(mir::AddParticleBehavior(emitter, false, "RibbonWidth"));
    CHECK_FALSE(emitter.updateBehaviors[2].enabled);
    CHECK(emitter.updateBehaviors.back().enabled);
    CHECK_FALSE(mir::MoveParticleBehavior(emitter, false, 2, 1));
    CHECK_FALSE(mir::RemoveParticleBehavior(emitter, true, 0));
    REQUIRE(mir::SetParticleBehaviorEnabled(emitter, false, 2, true));
    CHECK(emitter.updateBehaviors[2].enabled);
    CHECK_FALSE(emitter.updateBehaviors.back().enabled);
    CHECK(emitter.updateBehaviors[2].parameters == std::vector<float>{23.0f, 5.08f, 15.3f});
    std::string error;
    CHECK(mir::ValidateParticleEmitterAsset(emitter, error));
}

TEST_CASE("Legacy emitter upgrade refuses duplicates and preserves its original on failure", "[particles][stackedit][authoring]")
{
    auto emitter = RibbonEmitter();
    emitter.updateBehaviors.push_back({"RibbonAge", {}});
    const auto original = emitter.updateBehaviors;
    std::string message;
    CHECK_FALSE(mir::UpgradeParticleEmitter(emitter, message));
    CHECK(emitter.sourceVersion == 2);
    CHECK(emitter.updateBehaviors.size() == original.size());
    CHECK(message.find("duplicate") != std::string::npos);
    emitter.updateBehaviors.pop_back();
    REQUIRE(mir::UpgradeParticleEmitter(emitter, message));
    CHECK(emitter.sourceVersion == 3);
    CHECK(message.empty());
}
