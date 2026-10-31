/**
 * @file ParticleLifecycle.tests.cpp
 * @author sumin.park
 * @brief Headless emitter ownership, scene persistence and reload transitions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleTestWorld.h"

#include <core/ecs/ComponentRegistry.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_CASE("Particle systems allocate in LateUpdate and stop pause reset have distinct lifetimes", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System();
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto first = test.Particles().Find(owner)->handle;
    REQUIRE(test.Particles().Stop(owner));
    REQUIRE(test.Particles().Pause(owner, true));
    for (int i = 0; i < 10; ++i) test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->state == mir::ParticleExecutionState::Inactive);
    REQUIRE(test.Particles().Pause(owner, false));
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    REQUIRE(test.Particles().Start(owner));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->handle != first);
    const auto second = test.Particles().Find(owner)->handle;
    REQUIRE(test.Particles().Reset(owner));
    CHECK_FALSE(test.Particles().Runtime().Find(second));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    REQUIRE(test.Particles().Destroy(owner));
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    const auto manual = test.System(false);
    test.Tick();
    CHECK_FALSE(test.Particles().Find(manual));
    test.Particles().Start(manual);
    test.Tick();
    REQUIRE(test.Particles().Find(manual));
}

TEST_CASE("Particle paused worlds freeze drains and removed owners release bounded instances", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System();
    test.Tick();
    const auto handle = test.Particles().Find(owner)->handle;
    test.Particles().Stop(owner);
    mir::SetPlayState(test.world, mir::PlayState::Paused);
    for (int i = 0; i < 10; ++i) test.Tick();
    REQUIRE(test.Particles().Runtime().Find(handle));
    test.Particles().Pause(owner, true);
    test.world.DestroyEntity(owner);
    REQUIRE(test.Particles().Runtime().Find(handle));
    CHECK(test.Particles().Runtime().Find(handle)->owner == mir::kNullEntity);
    mir::SetPlayState(test.world, mir::PlayState::Running);
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
}

TEST_CASE("Particle copied components cannot share handles and excess systems wait for capacity", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto first = test.System();
    test.Tick();
    const auto copy = *test.world.GetComponent<mir::ParticleSystem>(first);
    const auto second = test.world.CreateEntity();
    mir::AddTransform(test.world, second);
    test.world.AddComponent<mir::ParticleSystem>(second, copy);
    CHECK(test.world.GetComponent<mir::ParticleSystem>(second)->instance.IsNull());
    const auto third = test.System();
    test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 2);
    REQUIRE(test.Particles().Find(first));
    const auto firstHandle = test.Particles().Find(first)->handle;
    test.Particles().Destroy(first);
    for (int i = 0; i < 3; ++i) test.Tick();
    REQUIRE(test.Particles().Find(second));
    REQUIRE(test.Particles().Find(third));
    CHECK(test.Particles().Find(second)->handle != firstHandle);
    CHECK(test.Particles().Find(third)->handle != firstHandle);
}

TEST_CASE("Particle cleared effect references release instances and component removal drains", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System();
    test.Tick();
    const auto first = test.Particles().Find(owner)->handle;
    auto &emitter = *test.world.GetComponent<mir::ParticleSystem>(owner);
    emitter.systemName[0] = '\0';
    test.Tick();
    CHECK_FALSE(test.Particles().Runtime().Find(first));
    CHECK(test.Particles().Runtime().Stats().active == 0);
    std::strcpy(emitter.systemName, "effects/exhaust.particle.json");
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto second = test.Particles().Find(owner)->handle;
    test.world.RemoveComponent<mir::ParticleSystem>(owner);
    REQUIRE(test.Particles().Runtime().Find(second));
    CHECK(test.Particles().Runtime().Find(second)->owner.IsNull());
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
}

TEST_CASE("Particle reload stages preserve old state on abort and reset bindings on commit", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System();
    test.Tick();
    const auto old = test.Particles().Find(owner)->handle;

    test.files.Publish(0.1f, false);
    test.assets.BeginReload();
    CHECK_THROWS(test.StageEvent(mir::Stage::ReloadPrepare));
    test.StageEvent(mir::Stage::ReloadAbort);
    test.assets.AbortReload();
    REQUIRE(test.Particles().Runtime().Find(old));
    CHECK(test.Particles().Find(owner)->effect.emitterDefinitions[0].lifetime == 0.05f);

    test.files.Publish(0.1f);
    test.assets.BeginReload();
    REQUIRE_NOTHROW(test.StageEvent(mir::Stage::ReloadPrepare));
    REQUIRE(test.Particles().Runtime().Find(old));
    test.Particles().Pause(owner, true);
    test.assets.CommitReload();
    test.StageEvent(mir::Stage::ReloadCommit);
    CHECK_FALSE(test.Particles().Runtime().Find(old));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->paused);
    CHECK(test.Particles().Find(owner)->effect.emitterDefinitions[0].lifetime == 0.1f);
    test.Particles().Stop(owner);
    test.files.Publish(0.2f);
    test.assets.BeginReload();
    test.StageEvent(mir::Stage::ReloadPrepare);
    test.assets.CommitReload();
    test.StageEvent(mir::Stage::ReloadCommit);
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
}

TEST_CASE("Particle save load and repeated play scene transitions never serialize live handles", "[particles]")
{
    mir::test::ParticleTestWorld test;
    auto scene = mir::NewScene("particle lifecycle");
    auto owner = test.System();
    scene.mEntities.emplace(mir::AllocateStableId(scene), owner);
    const auto dir = test.files.Root() / "scene";
    test.Tick();
    const auto old = test.Particles().Find(owner)->handle;
    REQUIRE(mir::SaveScene(test.world, dir, scene));
    mir::UnloadScene(test.world, scene);
    test.StageEvent(mir::Stage::SceneUnload);
    CHECK(test.Particles().Runtime().Stats().active == 0);
    CHECK_FALSE(test.Particles().Runtime().Find(old));
    scene = mir::LoadScene(test.world, dir);
    REQUIRE(scene.mEntities.size() == 1);
    owner = scene.mEntities.begin()->second;
    test.StageEvent(mir::Stage::SceneLoad);
    const auto *emitter = test.world.GetComponent<mir::ParticleSystem>(owner);
    REQUIRE(emitter);
    CHECK(emitter->instance.IsNull());
    CHECK(std::string(emitter->systemName) == "effects/exhaust.particle.json");

    const auto *ops = mir::ComponentRegistry::Instance().Find("ParticleSystem");
    REQUIRE(ops);
    CHECK(ops->mFields.size() == 6 + mir::kMaxSystemEmitters);
    CHECK(ops->FindField("emitterOffset7") != nullptr);
    CHECK(ops->FindField("emitterOffset8") == nullptr);
    CHECK(ops->FindField("wind") != nullptr);
    CHECK(ops->FindField("spawnRateScale") != nullptr);
    CHECK(ops->FindField("sizeScale") != nullptr);
    CHECK(ops->FindField("effectName") == nullptr);
    CHECK(mir::ComponentRegistry::Instance().Find("ParticleEmitter") == nullptr);
    CHECK(ops->FindField("instance") == nullptr);
    for (int i = 0; i < 10; ++i)
    {
        test.StageEvent(mir::Stage::PlayBegin);
        test.Tick();
        REQUIRE(test.Particles().Find(owner));
        CHECK(test.Particles().Runtime().Stats().active == 1);
        test.StageEvent(mir::Stage::PlayEnd);
        CHECK(test.Particles().Runtime().Stats().active == 0);
        CHECK(test.world.GetComponent<mir::ParticleSystem>(owner)->instance.IsNull());
    }
}

TEST_CASE("Particle system user overrides persist and update only the selected instance", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.System();
    auto &component = *test.world.GetComponent<mir::ParticleSystem>(owner);
    component.overrideParameters = true;
    component.userParameters = {0.5f, 2};
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto handle = test.Particles().Find(owner)->handle;
    const auto emitter = test.Particles().Find(owner)->emitters[0];
    CHECK(test.Particles().Runtime().FindEmitter(emitter)->spawns == 0);
    test.Tick();
    CHECK(test.Particles().Runtime().FindEmitter(emitter)->spawns == 1);
    CHECK(test.Particles().Find(owner)->parameters.sizeScale == 2);
    CHECK(test.Particles().Find(owner)->effect.parameters.spawnRateScale == 1);
    const auto other = test.System();
    test.Tick();
    REQUIRE(test.Particles().Find(other));
    CHECK(test.Particles().Find(other)->parameters.spawnRateScale == 1);
    REQUIRE(test.Particles().SetParameters(owner, {2, 3}));
    test.Tick();
    CHECK(test.Particles().Find(owner)->handle == handle);
    CHECK(test.Particles().Runtime().FindEmitter(emitter)->spawns == 2);
    CHECK(test.Particles().Find(other)->parameters.sizeScale == 1);
    CHECK_FALSE(test.Particles().SetParameters(owner, {-1, 1}));
    CHECK(test.Particles().Find(owner)->parameters.spawnRateScale == 2);

    auto scene = mir::NewScene("particle overrides");
    scene.mEntities.emplace(mir::AllocateStableId(scene), owner);
    const auto dir = test.files.Root() / "overrides-scene";
    REQUIRE(mir::SaveScene(test.world, dir, scene));
    mir::UnloadScene(test.world, scene);
    test.StageEvent(mir::Stage::SceneUnload);
    scene = mir::LoadScene(test.world, dir);
    REQUIRE(scene.mEntities.size() == 1);
    const auto loaded = scene.mEntities.begin()->second;
    const auto *saved = test.world.GetComponent<mir::ParticleSystem>(loaded);
    REQUIRE(saved);
    CHECK(saved->instance.IsNull());
    CHECK(saved->overrideParameters);
    CHECK(saved->userParameters.spawnRateScale == 2);
    CHECK(saved->userParameters.sizeScale == 3);
    test.StageEvent(mir::Stage::SceneLoad);
    test.Tick();
    REQUIRE(test.Particles().Find(loaded));
    CHECK(test.Particles().Find(loaded)->parameters.spawnRateScale == 2);
    CHECK(test.Particles().Find(loaded)->parameters.sizeScale == 3);
}

TEST_CASE("Particle world resolves two emitter dependencies and retires both after owner removal", "[particles]")
{
    mir::test::ParticleTestWorld test;
    auto definition = test.files.Definition();
    const auto secondId = mir::MakeAssetId("assets/effects/secondary.emitter.json");
    auto second = definition.emitterDefinitions[0];
    second.spawnRate = 30;
    second.lifetime = 0.2f;
    definition.emitters.push_back(secondId);
    definition.emitterDefinitions.push_back(second);
    test.files.Write("effect.blob", mir::BuildParticleEffectAsset(definition));
    test.files.Write("secondary.blob", mir::BuildParticleEmitterAsset(second));
    const mir::AssetManifestSourceEntry entries[]{
        {{mir::AssetId{901}, mir::kParticleScriptTypeTag, mir::kParticleScriptContentVersion}, "script.blob"},
        {{mir::AssetId{902}, mir::kParticleScriptLayoutTypeTag, mir::kParticleScriptLayoutContentVersion}, "layout.blob"},
        {{test.files.EffectId(), mir::kParticleEffectTypeTag, mir::kParticleEffectContentVersion}, "effect.blob"},
        {{test.files.EmitterId(), mir::kParticleEmitterTypeTag, mir::kParticleEmitterContentVersion}, "emitter.blob"},
        {{secondId, mir::kParticleEmitterTypeTag, mir::kParticleEmitterContentVersion}, "secondary.blob"},
        {{test.files.TextureId(), mir::kTextureAssetTypeTag, 1}, "texture.blob"}};
    test.files.Write(mir::kAssetManifestFileName, mir::BuildAssetManifestBlob(entries));
    test.assets.BeginReload();
    test.StageEvent(mir::Stage::ReloadPrepare);
    test.assets.CommitReload();
    test.StageEvent(mir::Stage::ReloadCommit);
    const auto owner = test.System();
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto handle = test.Particles().Find(owner)->handle;
    REQUIRE(test.Particles().Find(owner)->emitters.size() == 2);
    const auto firstEmitter = test.Particles().Find(owner)->emitters[0];
    const auto secondEmitter = test.Particles().Find(owner)->emitters[1];
    CHECK(firstEmitter != secondEmitter);
    CHECK(test.Particles().Runtime().Stats().activeEmitters == 2);
    CHECK(test.Particles().Runtime().FindEmitter(firstEmitter)->spawns == 1);
    CHECK(test.Particles().Runtime().FindEmitter(secondEmitter)->spawns == 0);
    test.world.DestroyEntity(owner);
    for (int i = 0; i < 3; ++i) test.Tick();
    REQUIRE(test.Particles().Runtime().Find(handle));
    CHECK(test.Particles().Runtime().FindEmitter(firstEmitter)->state == mir::ParticleExecutionState::Complete);
    for (int i = 0; i < 10; ++i) test.Tick();
    CHECK_FALSE(test.Particles().Runtime().Find(handle));
    CHECK_FALSE(test.Particles().Runtime().FindEmitter(firstEmitter));
    CHECK_FALSE(test.Particles().Runtime().FindEmitter(secondEmitter));
    CHECK(test.Particles().Runtime().Stats().activeEmitters == 0);
}

TEST_CASE("Inactive particle systems reconcile reflected user overrides while draining", "[particles]")
{
    mir::test::ParticleTestWorld test;
    test.files.Publish(1.0f);
    const auto owner = test.System();
    test.Tick();
    REQUIRE(test.Particles().Stop(owner));
    auto &component = *test.world.GetComponent<mir::ParticleSystem>(owner);
    component.overrideParameters = true;
    component.userParameters = {0.5f, 2};
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->parameters.sizeScale == 2);
    component.overrideParameters = false;
    test.Tick();
    CHECK(test.Particles().Find(owner)->parameters.sizeScale == 1);
}

TEST_CASE("Particle emitter offsets are hidden persisted fields that move only their own instance", "[particles]")
{
    int hidden = 0;
    for (const auto &field : mir::kParticleSystemFields)
        if (field.mName.starts_with("emitterOffset"))
        {
            CHECK(field.mHidden);
            CHECK(field.mKind == mir::FieldKind::Vec3);
            CHECK(field.mOffset == offsetof(mir::ParticleSystem, emitterOffsets) + hidden * sizeof(glm::vec3));
            ++hidden;
        }
    CHECK(hidden == static_cast<int>(mir::kMaxSystemEmitters));

    mir::test::ParticleTestWorld test;
    test.files.Publish(1.0f);
    const auto moved = test.System();
    const auto still = test.System();
    test.world.GetComponent<mir::ParticleSystem>(moved)->emitterOffsets[0] = {3, 0, 0};
    test.Tick();
    REQUIRE(test.Particles().Find(moved));
    REQUIRE(test.Particles().Find(still));
    const auto position = [&](mir::Entity owner)
    {
        return test.Particles().Runtime().FindEmitter(test.Particles().Find(owner)->emitters[0])->attachmentPosition.Meters();
    };
    CHECK(position(moved).x - position(still).x == 3);

    auto scene = mir::NewScene("particle offsets");
    scene.mEntities.emplace(mir::AllocateStableId(scene), moved);
    const auto dir = test.files.Root() / "offsets-scene";
    REQUIRE(mir::SaveScene(test.world, dir, scene));
    mir::UnloadScene(test.world, scene);
    test.StageEvent(mir::Stage::SceneUnload);
    scene = mir::LoadScene(test.world, dir);
    REQUIRE(scene.mEntities.size() == 1);
    const auto *loaded = test.world.GetComponent<mir::ParticleSystem>(scene.mEntities.begin()->second);
    REQUIRE(loaded);
    CHECK(loaded->emitterOffsets[0] == glm::vec3(3, 0, 0));
    CHECK(loaded->emitterOffsets[1] == glm::vec3(0));
}
