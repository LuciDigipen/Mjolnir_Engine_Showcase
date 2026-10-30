/**
 * @file ParticleAuthoring.tests.cpp
 * @author sumin.park
 * @brief Source authoring, curve evaluation, preview isolation and resource policies.
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
    mir::ParticleEffect Effect(bool ribbon = false)
    {
        mir::ParticleEmitterAsset emitter;
        emitter.texture = {1}; emitter.capacity = 32; emitter.spawnRate = 60; emitter.lifetime = 1;
        if (ribbon)
        {
            emitter.simulationTarget = mir::ParticleSimulationTarget::CPU; emitter.spawnRate = 0; emitter.spawnPerMeter = 1;
            emitter.spawnBehaviors = {{"RibbonSpawn",{0.25f}}};
            emitter.updateBehaviors = {{"RibbonAge",{}},{"RibbonWind",{}},{"RibbonWidthCurve",{1,3,5}},
                {"RibbonColorGradient",{1,0,0,1,0,1,1,1}},{"RibbonFade",{}}};
        }
        mir::ParticleEffect effect; effect.emitters = {{1}}; effect.emitterDefinitions = {emitter};
        return effect;
    }
    const mir::ParticleEmitterInstance &Emitter(const mir::ParticleRuntime &runtime, mir::ParticleHandle handle)
    { return *runtime.FindEmitter(runtime.Find(handle)->emitters[0]); }
}
TEST_CASE("Particle authoring source writers preserve curves gradients renderer and quality", "[particles][authoring]")
{
    auto effect = Effect();
    auto emitter = effect.emitterDefinitions[0];
    emitter.sourceVersion = 2;
    emitter.updateBehaviors = {{"Age",{}},{"SizeCurve",{1,3,5}},{"ColorGradient",{1,0.4f,0,1,0.2f,0.3f,1,0}}};
    emitter.quality = {0.7f,10,100,0.25f}; emitter.renderer.lit = true;
    const auto source = mir::WriteParticleEmitterSource(emitter,"textures/smoke.png");
    const auto parsed = mir::ReadParticleEmitterSource(source,"assets");
    REQUIRE(parsed.mEmitter);
    CHECK(parsed.mEmitter->updateBehaviors[2].parameters.size()==8);
    CHECK(parsed.mEmitter->quality.minSpawnScale==0.25f);
    CHECK(parsed.mEmitter->renderer.lit);
    const auto blob = mir::BuildParticleEmitterAsset(*parsed.mEmitter);
    const auto cooked = mir::ReadParticleEmitterAsset(*mir::ParseAssetBlob(blob));
    REQUIRE(cooked.mEmitter);
    CHECK(cooked.mEmitter->quality.importance==0.7f);
    CHECK(cooked.mEmitter->updateBehaviors[1].name=="SizeCurve");
    const auto script=mir::GenerateParticleScript(emitter);
    CHECK(script.find("a.color = p.color * lerp(")!=std::string::npos);
    CHECK(script.find("a.identity = uint4(instance, p.range.z, birth.sequence, 1u)")!=std::string::npos);
    const std::vector<std::string> paths{"particles/exhaust.emitter.json"};
    const auto system=mir::ReadParticleEffectSource(mir::WriteParticleEffectSource(effect,paths),"assets");
    REQUIRE(system.mEffect);
    CHECK(system.mEmitterPaths==paths);
    emitter.updateBehaviors[2].parameters[7]=2;
    std::string error;
    CHECK_FALSE(mir::ValidateParticleEmitterAsset(emitter,error));
    CHECK(mir::EvaluateParticleCurve(1,3,5,0.25f)==2);
    CHECK(mir::EvaluateParticleCurve(1,3,5,0.75f)==4);
    CHECK(mir::EvaluateParticleCurve(1,3,5,2)==5);
}
TEST_CASE("Preview ticks do not spawn or age game instances and pause freezes preview", "[particles][preview]")
{
    mir::ParticleRuntime runtime;
    const auto game = runtime.Start({2},Effect());
    const auto preview = runtime.Start({2},Effect(),{},mir::kNullEntity,nullptr,true);
    REQUIRE_FALSE(preview.IsNull());
    runtime.Advance(1.0f/60,true);
    CHECK(Emitter(runtime,game).seconds==0);
    CHECK(Emitter(runtime,game).spawns==0);
    CHECK(Emitter(runtime,preview).spawns==1);
    runtime.Pause(preview,true); runtime.Advance(1.0f/60,true);
    CHECK(Emitter(runtime,preview).spawns==0);
    CHECK(Emitter(runtime,preview).seconds==0);
    runtime.Clear();
    CHECK_FALSE(runtime.Find(preview));
}
TEST_CASE("Particle admission caps reject excess resources without disturbing live instances", "[particles][budgets]")
{
    auto budgets = mir::ParticleBudgets{}; budgets.emitters=1; budgets.gpuParticles=32;
    mir::ParticleRuntime runtime(4,budgets);
    const auto first=runtime.Start({2},Effect()); REQUIRE_FALSE(first.IsNull());
    CHECK(Emitter(runtime,first).births.capacity()>=budgets.births);
    CHECK(runtime.Start({2},Effect()).IsNull());
    CHECK(runtime.Stats().rejectedCapacityStarts==1);
    CHECK(runtime.Find(first));
    CHECK_FALSE(runtime.ConfigureBudgets({}));
    runtime.Clear();
    budgets.emitters=64;
    REQUIRE(runtime.ConfigureBudgets(budgets));
    const auto gpu=runtime.Start({2},Effect()); REQUIRE_FALSE(gpu.IsNull());
    CHECK(runtime.Start({2},Effect()).IsNull());
    CHECK(runtime.Stats().rejectedCapacityStarts==1);
    runtime.Clear();
    budgets.emitters=64; budgets.ribbonPoints=16;
    REQUIRE(runtime.ConfigureBudgets(budgets));
    CHECK(runtime.Start({2},Effect(true)).IsNull());
    budgets.ribbonPoints=65536; budgets.cpuBytes=65536;
    REQUIRE(runtime.ConfigureBudgets(budgets));
    CHECK(runtime.Start({2},Effect()).IsNull());
    CHECK(runtime.Stats().rejectedMemoryStarts>0);
    CHECK(runtime.Stats().cpuHighWaterBytes>=runtime.Stats().cpuBytes);
    mir::ParticleRuntime single(1);
    REQUIRE_FALSE(single.Start({2},Effect()).IsNull());
    CHECK(single.Start({2},Effect()).IsNull());
    CHECK(single.Stats().rejectedCapacityStarts==1);
    mir::ParticleRuntime trails;
    const auto trail=trails.Start({2},Effect(true)); REQUIRE_FALSE(trail.IsNull());
    CHECK(Emitter(trails,trail).ribbon.points.capacity()>=32);
}
TEST_CASE("Importance and camera distance reduce births without freezing old ribbon history", "[particles][quality]")
{
    mir::ParticleRuntime runtime;
    auto effect=Effect(true); effect.parameters.wind={0,2,0};
    effect.emitterDefinitions[0].quality={1,1,2,0};
    const auto handle=runtime.Start({2},effect);
    runtime.SetQualityOrigin(mir::WorldPosition{});
    runtime.Advance(1.0f/60);
    const auto old=Emitter(runtime,handle).ribbon.points.front();
    runtime.SetQualityOrigin(*mir::WorldPosition::FromMeters(mir::WorldVec3(1000,0,0)));
    for(int i=0;i<6;++i) runtime.Advance(1.0f/60);
    const auto &points=Emitter(runtime,handle).ribbon.points;
    REQUIRE(points.size()==1);
    CHECK(points[0].position.Meters().y>old.position.Meters().y+0.19);
    CHECK(points[0].color.a<old.color.a);
    CHECK(runtime.Stats().qualityReducedEmitters==1);
    runtime.SetQualityOrigin(mir::WorldPosition{}); runtime.Advance(1.0f/60);
    REQUIRE(Emitter(runtime,handle).ribbon.points.size()==2);
    CHECK(Emitter(runtime,handle).ribbon.points.back().ribbon!=old.ribbon);
    CHECK(std::isfinite(runtime.Stats().cpuMs));
}
TEST_CASE("Configured particle birth limits drop work without later catchup", "[particles][budgets]")
{
    auto budgets=mir::ParticleBudgets{}; budgets.births=2;
    mir::ParticleRuntime runtime(4,budgets);
    auto effect=Effect(); effect.emitterDefinitions[0].spawnRate=600;
    const auto handle=runtime.Start({2},effect);
    runtime.Advance(1.0f/60);
    CHECK(Emitter(runtime,handle).spawns==2);
    CHECK(runtime.Stats().droppedBudgetSpawns==8);
    effect.emitterDefinitions[0].spawnRate=0;
    runtime.Stop(handle); runtime.Advance(1.0f/60);
    CHECK(Emitter(runtime,handle).spawns==0);
}
TEST_CASE("Particle CPU appearance preserves curve and gradient keys while draining", "[particles][ribbon]")
{
    mir::ParticleRuntime runtime;
    const auto handle=runtime.Start({2},Effect(true));
    runtime.Advance(1.0f/60);runtime.Stop(handle);
    for(int i=0;i<29;++i) runtime.Advance(1.0f/60);
    const auto &point=Emitter(runtime,handle).ribbon.points.front();
    CHECK(point.width==Catch::Approx(0.45f));
    CHECK(point.color.r==Catch::Approx(0.5f));
    CHECK(point.color.g==Catch::Approx(0.2f));
    CHECK(point.color.b==Catch::Approx(0.05f));
    CHECK(point.color.a==Catch::Approx(0.25f));
}
